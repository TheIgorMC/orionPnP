#!/usr/bin/env python3
"""
RS485 command sender GUI for the OrionPnP feeder board protocol.

Tkinter (stdlib - no extra dependency beyond pyserial) front end for the
same protocol logic rs485_sender.py's CLI/REPL uses (see
rs485_protocol.py). Connects to a USB-RS485 adapter on a COM port, shows
every frame that comes off the bus in a live log, and offers:

  - Address setup: Scan / Assign, including a one-click scan+assign
  - Feed & peel test: pitch setup, single feed/peel, and a timed
    feed+peel cycle test (the firmware doesn't couple the two yet)
  - Packet builder: per-command payload fields, expected reply, and a
    live byte-by-byte preview of the exact frame (CRC included)

The baud rate is pre-filled from the newest firmware's RS485_BAUD.

Usage:
    python rs485_gui.py
"""
import queue
import secrets
import statistics
import sys
import threading
import time
import tkinter as tk
from tkinter import ttk, scrolledtext, messagebox, filedialog

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    print("This tool needs pyserial: pip install pyserial", file=sys.stderr)
    sys.exit(1)

from rs485_protocol import (
    DEFAULT_BAUD,
    CMD_NAMES,
    COMMAND_SPECS,
    ERROR_CODES,
    cmd_name,
    build_frame,
    describe_frame,
    firmware_bauds,
    FrameAssembler,
)

POLL_INTERVAL_MS = 50
READ_CHUNK_TIMEOUT_S = 0.1  # how often the reader thread wakes up to check the stop flag

CMD_ACK = 0x82
CMD_NACK = 0x83
CMD_DISCOVER = 0x10
CMD_DISCOVER_HERE = 0x90
CMD_ASSIGN_ADDR = 0x11
CMD_GET_COMPONENT = 0x20
CMD_COMPONENT_INFO = 0xA0
CMD_SET_PITCH_MM = 0x25
CMD_FEED_NEXT = 0x26
CMD_PEEL = 0x34
CMD_ZERO_HERE = 0x24
CMD_GET_SERIAL = 0x33
CMD_SET_HW_INFO = 0x2A
CMD_GET_HW_INFO = 0x29
CMD_SET_PEEL_TIME = 0x35
CMD_GET_PEEL_TIME = 0x36
CMD_JOG = 0x37
CMD_I2C_SCAN = 0x38
CMD_SET_SERIAL = 0x39

# Reply timeouts. FEED_NEXT can legitimately take up to the firmware's
# MOVE_TIMEOUT_MS (6000) before it NACKs; PEEL replies only after the run.
DEFAULT_REPLY_TIMEOUT_S = 1.0
FEED_REPLY_TIMEOUT_S = 7.0
PEEL_REPLY_MARGIN_S = 1.5
PEEL_CAL_MAX_S = 5.0  # firmware caps the calibrated peel time at 5000 ms
JOG_STEPS_MM = [-4, -2, -1, -0.5, -0.2, 0.2, 0.5, 1, 2, 4]
DISCOVER_WINDOW_S = 0.6  # firmware jitters replies over 0-200ms

CYCLE_ORDERS = ["feed, then peel", "peel, then feed", "feed only", "peel only"]


class SerialLink:
    """Owns the open port + background reader thread. Every frame that
    arrives goes onto `inbox` (drained by the GUI thread) and to any
    request() currently waiting for a reply on a worker thread."""

    def __init__(self, port: str, baud: int, rts_tx: bool, inbox: queue.Queue):
        self.ser = serial.Serial(port, baud, timeout=READ_CHUNK_TIMEOUT_S)
        self.rts_tx = rts_tx
        self.inbox = inbox
        self._waiters = []
        self._waiters_lock = threading.Lock()
        self._tx_lock = threading.Lock()
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._reader_loop, daemon=True)
        self._thread.start()

    def _reader_loop(self):
        assembler = FrameAssembler()
        while not self._stop.is_set():
            try:
                chunk = self.ser.read(1)
            except serial.SerialException as exc:
                self.inbox.put(("error", str(exc)))
                return
            if not chunk:
                continue
            frame = assembler.feed(chunk[0])
            if frame is not None:
                self.inbox.put(("frame", frame))
                with self._waiters_lock:
                    for q in self._waiters:
                        q.put(frame)

    def send(self, addr: int, cmd: int, payload: bytes = b""):
        frame = build_frame(addr, cmd, payload)
        with self._tx_lock:
            if self.rts_tx:
                self.ser.setRTS(True)
            self.ser.write(frame)
            self.ser.flush()
            if self.rts_tx:
                time.sleep(0.002)
                self.ser.setRTS(False)
        return frame

    def request(self, addr, cmd, payload, accept, timeout_s, cancel: threading.Event = None, collect=False):
        """Send a frame, then wait for replies matching accept(frame).
        Returns (first match or None, elapsed seconds since the frame
        finished sending) - or, with collect=True, (every match within
        timeout_s, elapsed)."""
        q = queue.Queue()
        with self._waiters_lock:
            self._waiters.append(q)
        try:
            self.send(addr, cmd, payload)
            t0 = time.monotonic()
            deadline = t0 + timeout_s
            matches = []
            while True:
                remaining = deadline - time.monotonic()
                if remaining <= 0 or (cancel is not None and cancel.is_set()):
                    return (matches if collect else None), time.monotonic() - t0
                try:
                    f = q.get(timeout=min(remaining, 0.1))
                except queue.Empty:
                    continue
                if accept(f):
                    if not collect:
                        return f, time.monotonic() - t0
                    matches.append(f)
        finally:
            with self._waiters_lock:
                self._waiters.remove(q)

    def close(self):
        self._stop.set()
        self._thread.join(timeout=1.0)
        try:
            self.ser.close()
        except serial.SerialException:
            pass


def _parse_hex_token(t: str) -> int:
    """Payload bytes are always hex here (the field is labeled as such) -
    an optional 0x/0X prefix is accepted but not required, so '0A' and
    '0x0A' both work (plain int(t, 0) would reject '0A' - no 0x prefix,
    and a leading zero isn't valid octal either)."""
    t = t.strip()
    if t[:2].lower() == "0x":
        t = t[2:]
    return int(t, 16) & 0xFF


def _parse_hex_payload(text: str) -> bytes:
    return bytes(_parse_hex_token(t) for t in text.replace(",", " ").split())


# (code, display name) pairs for the packet builder dropdown, sorted by code
CMD_CHOICES = [(code, f"0x{code:02X} {name}") for code, name in sorted(CMD_NAMES.items())
               if code < 0x80]  # requests only - replies are never something you'd send


class Aborted(Exception):
    pass


class App:
    def __init__(self, root: tk.Tk):
        self.root = root
        self.root.title("OrionPnP Feeder RS485 Sender")
        self.link: SerialLink | None = None
        self.inbox = queue.Queue()  # reader thread frames + worker log lines, in arrival order
        self.worker: threading.Thread | None = None
        self.abort = threading.Event()
        self.firmwares = firmware_bauds()

        self._build_widgets()
        self._refresh_ports()
        self.root.protocol("WM_DELETE_WINDOW", self._on_close)
        self._poll_inbox()

    # ---------------------------
    # Layout
    # ---------------------------
    def _build_widgets(self):
        pad = {"padx": 4, "pady": 4}
        self.root.columnconfigure(0, weight=1)

        # --- Connection ---
        conn = ttk.LabelFrame(self.root, text="Connection")
        conn.grid(row=0, column=0, sticky="ew", **pad)

        ttk.Label(conn, text="Port:").grid(row=0, column=0, **pad)
        self.port_var = tk.StringVar()
        self.port_combo = ttk.Combobox(conn, textvariable=self.port_var, width=12, state="readonly")
        self.port_combo.grid(row=0, column=1, **pad)
        ttk.Button(conn, text="Refresh", command=self._refresh_ports).grid(row=0, column=2, **pad)

        ttk.Label(conn, text="Firmware:").grid(row=0, column=3, **pad)
        self.fw_var = tk.StringVar()
        fw_labels = [f"{name} ({baud} baud)" for name, baud in self.firmwares] or ["(none found)"]
        self.fw_combo = ttk.Combobox(conn, textvariable=self.fw_var, values=fw_labels, width=20, state="readonly")
        self.fw_combo.current(0)
        self.fw_combo.grid(row=0, column=4, **pad)
        self.fw_combo.bind("<<ComboboxSelected>>", self._on_fw_choice)

        ttk.Label(conn, text="Baud:").grid(row=0, column=5, **pad)
        self.baud_var = tk.StringVar(value=str(self.firmwares[0][1] if self.firmwares else DEFAULT_BAUD))
        ttk.Entry(conn, textvariable=self.baud_var, width=8).grid(row=0, column=6, **pad)

        self.rts_var = tk.BooleanVar(value=False)
        ttk.Checkbutton(conn, text="RTS controls TX (DE)", variable=self.rts_var).grid(row=0, column=7, sticky="w", **pad)

        self.connect_btn = ttk.Button(conn, text="Connect", command=self._toggle_connect)
        self.connect_btn.grid(row=0, column=8, **pad)

        self.status_var = tk.StringVar(value="Disconnected")
        self.status_label = ttk.Label(conn, textvariable=self.status_var, foreground="red")
        self.status_label.grid(row=0, column=9, **pad)

        # --- Target + always-available actions ---
        target = ttk.Frame(self.root)
        target.grid(row=1, column=0, sticky="ew", **pad)
        ttk.Label(target, text="Target addr:").grid(row=0, column=0, **pad)
        self.addr_var = tk.StringVar(value="1")
        ttk.Entry(target, textvariable=self.addr_var, width=6).grid(row=0, column=1, **pad)
        ttk.Button(target, text="Ping", command=lambda: self._quick(0x01)).grid(row=0, column=2, **pad)
        ttk.Button(target, text="Get Status", command=lambda: self._quick(0x30)).grid(row=0, column=3, **pad)
        ttk.Button(target, text="Identify", command=lambda: self._quick(0x32, bytes([0]))).grid(row=0, column=4, **pad)
        ttk.Button(target, text="Stop motors", command=lambda: self._quick(0x31)).grid(row=0, column=5, **pad)
        self.busy_var = tk.StringVar()
        ttk.Label(target, textvariable=self.busy_var, foreground="#c64600").grid(row=0, column=6, **pad)
        self.abort_btn = ttk.Button(target, text="Abort sequence", command=self._abort, state="disabled")
        self.abort_btn.grid(row=0, column=7, **pad)

        # --- Tabs ---
        tabs = ttk.Notebook(self.root)
        tabs.grid(row=2, column=0, sticky="ew", **pad)
        tabs.add(self._build_setup_tab(tabs), text="Address setup")
        tabs.add(self._build_jog_tab(tabs), text="Jog & zero")
        tabs.add(self._build_feed_tab(tabs), text="Feed & peel test")
        tabs.add(self._build_eeprom_tab(tabs), text="EEPROM / serial")
        tabs.add(self._build_builder_tab(tabs), text="Packet builder")

        # --- Log ---
        log_frame = ttk.LabelFrame(self.root, text="Log")
        log_frame.grid(row=3, column=0, sticky="nsew", **pad)
        self.root.rowconfigure(3, weight=1)
        log_frame.rowconfigure(0, weight=1)
        log_frame.columnconfigure(0, weight=1)

        self.log = scrolledtext.ScrolledText(log_frame, height=16, state="disabled", wrap="word")
        self.log.grid(row=0, column=0, columnspan=4, sticky="nsew", **pad)
        self.log.tag_configure("tx", foreground="#1a5fb4")
        self.log.tag_configure("rx", foreground="#26a269")
        self.log.tag_configure("err", foreground="#c01c28")
        self.log.tag_configure("info", foreground="gray40")
        self.log.tag_configure("result", foreground="#813d9c")

        self.verbose_var = tk.BooleanVar(value=False)
        self.verbose = False  # plain copy - worker threads must not touch Tk variables
        self.verbose_var.trace_add("write", lambda *_: setattr(self, "verbose", self.verbose_var.get()))
        ttk.Checkbutton(log_frame, text="Show raw TX bytes", variable=self.verbose_var).grid(row=1, column=0, sticky="w", **pad)
        ttk.Button(log_frame, text="Clear", command=self._clear_log).grid(row=1, column=1, **pad)
        ttk.Button(log_frame, text="Save...", command=self._save_log).grid(row=1, column=2, **pad)

        self.addr_var.trace_add("write", lambda *_: self._update_preview())
        self._on_cmd_choice()

    def _build_setup_tab(self, parent):
        pad = {"padx": 4, "pady": 4}
        tab = ttk.Frame(parent)
        ttk.Label(tab, foreground="gray25", text=(
            "Feeders boot unassigned (addr 0) every power-up. Scan, then Assign the nonce a new address. "
            "Scan + assign does both when exactly one unassigned feeder answers, and sets Target addr.")
        ).grid(row=0, column=0, columnspan=7, sticky="w", **pad)

        ttk.Button(tab, text="Scan (CMD_DISCOVER)", command=lambda: self._quick(CMD_DISCOVER, addr=0x00)).grid(row=1, column=0, **pad)
        ttk.Label(tab, text="Nonce:").grid(row=1, column=1, **pad)
        self.nonce_var = tk.StringVar(value="0x0000")
        ttk.Entry(tab, textvariable=self.nonce_var, width=8).grid(row=1, column=2, **pad)
        ttk.Label(tab, text="New addr:").grid(row=1, column=3, **pad)
        self.new_addr_var = tk.StringVar(value="1")
        ttk.Entry(tab, textvariable=self.new_addr_var, width=6).grid(row=1, column=4, **pad)
        ttk.Button(tab, text="Assign", command=self._do_assign).grid(row=1, column=5, **pad)
        ttk.Button(tab, text="Scan + assign", command=self._do_scan_assign).grid(row=1, column=6, **pad)
        ttk.Label(tab, foreground="gray40", text="(Nonce fills in automatically from the latest Scan reply.)").grid(
            row=2, column=0, columnspan=7, sticky="w", **pad)

        row = ttk.Frame(tab)
        row.grid(row=3, column=0, columnspan=7, sticky="w")
        ttk.Button(row, text="Get component/config", command=lambda: self._quick(CMD_GET_COMPONENT)).grid(row=0, column=0, **pad)
        return tab

    def _build_jog_tab(self, parent):
        pad = {"padx": 4, "pady": 4}
        tab = ttk.Frame(parent)
        ttk.Label(tab, foreground="gray25", wraplength=760, justify="left", text=(
            "Seat the sprocket hole for the reel's first pocket, then Set zero here. Needs v0.02 firmware. "
            "4 mm = one tooth, 2 mm = half a tooth (the finest feed pitch). Jog moves are relative to where "
            "the wheel is; each reply shows the new raw angle.")
        ).grid(row=0, column=0, columnspan=12, sticky="w", **pad)

        for i, mm in enumerate(JOG_STEPS_MM):
            label = f"{mm:+g} mm"
            ttk.Button(tab, text=label, width=8, command=lambda m=mm: self._jog(m)).grid(row=1, column=i, **pad)

        ttk.Label(tab, text="Custom (mm):").grid(row=2, column=0, columnspan=2, sticky="e", **pad)
        self.jog_custom_var = tk.StringVar(value="0.5")
        ttk.Entry(tab, textvariable=self.jog_custom_var, width=8).grid(row=2, column=2, columnspan=2, sticky="w", **pad)
        ttk.Button(tab, text="Jog", command=self._do_jog_custom).grid(row=2, column=4, columnspan=2, **pad)
        ttk.Label(tab, foreground="gray40", text="signed, 0.1 mm resolution, up to 160").grid(
            row=2, column=6, columnspan=6, sticky="w", **pad)

        ttk.Separator(tab).grid(row=3, column=0, columnspan=12, sticky="ew", pady=6)
        ttk.Button(tab, text="Set zero here", command=self._do_zero_here).grid(row=4, column=0, columnspan=2, **pad)
        ttk.Button(tab, text="Get status", command=lambda: self._quick(0x30)).grid(row=4, column=2, columnspan=2, **pad)
        ttk.Button(tab, text="Feed once", command=lambda: self._start_cycle(1, "feed only")).grid(row=4, column=4, columnspan=2, **pad)
        ttk.Label(tab, foreground="gray40", wraplength=420, justify="left", text=(
            "Set zero here stores the current wheel angle as the tape zero (CMD_ZERO_HERE). "
            "Changing the component id clears it. Flipping the feed direction mirrors the angle, "
            "so set the direction first.")
        ).grid(row=4, column=6, columnspan=6, sticky="w", **pad)
        return tab

    def _build_feed_tab(self, parent):
        pad = {"padx": 4, "pady": 4}
        tab = ttk.Frame(parent)
        ttk.Label(tab, foreground="#c64600", wraplength=760, justify="left", text=(
            "v0.02 and earlier do not couple feed and peel: CMD_FEED_NEXT only turns the sprocket (motor A). "
            "v0.02b+ does once a peel rate is set (CMD_SET_PEEL_RATE) - then use 'feed only' below, or the peel runs twice. "
            "The cycle test below runs them back-to-back from the PC, waiting for each ACK. They can't overlap: "
            "the feeder doesn't listen to the bus while a motor runs.")
        ).grid(row=0, column=0, columnspan=8, sticky="w", **pad)

        # Setup
        ttk.Label(tab, text="Pitch (mm):").grid(row=1, column=0, sticky="e", **pad)
        self.pitch_var = tk.StringVar(value="4")
        ttk.Combobox(tab, textvariable=self.pitch_var, width=6, values=["2", "4", "8", "12", "16", "20", "24"]).grid(row=1, column=1, sticky="w", **pad)
        ttk.Button(tab, text="Set pitch", command=self._do_set_pitch).grid(row=1, column=2, **pad)
        ttk.Button(tab, text="Read config", command=lambda: self._quick(CMD_GET_COMPONENT)).grid(row=1, column=3, **pad)
        ttk.Button(tab, text="Feed once", command=lambda: self._start_cycle(1, "feed only")).grid(row=1, column=4, **pad)

        # Peel
        ttk.Label(tab, text="Peel dir:").grid(row=2, column=0, sticky="e", **pad)
        self.peel_dir_var = tk.StringVar(value="fwd")
        ttk.Combobox(tab, textvariable=self.peel_dir_var, width=6, state="readonly", values=["fwd", "rev"]).grid(row=2, column=1, sticky="w", **pad)
        ttk.Label(tab, text="Peel time (ms):").grid(row=2, column=2, sticky="e", **pad)
        self.peel_ms_var = tk.StringVar(value="1570")
        ttk.Spinbox(tab, textvariable=self.peel_ms_var, from_=10, to=5000, increment=10, width=7).grid(row=2, column=3, sticky="w", **pad)
        ttk.Button(tab, text="Peel once", command=lambda: self._start_cycle(1, "peel only")).grid(row=2, column=4, **pad)
        ttk.Label(tab, foreground="gray40", text="10ms steps; one-off runs up to 2550ms, saved time up to 5000ms").grid(
            row=2, column=5, columnspan=3, sticky="w", **pad)

        # Per-feeder calibration (v0.02)
        cal = ttk.LabelFrame(tab, text="This feeder's calibrated peel time (v0.02 firmware)")
        cal.grid(row=3, column=0, columnspan=8, sticky="ew", **pad)
        ttk.Button(cal, text="Save time above to feeder", command=self._do_save_peel_cal).grid(row=0, column=0, **pad)
        ttk.Button(cal, text="Read from feeder", command=lambda: self._quick(CMD_GET_PEEL_TIME)).grid(row=0, column=1, **pad)
        ttk.Button(cal, text="Run calibrated peel (fwd)", command=lambda: self._do_peel_cal_run(0)).grid(row=0, column=2, **pad)
        ttk.Button(cal, text="(rev)", command=lambda: self._do_peel_cal_run(1)).grid(row=0, column=3, **pad)
        self.peel_use_cal_var = tk.BooleanVar(value=False)
        ttk.Checkbutton(cal, text="Peel steps below use the feeder's saved time", variable=self.peel_use_cal_var).grid(
            row=1, column=0, columnspan=4, sticky="w", **pad)

        # Cycle test
        ttk.Separator(tab).grid(row=4, column=0, columnspan=8, sticky="ew", pady=6)
        ttk.Label(tab, text="Cycle:").grid(row=5, column=0, sticky="e", **pad)
        self.order_var = tk.StringVar(value=CYCLE_ORDERS[0])
        ttk.Combobox(tab, textvariable=self.order_var, width=16, state="readonly", values=CYCLE_ORDERS).grid(row=5, column=1, columnspan=2, sticky="w", **pad)
        ttk.Label(tab, text="Cycles:").grid(row=5, column=3, sticky="e", **pad)
        self.cycles_var = tk.StringVar(value="5")
        ttk.Spinbox(tab, textvariable=self.cycles_var, from_=1, to=1000, width=6).grid(row=5, column=4, sticky="w", **pad)
        ttk.Label(tab, text="Pause between (ms):").grid(row=5, column=5, sticky="e", **pad)
        self.pause_var = tk.StringVar(value="300")
        ttk.Spinbox(tab, textvariable=self.pause_var, from_=0, to=10000, increment=100, width=7).grid(row=5, column=6, sticky="w", **pad)
        ttk.Button(tab, text="Run cycle test", command=lambda: self._start_cycle(None, None)).grid(row=5, column=7, **pad)

        ttk.Label(tab, foreground="gray40", wraplength=760, justify="left", text=(
            "Each step logs its round-trip time. FEED time is the real sprocket move; PEEL time is the duration "
            "you asked for. Tune the peel time until the cover tape stays taut over several cycles without "
            "lifting the next pocket, then save it to the feeder.")
        ).grid(row=6, column=0, columnspan=8, sticky="w", **pad)
        return tab

    def _build_eeprom_tab(self, parent):
        pad = {"padx": 4, "pady": 4}
        tab = ttk.Frame(parent)
        ttk.Label(tab, foreground="gray25", wraplength=760, justify="left", text=(
            "If Get serial answers NACK ERR_I2C but everything else works, the EEPROM is probably a plain AT24C02: "
            "it has no factory serial page, so there is nothing to read. I2C scan tells you which it is. On a plain "
            "AT24C02 you can program your own serial (v0.02 firmware). An AT24CS02's factory serial is read-only.")
        ).grid(row=0, column=0, columnspan=6, sticky="w", **pad)

        ttk.Button(tab, text="I2C scan", command=lambda: self._quick(CMD_I2C_SCAN)).grid(row=1, column=0, **pad)
        ttk.Button(tab, text="Get serial", command=lambda: self._quick(CMD_GET_SERIAL)).grid(row=1, column=1, **pad)
        ttk.Button(tab, text="Get HW info", command=lambda: self._quick(CMD_GET_HW_INFO)).grid(row=1, column=2, **pad)

        ttk.Label(tab, text="Serial (32 hex):").grid(row=2, column=0, sticky="e", **pad)
        self.serial_var = tk.StringVar()
        ttk.Entry(tab, textvariable=self.serial_var, width=40, font=("Consolas", 10)).grid(row=2, column=1, columnspan=3, sticky="w", **pad)
        ttk.Button(tab, text="Random", command=lambda: self.serial_var.set(secrets.token_hex(16).upper())).grid(row=2, column=4, **pad)
        ttk.Button(tab, text="Program serial", command=self._do_program_serial).grid(row=2, column=5, **pad)
        ttk.Label(tab, foreground="gray40", text="Write it down or keep the log: it is the feeder's identity.").grid(
            row=3, column=1, columnspan=5, sticky="w", **pad)

        ttk.Label(tab, text="Tape width (mm):").grid(row=4, column=0, sticky="e", **pad)
        self.width_var = tk.StringVar(value="8")
        ttk.Combobox(tab, textvariable=self.width_var, width=6, state="readonly",
                     values=["8", "12", "16", "24", "32", "44", "56"]).grid(row=4, column=1, sticky="w", **pad)
        ttk.Button(tab, text="Write tape width", command=self._do_write_width).grid(row=4, column=2, columnspan=2, sticky="w", **pad)
        return tab

    def _build_builder_tab(self, parent):
        pad = {"padx": 4, "pady": 3}
        tab = ttk.Frame(parent)
        tab.columnconfigure(1, weight=1)

        ttk.Label(tab, text="Command:").grid(row=0, column=0, sticky="e", **pad)
        self.cmd_choice_var = tk.StringVar()
        self.cmd_combo = ttk.Combobox(tab, textvariable=self.cmd_choice_var, width=28, state="readonly",
                                      values=[label for _, label in CMD_CHOICES])
        self.cmd_combo.current(0)
        self.cmd_combo.grid(row=0, column=1, sticky="w", **pad)
        self.cmd_combo.bind("<<ComboboxSelected>>", self._on_cmd_choice)

        self.summary_var = tk.StringVar()
        ttk.Label(tab, textvariable=self.summary_var, wraplength=760, justify="left").grid(
            row=1, column=0, columnspan=3, sticky="w", **pad)

        self.fields_frame = ttk.Frame(tab)
        self.fields_frame.grid(row=2, column=0, columnspan=3, sticky="w", **pad)
        self.field_vars = []

        ttk.Label(tab, text="Payload (hex):").grid(row=3, column=0, sticky="e", **pad)
        self.payload_var = tk.StringVar()
        ttk.Entry(tab, textvariable=self.payload_var, width=40).grid(row=3, column=1, sticky="w", **pad)
        self.payload_var.trace_add("write", lambda *_: self._update_preview())

        self.reply_var = tk.StringVar()
        ttk.Label(tab, textvariable=self.reply_var, foreground="#26a269").grid(row=4, column=0, columnspan=3, sticky="w", **pad)
        self.notes_var = tk.StringVar()
        ttk.Label(tab, textvariable=self.notes_var, foreground="#c64600", wraplength=760, justify="left").grid(
            row=5, column=0, columnspan=3, sticky="w", **pad)

        self.preview_var = tk.StringVar()
        ttk.Label(tab, textvariable=self.preview_var, font=("Consolas", 10)).grid(row=6, column=0, columnspan=2, sticky="w", **pad)
        btns = ttk.Frame(tab)
        btns.grid(row=6, column=2, sticky="e")
        ttk.Button(btns, text="Copy hex", command=self._copy_frame).grid(row=0, column=0, **pad)
        ttk.Button(btns, text="Send", command=self._do_builder_send).grid(row=0, column=1, **pad)
        self._preview_frame = b""
        return tab

    # ---------------------------
    # Connection management
    # ---------------------------
    def _refresh_ports(self):
        ports = [p.device for p in serial.tools.list_ports.comports()]
        self.port_combo["values"] = ports
        if ports and not self.port_var.get():
            self.port_var.set(ports[0])

    def _on_fw_choice(self, _event=None):
        idx = self.fw_combo.current()
        if 0 <= idx < len(self.firmwares):
            self.baud_var.set(str(self.firmwares[idx][1]))

    def _toggle_connect(self):
        if self.link is None:
            self._connect()
        else:
            self._disconnect()

    def _connect(self):
        port = self.port_var.get()
        if not port:
            messagebox.showerror("No port", "Choose a COM port first.")
            return
        try:
            baud = int(self.baud_var.get(), 0)
        except ValueError:
            messagebox.showerror("Bad baud", f"'{self.baud_var.get()}' isn't a valid number.")
            return
        try:
            self.link = SerialLink(port, baud, self.rts_var.get(), self.inbox)
        except serial.SerialException as exc:
            messagebox.showerror("Connect failed", str(exc))
            return
        self.status_var.set(f"Connected ({port} @ {baud})")
        self.status_label.configure(foreground="#26a269")
        self.connect_btn.configure(text="Disconnect")
        self._log(f"Connected to {port} @ {baud} baud" + (" (RTS-controlled TX)" if self.rts_var.get() else ""), "info")

    def _disconnect(self):
        self.abort.set()
        if self.link is not None:
            self.link.close()
            self.link = None
        self.status_var.set("Disconnected")
        self.status_label.configure(foreground="red")
        self.connect_btn.configure(text="Connect")
        self._log("Disconnected.", "info")

    def _on_close(self):
        self._disconnect()
        self.root.destroy()

    # ---------------------------
    # Fire-and-forget sends (GUI thread) - replies just show up in the log
    # ---------------------------
    def _require_link(self) -> bool:
        if self.link is None:
            messagebox.showwarning("Not connected", "Connect to a port first.")
            return False
        if self.worker is not None and self.worker.is_alive():
            self._log("A sequence is running - wait for it or Abort first.", "err")
            return False
        return True

    def _tx_text(self, frame: bytes, addr: int, cmd: int, payload: bytes) -> str:
        if self.verbose:
            return f"TX: {frame.hex(' ')}  (addr=0x{addr:02X} cmd={cmd_name(cmd)})"
        return (f"TX  addr=0x{addr:02X} cmd={cmd_name(cmd)} "
                f"payload={payload.hex(' ') if payload else '(empty)'}")

    def _send(self, addr: int, cmd: int, payload: bytes = b""):
        if not self._require_link():
            return
        try:
            frame = self.link.send(addr, cmd, payload)
        except serial.SerialException as exc:
            self._log(f"Write failed: {exc}", "err")
            self._disconnect()
            return
        self._log(self._tx_text(frame, addr, cmd, payload), "tx")

    def _parse_addr(self, var: tk.StringVar, label: str):
        try:
            return int(var.get(), 0) & 0xFF
        except ValueError:
            self._log(f"Bad {label}: '{var.get()}'", "err")
            return None

    def _quick(self, cmd: int, payload: bytes = b"", addr: int = None):
        if addr is None:
            addr = self._parse_addr(self.addr_var, "target address")
            if addr is None:
                return
        self._send(addr, cmd, payload)

    # ---------------------------
    # Worker sequences - one at a time, wait for each reply
    # ---------------------------
    def _ui(self, fn):
        self.inbox.put(("call", fn))

    def _start_worker(self, label: str, fn):
        if not self._require_link():
            return
        self.abort.clear()
        self.busy_var.set(f"Running: {label}")
        self.abort_btn.configure(state="normal")

        def run():
            try:
                fn()
            except Aborted:
                self._log("Sequence aborted.", "err")
            except serial.SerialException as exc:
                self._log(f"Serial error: {exc}", "err")
            except Exception as exc:  # keep the GUI alive whatever a sequence trips over
                self._log(f"Sequence error: {exc!r}", "err")
            finally:
                self._ui(self._worker_done)

        self.worker = threading.Thread(target=run, daemon=True)
        self.worker.start()

    def _worker_done(self):
        self.busy_var.set("")
        self.abort_btn.configure(state="disabled")

    def _abort(self):
        self.abort.set()

    def _req(self, addr, cmd, payload=b"", timeout_s=DEFAULT_REPLY_TIMEOUT_S, expect=()):
        """Worker-thread request: send, wait for ACK/NACK (or a cmd in
        `expect`) from `addr`, log the round-trip. Returns (frame, ms)."""
        link = self.link
        if link is None or self.abort.is_set():
            raise Aborted()
        wanted = {CMD_ACK, CMD_NACK, *expect}
        self._log(self._tx_text(build_frame(addr, cmd, payload), addr, cmd, payload), "tx")
        frame, elapsed = link.request(addr, cmd, payload, lambda f: f.addr == addr and f.cmd in wanted,
                                      timeout_s, cancel=self.abort)
        if self.abort.is_set():
            raise Aborted()
        ms = elapsed * 1000.0
        if frame is None:
            self._log(f"   no reply to {cmd_name(cmd)} within {timeout_s:.1f}s", "err")
        elif frame.cmd == CMD_NACK:
            err = ERROR_CODES.get(frame.payload[0], f"0x{frame.payload[0]:02X}") if frame.payload else "(no code)"
            self._log(f"   {cmd_name(cmd)} NACK {err} after {ms:.0f} ms", "err")
        elif cmd == CMD_JOG and len(frame.payload) >= 2:
            raw = (frame.payload[0] << 8) | frame.payload[1]
            self._log(f"   jog done in {ms:.0f} ms: angle raw={raw} ({raw * 360.0 / 4096.0:.1f} deg)", "result")
        else:
            self._log(f"   {cmd_name(cmd)} -> {cmd_name(frame.cmd)} in {ms:.0f} ms", "result")
        return frame, ms

    # --- Address setup ---
    def _do_assign(self):
        try:
            nonce = int(self.nonce_var.get(), 0) & 0xFFFF
        except ValueError:
            self._log(f"Bad nonce: '{self.nonce_var.get()}'", "err")
            return
        new_addr = self._parse_addr(self.new_addr_var, "new address")
        if new_addr is None:
            return
        self._start_worker("assign", lambda: self._assign(nonce, new_addr))

    def _assign(self, nonce: int, new_addr: int) -> bool:
        payload = bytes([(nonce >> 8) & 0xFF, nonce & 0xFF, new_addr])
        link = self.link
        if link is None:
            raise Aborted()
        self._log(self._tx_text(build_frame(0, CMD_ASSIGN_ADDR, payload), 0, CMD_ASSIGN_ADDR, payload), "tx")
        # The ACK comes from the NEW address, so _req()'s same-addr filter doesn't fit here
        frame, _ = link.request(0x00, CMD_ASSIGN_ADDR, payload,
                                lambda f: f.addr == new_addr and f.cmd == CMD_ACK,
                                DEFAULT_REPLY_TIMEOUT_S, cancel=self.abort)
        if frame is None:
            self._log(f"   no ACK from 0x{new_addr:02X} - wrong nonce, feeder already assigned, "
                       f"or rebooted since the scan?", "err")
            return False
        self._log(f"   feeder 0x{nonce:04X} is now addr {new_addr} - Target addr set", "result")
        self._ui(lambda: self.addr_var.set(str(new_addr)))
        return True

    def _do_scan_assign(self):
        new_addr = self._parse_addr(self.new_addr_var, "new address")
        if new_addr is not None:
            self._start_worker("scan + assign", lambda: self._scan_assign(new_addr))

    def _scan_assign(self, new_addr: int):
        self._log(self._tx_text(build_frame(0, CMD_DISCOVER), 0, CMD_DISCOVER, b""), "tx")
        replies, _ = self.link.request(0x00, CMD_DISCOVER, b"", lambda f: f.cmd == CMD_DISCOVER_HERE,
                                       DISCOVER_WINDOW_S, cancel=self.abort, collect=True)
        if not replies:
            self._log("   nobody answered - no unassigned feeders. Already assigned? Try Ping on its address.", "err")
            return
        if len(replies) > 1:
            self._log(f"   {len(replies)} feeders answered - Assign them one at a time with the nonces above.", "err")
            return
        p = replies[0].payload
        self._assign((p[0] << 8) | p[1], new_addr)

    # --- Feed & peel ---
    def _do_set_pitch(self):
        try:
            mm = int(self.pitch_var.get(), 0)
            if not 1 <= mm <= 255:
                raise ValueError
        except ValueError:
            self._log(f"Bad pitch: '{self.pitch_var.get()}' (whole mm, 1-255)", "err")
            return
        addr = self._parse_addr(self.addr_var, "target address")
        if addr is None:
            return
        if mm % 2:
            self._log(f"Note: firmware rounds pitch to 2mm steps, {mm}mm won't be exact.", "info")

        def run():
            frame, _ = self._req(addr, CMD_SET_PITCH_MM, bytes([mm]))
            if frame is not None and frame.cmd == CMD_ACK:
                self._req(addr, CMD_GET_COMPONENT, expect=(CMD_COMPONENT_INFO,))  # read it back
        self._start_worker("set pitch", run)

    def _start_cycle(self, cycles, order):
        addr = self._parse_addr(self.addr_var, "target address")
        if addr is None:
            return
        order = order or self.order_var.get()
        use_cal = self.peel_use_cal_var.get()
        try:
            n = cycles or int(self.cycles_var.get())
            pause_s = int(self.pause_var.get()) / 1000.0
            if use_cal:
                peel_payload = COMMAND_SPECS[CMD_PEEL].encode([self.peel_dir_var.get(), ""])
                peel_timeout = PEEL_CAL_MAX_S + PEEL_REPLY_MARGIN_S
            else:
                peel_ms = int(self.peel_ms_var.get())
                peel_payload = COMMAND_SPECS[CMD_PEEL].encode([self.peel_dir_var.get(), peel_ms])
                peel_timeout = peel_payload[1] / 100.0 + PEEL_REPLY_MARGIN_S
                if peel_payload[1] * 10 != peel_ms and "peel" in order:
                    self._log(f"Peel time rounded to {peel_payload[1] * 10} ms (10ms steps).", "info")
        except ValueError as exc:
            self._log(f"Bad cycle settings: {exc}", "err")
            return
        steps = {"feed, then peel": ["feed", "peel"], "peel, then feed": ["peel", "feed"],
                 "feed only": ["feed"], "peel only": ["peel"]}[order]
        self._start_worker(order if n == 1 else f"{n}x {order}",
                           lambda: self._cycle(addr, n, steps, peel_payload, pause_s, peel_timeout))

    def _cycle(self, addr, n, steps, peel_payload, pause_s, peel_timeout):
        times = {"feed": [], "peel": []}
        try:
            for i in range(n):
                if n > 1:
                    self._log(f"-- cycle {i + 1}/{n}", "info")
                for step in steps:
                    if step == "feed":
                        frame, ms = self._req(addr, CMD_FEED_NEXT, timeout_s=FEED_REPLY_TIMEOUT_S)
                    else:
                        frame, ms = self._req(addr, CMD_PEEL, peel_payload, timeout_s=peel_timeout)
                    if frame is None or frame.cmd != CMD_ACK:
                        code = frame.payload[:1] if frame is not None else b""
                        if code == b"\x06" and step == "feed":
                            self._log("   ERR_NOT_READY: no pitch set - use Set pitch first.", "err")
                        elif code == b"\x06" and step == "peel":
                            self._log("   ERR_NOT_READY: no saved peel time - use Save time above to feeder first.", "err")
                        self._log("Stopping the test at the first failure.", "err")
                        return
                    times[step].append(ms)
                if i + 1 < n and pause_s > 0 and self.abort.wait(pause_s):
                    raise Aborted()
        finally:
            if n > 1:
                for step, t in times.items():
                    if t:
                        self._log(f"{step.upper()}: {len(t)} ok, avg {statistics.mean(t):.0f} ms, "
                                  f"min {min(t):.0f}, max {max(t):.0f}", "result")

    # --- Peel calibration ---
    def _do_save_peel_cal(self):
        addr = self._parse_addr(self.addr_var, "target address")
        if addr is None:
            return
        try:
            payload = COMMAND_SPECS[CMD_SET_PEEL_TIME].encode([self.peel_ms_var.get()])
        except ValueError as exc:
            self._log(f"Bad peel time: {exc}", "err")
            return

        def run():
            frame, _ = self._req(addr, CMD_SET_PEEL_TIME, payload)
            if frame is not None and frame.cmd == CMD_ACK:
                self._req(addr, CMD_GET_PEEL_TIME, expect=(0xA4,))  # read it back
        self._start_worker("save peel time", run)

    def _do_peel_cal_run(self, direction):
        addr = self._parse_addr(self.addr_var, "target address")
        if addr is not None:
            self._start_worker("calibrated peel", lambda: self._req(
                addr, CMD_PEEL, bytes([direction]), timeout_s=PEEL_CAL_MAX_S + PEEL_REPLY_MARGIN_S))

    # --- Jog / zero ---
    def _jog(self, mm):
        addr = self._parse_addr(self.addr_var, "target address")
        if addr is None:
            return
        try:
            payload = COMMAND_SPECS[CMD_JOG].encode([str(mm)])
        except ValueError as exc:
            self._log(f"Bad jog: {exc}", "err")
            return
        self._start_worker(f"jog {mm:+g} mm", lambda: self._req(
            addr, CMD_JOG, payload, timeout_s=FEED_REPLY_TIMEOUT_S))

    def _do_jog_custom(self):
        try:
            mm = float(self.jog_custom_var.get())
        except ValueError:
            self._log(f"Bad jog distance: '{self.jog_custom_var.get()}'", "err")
            return
        self._jog(mm)

    def _do_zero_here(self):
        addr = self._parse_addr(self.addr_var, "target address")
        if addr is not None:
            self._start_worker("set zero", lambda: self._req(addr, CMD_ZERO_HERE))

    # --- EEPROM ---
    def _do_program_serial(self):
        addr = self._parse_addr(self.addr_var, "target address")
        if addr is None:
            return
        try:
            payload = COMMAND_SPECS[CMD_SET_SERIAL].encode([self.serial_var.get()])
        except ValueError as exc:
            self._log(f"Bad serial: {exc}", "err")
            return

        def run():
            frame, _ = self._req(addr, CMD_SET_SERIAL, payload, timeout_s=2.0)
            if frame is None or frame.cmd != CMD_ACK:
                if frame is not None and frame.payload[:1] == b"\x08":
                    self._log("   This feeder has a factory serial (AT24CS02); it can't be overridden.", "err")
                elif frame is not None and frame.payload[:1] == b"\x07":
                    self._log("   EEPROM write/verify failed - run I2C scan to see what answers.", "err")
                return
            self._req(addr, CMD_GET_SERIAL, timeout_s=1.0, expect=(0xA3,))  # read it back
        self._start_worker("program serial", run)

    def _do_write_width(self):
        addr = self._parse_addr(self.addr_var, "target address")
        if addr is None:
            return
        payload = COMMAND_SPECS[CMD_SET_HW_INFO].encode([self.width_var.get()])
        self._start_worker("write tape width", lambda: self._req(addr, CMD_SET_HW_INFO, payload, timeout_s=2.0))

    # ---------------------------
    # Packet builder
    # ---------------------------
    def _current_cmd(self):
        idx = self.cmd_combo.current()
        return CMD_CHOICES[idx][0] if idx >= 0 else None

    def _on_cmd_choice(self, _event=None):
        code = self._current_cmd()
        if code is None:
            return
        spec = COMMAND_SPECS.get(code)
        for child in self.fields_frame.winfo_children():
            child.destroy()
        self.field_vars = []
        if spec is None:
            self.summary_var.set("")
            self.reply_var.set("")
            self.notes_var.set("")
            self.payload_var.set("")
            return
        self.summary_var.set(spec.summary + ("  [broadcast: always sent to addr 0x00]" if spec.broadcast else ""))
        self.reply_var.set(f"Expected reply: {spec.reply}")
        self.notes_var.set(spec.notes)
        if not spec.fields:
            ttk.Label(self.fields_frame, text="(no payload)", foreground="gray40").grid(row=0, column=0, padx=4)
        for r, field in enumerate(spec.fields):
            var = tk.StringVar(value=field.default)
            ttk.Label(self.fields_frame, text=f"{field.name}:").grid(row=r, column=0, sticky="e", padx=4, pady=2)
            if field.kind == "choice":
                w = ttk.Combobox(self.fields_frame, textvariable=var, width=12,
                                 values=[label for label, _ in field.choices])
            else:
                w = ttk.Entry(self.fields_frame, textvariable=var, width=12)
            w.grid(row=r, column=1, sticky="w", padx=4, pady=2)
            ttk.Label(self.fields_frame, text=field.help, foreground="gray40").grid(row=r, column=2, sticky="w", padx=4)
            var.trace_add("write", lambda *_: self._fields_to_payload())
            self.field_vars.append(var)
        self._fields_to_payload()

    def _fields_to_payload(self):
        spec = COMMAND_SPECS.get(self._current_cmd())
        if spec is None:
            return
        try:
            payload = spec.encode([v.get() for v in self.field_vars])
        except ValueError as exc:
            self._preview_frame = b""
            self.preview_var.set(f"Field error: {exc}")
            return
        self.payload_var.set(payload.hex(" ").upper())  # triggers _update_preview()

    def _builder_addr(self, spec):
        if spec is not None and spec.broadcast:
            return 0x00
        try:
            return int(self.addr_var.get(), 0) & 0xFF
        except ValueError:
            return None

    def _update_preview(self):
        if not hasattr(self, "preview_var"):
            return  # still building widgets
        code = self._current_cmd()
        spec = COMMAND_SPECS.get(code)
        addr = self._builder_addr(spec)
        try:
            payload = _parse_hex_payload(self.payload_var.get())
            if addr is None:
                raise ValueError(f"bad target addr '{self.addr_var.get()}'")
            self._preview_frame = build_frame(addr, code, payload)
        except ValueError as exc:
            self._preview_frame = b""
            self.preview_var.set(f"Can't build frame: {exc}")
            return
        self.preview_var.set(f"Frame: {self._preview_frame.hex(' ').upper()}\n"
                             f"       {describe_frame(self._preview_frame)}")

    def _copy_frame(self):
        if self._preview_frame:
            self.root.clipboard_clear()
            self.root.clipboard_append(self._preview_frame.hex(" ").upper())

    def _do_builder_send(self):
        if not self._preview_frame:
            self._log("Fix the frame first (see the preview line).", "err")
            return
        f = self._preview_frame
        self._send(f[1], f[2], f[4:-1])

    # ---------------------------
    # Log / inbox
    # ---------------------------
    def _log(self, text: str, tag: str = None):
        """Queued like everything else, so lines always appear in the order they happened."""
        self.inbox.put(("log", (text, tag)))

    def _write_log(self, text: str, tag: str = None):
        self.log.configure(state="normal")
        ts = time.strftime("%H:%M:%S")
        self.log.insert("end", f"[{ts}] {text}\n", tag or ())
        self.log.see("end")
        self.log.configure(state="disabled")

    def _clear_log(self):
        self.log.configure(state="normal")
        self.log.delete("1.0", "end")
        self.log.configure(state="disabled")

    def _save_log(self):
        path = filedialog.asksaveasfilename(defaultextension=".txt", filetypes=[("Text", "*.txt"), ("All files", "*.*")])
        if not path:
            return
        with open(path, "w", encoding="utf-8") as f:
            f.write(self.log.get("1.0", "end"))

    def _poll_inbox(self):
        try:
            while True:
                kind, item = self.inbox.get_nowait()
                if kind == "frame":
                    self._write_log(f"RX  {item}", "rx")
                    if item.cmd == CMD_DISCOVER_HERE and len(item.payload) >= 2:
                        self.nonce_var.set(f"0x{(item.payload[0] << 8) | item.payload[1]:04X}")
                elif kind == "log":
                    self._write_log(*item)
                elif kind == "call":
                    item()
                elif kind == "error":
                    self._write_log(f"Read error: {item}", "err")
                    if self.link is not None:
                        self._disconnect()
        except queue.Empty:
            pass
        self.root.after(POLL_INTERVAL_MS, self._poll_inbox)


def main():
    root = tk.Tk()
    App(root)
    root.mainloop()


if __name__ == "__main__":
    main()
