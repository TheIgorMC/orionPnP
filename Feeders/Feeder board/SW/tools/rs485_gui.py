#!/usr/bin/env python3
"""
RS485 command sender GUI for the OrionPnP feeder board protocol.

Tkinter (stdlib - no extra dependency beyond pyserial) front end for the
same protocol logic rs485_sender.py's CLI/REPL uses (see
rs485_protocol.py). Connects to a USB-RS485 adapter on a COM port and
offers:

  - A live "Feeder" card that stays visible on every tab: link health,
    magnet, driver fault, 12V current, wheel angle, and what is saved on
    the feeder (zero, pitch, tape width, peel time/rate). It polls the
    feeder once a second while nothing else is running.
  - Bring-up: a first-test checklist (connect, address, link, status,
    tape width, pitch, zero, feed, peel rate). Steps tick themselves from
    what the feeder reports, not just from what you pressed this session.
  - Feed & peel: pitch, peel time, peel rate (v0.02b: peel follows feed),
    a rate helper, and a timed cycle test.
  - Jog & zero, Setup (scan/assign, LED brightness, EEPROM/serial) and a
    packet builder with a live byte-by-byte preview (CRC included).
  - STOP is always visible (also Esc): aborts the running sequence and
    broadcasts CMD_STOP. A feeder only hears it between moves - it does not
    listen to the bus while a motor is running.

The baud rate is pre-filled from the newest firmware's RS485_BAUD. Port,
address and a few settings are remembered between runs.

Usage:
    python rs485_gui.py
"""
import json
import os
import queue
import secrets
import statistics
import struct
import sys
import threading
import time
import tkinter as tk
from tkinter import ttk, scrolledtext, messagebox, filedialog
from tkinter import font as tkfont

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    print("This tool needs pyserial: pip install pyserial", file=sys.stderr)
    sys.exit(1)

import tapjig_engine as tapjig
from rs485_protocol import (
    DEFAULT_BAUD,
    CMD_NAMES,
    COMMAND_SPECS,
    ERROR_CODES,
    IMON_DEFAULT_MA_PER_COUNT,
    cmd_name,
    build_frame,
    describe_frame,
    firmware_bauds,
    parse_status,
    FrameAssembler,
)

POLL_INTERVAL_MS = 50
STATUS_POLL_S = 1.0       # live-card refresh while idle
LINK_STALE_S = 3.5        # no reply for this long with auto-refresh on = link down
READ_CHUNK_TIMEOUT_S = 0.1  # how often the reader thread wakes up to check the stop flag
SETTINGS_PATH = os.path.join(os.path.expanduser("~"), ".orionpnp_rs485_gui.json")

CMD_PING = 0x01
CMD_PONG = 0x81
CMD_ACK = 0x82
CMD_NACK = 0x83
CMD_DISCOVER = 0x10
CMD_DISCOVER_HERE = 0x90
CMD_ASSIGN_ADDR = 0x11
CMD_GET_COMPONENT = 0x20
CMD_COMPONENT_INFO = 0xA0
CMD_SET_PITCH_MM = 0x25
CMD_FEED_NEXT = 0x26
CMD_ZERO_HERE = 0x24
CMD_GET_HW_INFO = 0x29
CMD_HW_INFO = 0xA1
CMD_SET_HW_INFO = 0x2A
CMD_GET_STATUS = 0x30
CMD_SET_EXT_LED = 0x27
CMD_STATUS_INFO = 0xA2
CMD_STOP = 0x31
CMD_IDENTIFY = 0x32
CMD_GET_SERIAL = 0x33
CMD_PEEL = 0x34
CMD_SET_PEEL_TIME = 0x35
CMD_GET_PEEL_TIME = 0x36
CMD_PEEL_TIME_INFO = 0xA4
CMD_JOG = 0x37
CMD_I2C_SCAN = 0x38
CMD_SET_SERIAL = 0x39
CMD_SET_LED_BRIGHTNESS = 0x3A
CMD_SET_PEEL_RATE = 0x3B
CMD_GET_PEEL_RATE = 0x3C
CMD_PEEL_RATE_INFO = 0xA6
CMD_FEED_BACK = 0x3D
CMD_SET_POSITION = 0x3E
CMD_GET_POSITION = 0x3F
CMD_POSITION_INFO = 0xA7

# Reply timeouts. FEED_NEXT can legitimately take up to the firmware's
# MOVE_TIMEOUT_MS (6000) before it NACKs, plus up to 5 s of coupled peel on
# v0.02b+; PEEL replies only after the run.
DEFAULT_REPLY_TIMEOUT_S = 1.0
FEED_REPLY_TIMEOUT_S = 12.0
PEEL_REPLY_MARGIN_S = 1.5
PEEL_CAL_MAX_S = 5.0  # firmware caps the calibrated peel time at 5000 ms
JOG_STEPS_MM = [-4, -2, -1, -0.5, -0.2, 0.2, 0.5, 1, 2, 4]
DISCOVER_WINDOW_S = 0.6  # firmware jitters replies over 0-200ms

CYCLE_ORDERS = ["feed, then peel", "peel, then feed", "feed only", "peel only"]

COLOR_OK = "#26a269"
COLOR_BAD = "#c01c28"
COLOR_WARN = "#c64600"
COLOR_OFF = "#9a9996"
COLOR_DIM = "gray40"
ICONS = {None: ("○", COLOR_OFF), "ok": ("✔", COLOR_OK), "fail": ("✖", COLOR_BAD), "warn": ("●", COLOR_WARN)}

# (key, headline) - the controls for each row are built in _build_bringup_tab
BRINGUP_STEPS = [
    ("connect", "Connect to the RS485 adapter"),
    ("address", "Give the feeder a bus address"),
    ("ping", "Check the link"),
    ("status", "Magnet detected, no driver fault"),
    ("width", "Tape width (set once per feeder)"),
    ("pitch", "Feed pitch"),
    ("zero", "Seat the first pocket's hole, set zero"),
    ("feed", "Feed once"),
    ("peelrate", "Peel rate (feed and peel move together)"),
]

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
        self.root.geometry("1220x900")
        self.root.minsize(1020, 680)
        self.link: SerialLink | None = None
        self.inbox = queue.Queue()  # reader thread frames + worker log lines, in arrival order
        self.worker: threading.Thread | None = None
        self.abort = threading.Event()
        self.firmwares = firmware_bauds()
        self.settings = self._load_settings()

        # live-card state
        self.last_rx = None            # monotonic time of the last frame from the target
        self.rx_wait_since = time.monotonic()  # when we started waiting for the first reply from this target
        self.last_poll = 0.0
        self.poll_inflight = False     # worker-side flags read by the GUI thread to keep the log quiet
        self.reading_config = False
        self.config_read_for = None    # address whose saved config was auto-read this connection
        self.cfg_state = {}            # raw values behind the card's "saved on feeder" text
        self.step_widgets = {}
        self.ind = {}

        self._build_styles()
        self._build_widgets()
        self._refresh_ports()
        self._apply_settings()
        self.root.protocol("WM_DELETE_WINDOW", self._on_close)
        self.root.bind("<Escape>", lambda _e: self._stop_all())
        self._set_step("connect", None, "")
        self._poll_inbox()
        self._tick()

    # ---------------------------
    # Remembered settings
    # ---------------------------
    def _load_settings(self) -> dict:
        try:
            with open(SETTINGS_PATH, encoding="utf-8") as f:
                data = json.load(f)
            return data if isinstance(data, dict) else {}
        except (OSError, ValueError):
            return {}

    def _save_settings(self):
        data = {
            "port": self.port_var.get(), "fw": self.fw_var.get(), "baud": self.baud_var.get(),
            "rts": self.rts_var.get(), "addr": self.addr_var.get(), "new_addr": self.new_addr_var.get(),
            "pitch": self.pitch_var.get(), "width": self.width_var.get(), "peel_ms": self.peel_ms_var.get(),
            "peel_dir": self.peel_dir_var.get(), "rate": self.rate_var.get(), "autopoll": self.autopoll_var.get(),
            "prod_port": self.prod_port_var.get(), "prod_routine": self.routine_var.get(),
            "prod_test_hex": self.test_hex_var.get(), "prod_prod_hex": self.prod_hex_var.get(),
            "prod_avrdude": self.avrdude_var.get(), "prod_logdir": self.logdir_var.get(),
            "prod_width": self.prod_width_var.get(), "prod_operator": self.operator_var.get(),
            "prod_dry": self.dry_isp_var.get(), "prod_stop_on_fail": self.stop_on_fail_var.get(),
        }
        try:
            with open(SETTINGS_PATH, "w", encoding="utf-8") as f:
                json.dump(data, f, indent=1)
        except OSError:
            pass  # remembering settings is a convenience, never worth an error dialog

    def _apply_settings(self):
        s = self.settings
        ports = list(self.port_combo["values"])
        if s.get("port") in ports:
            self.port_var.set(s["port"])
        if s.get("fw") in list(self.fw_combo["values"]):
            self.fw_var.set(s["fw"])
        for key, var in (("baud", self.baud_var), ("addr", self.addr_var), ("new_addr", self.new_addr_var),
                         ("pitch", self.pitch_var), ("width", self.width_var), ("peel_ms", self.peel_ms_var),
                         ("peel_dir", self.peel_dir_var), ("rate", self.rate_var)):
            if isinstance(s.get(key), str) and s[key]:
                var.set(s[key])
        for key, var in (("prod_port", self.prod_port_var), ("prod_routine", self.routine_var),
                         ("prod_test_hex", self.test_hex_var), ("prod_prod_hex", self.prod_hex_var),
                         ("prod_avrdude", self.avrdude_var), ("prod_logdir", self.logdir_var),
                         ("prod_width", self.prod_width_var), ("prod_operator", self.operator_var)):
            if isinstance(s.get(key), str) and s[key]:
                var.set(s[key])
        for key, var in (("prod_dry", self.dry_isp_var), ("prod_stop_on_fail", self.stop_on_fail_var)):
            if isinstance(s.get(key), bool):
                var.set(s[key])
        self._prod_load_routine(quiet=True)
        if isinstance(s.get("rts"), bool):
            self.rts_var.set(s["rts"])
        if isinstance(s.get("autopoll"), bool):
            self.autopoll_var.set(s["autopoll"])

    # ---------------------------
    # Layout
    # ---------------------------
    def _build_styles(self):
        style = ttk.Style()
        style.configure("TButton", padding=(8, 3))
        base = tkfont.nametofont("TkDefaultFont")
        self.bold_font = base.copy()
        self.bold_font.configure(weight="bold")
        self.icon_font = base.copy()
        self.icon_font.configure(size=base.cget("size") + 4, weight="bold")
        self.big_font = base.copy()
        self.big_font.configure(size=base.cget("size") + 3, weight="bold")
        self.mono_font = tkfont.nametofont("TkFixedFont")

    def _build_widgets(self):
        pad = {"padx": 4, "pady": 4}
        self.root.columnconfigure(0, weight=1)
        self.root.rowconfigure(1, weight=5)
        self.root.rowconfigure(2, weight=1)

        # --- Connection ---
        conn = ttk.LabelFrame(self.root, text="Connection")
        conn.grid(row=0, column=0, sticky="ew", **pad)

        ttk.Label(conn, text="Port:").grid(row=0, column=0, **pad)
        self.port_var = tk.StringVar()
        self.port_combo = ttk.Combobox(conn, textvariable=self.port_var, width=14)
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

        self.connect_btn_var = tk.StringVar(value="Connect")
        ttk.Button(conn, textvariable=self.connect_btn_var, width=11, command=self._toggle_connect).grid(row=0, column=8, **pad)

        self.status_var = tk.StringVar(value="Disconnected")
        self.status_label = ttk.Label(conn, textvariable=self.status_var, foreground="red")
        self.status_label.grid(row=0, column=9, sticky="w", **pad)

        # Variables shared between the bring-up tab and the tab each belongs to
        self.new_addr_var = tk.StringVar(value="1")
        self.width_var = tk.StringVar(value="8")
        self.pitch_var = tk.StringVar(value="4")
        self.rate_var = tk.StringVar(value="")
        self.peel_ms_var = tk.StringVar(value="1570")
        self.peel_dir_var = tk.StringVar(value="fwd")
        self.cal_onfeeder_var = tk.StringVar(value="—")
        self.cal_mm_var = tk.StringVar(value="4")
        self.cal_net_var = tk.StringVar(value="0 ms")
        self.cal_avg_var = tk.StringVar(value="Average: —")
        self.cal_n_var = tk.StringVar(value="5")
        self.posx_var = tk.StringVar(value="0")
        self.cal_net_ms = 0

        # --- Feeder card (left) + tabs (right) ---
        main = ttk.Frame(self.root)
        main.grid(row=1, column=0, sticky="nsew")
        main.columnconfigure(1, weight=1)
        main.rowconfigure(0, weight=1)
        self._build_card(main).grid(row=0, column=0, sticky="ns", **pad)

        self.tabs = ttk.Notebook(main)
        self.tabs.grid(row=0, column=1, sticky="nsew", **pad)
        self.tab_frames = {}
        for key, label, builder in (
                ("bringup", "Bring-up", self._build_bringup_tab),
                ("feed", "Feed & peel", self._build_feed_tab),
                ("peelcal", "Peel calibration", self._build_peelcal_tab),
                ("jog", "Jog & zero", self._build_jog_tab),
                ("setup", "Setup", self._build_setup_tab),
                ("production", "Production", self._build_production_tab),
                ("builder", "Packet builder", self._build_builder_tab)):
            frame = builder(self.tabs)
            self.tab_frames[key] = frame
            self.tabs.add(frame, text=label)

        # --- Log ---
        log_frame = ttk.LabelFrame(self.root, text="Log")
        log_frame.grid(row=2, column=0, sticky="nsew", **pad)
        log_frame.rowconfigure(0, weight=1)
        log_frame.columnconfigure(0, weight=1)

        self.log = scrolledtext.ScrolledText(log_frame, height=6, state="disabled", wrap="word")
        self.log.grid(row=0, column=0, columnspan=5, sticky="nsew", **pad)
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

        self.addr_var.trace_add("write", lambda *_: self._on_addr_changed())
        self._on_cmd_choice()

    # --- The always-visible feeder card ---
    def _indicator_row(self, parent, row, key, title):
        ttk.Label(parent, text=title).grid(row=row, column=0, sticky="w", padx=(6, 2), pady=1)
        dot = ttk.Label(parent, text="●", foreground=COLOR_OFF)
        dot.grid(row=row, column=1, padx=2)
        var = tk.StringVar(value="—")
        ttk.Label(parent, textvariable=var, width=24).grid(row=row, column=2, sticky="w", padx=(2, 6))
        self.ind[key] = (dot, var)

    def _build_card(self, parent):
        pad = {"padx": 4, "pady": 3}
        card = ttk.LabelFrame(parent, text="Feeder")
        card.columnconfigure(0, weight=1)

        top = ttk.Frame(card)
        top.grid(row=0, column=0, sticky="ew", **pad)
        ttk.Label(top, text="Address:").grid(row=0, column=0, **pad)
        self.addr_var = tk.StringVar(value="1")
        ttk.Entry(top, textvariable=self.addr_var, width=6, font=self.bold_font).grid(row=0, column=1, **pad)
        self.autopoll_var = tk.BooleanVar(value=True)
        ttk.Checkbutton(top, text="Live refresh", variable=self.autopoll_var).grid(row=0, column=2, **pad)

        btns = ttk.Frame(card)
        btns.grid(row=1, column=0, sticky="ew", **pad)
        ttk.Button(btns, text="Ping", width=7, command=lambda: self._quick(CMD_PING)).grid(row=0, column=0, **pad)
        ttk.Button(btns, text="Identify", width=8, command=lambda: self._quick(CMD_IDENTIFY, bytes([0]))).grid(row=0, column=1, **pad)
        ttk.Button(btns, text="Refresh", width=8, command=lambda: self._quick(CMD_GET_STATUS)).grid(row=0, column=2, **pad)

        led = ttk.Frame(card)
        led.grid(row=2, column=0, sticky="e", padx=4)
        ttk.Label(led, text="2nd LED (fiber):").grid(row=0, column=0, padx=2)
        ttk.Button(led, text="On", width=5, command=lambda: self._quick(CMD_SET_EXT_LED, bytes([1]))).grid(row=0, column=1, padx=2)
        ttk.Button(led, text="Off", width=5, command=lambda: self._quick(CMD_SET_EXT_LED, bytes([0]))).grid(row=0, column=2, padx=2)

        self.stop_btn = tk.Button(card, text="STOP  (Esc)", bg="#c01c28", fg="white", activebackground="#8f1420",
                                  activeforeground="white", font=self.big_font, relief="raised", bd=3,
                                  command=self._stop_all)
        self.stop_btn.grid(row=3, column=0, sticky="ew", padx=8, pady=6, ipady=6)

        self.busy_var = tk.StringVar()
        ttk.Label(card, textvariable=self.busy_var, foreground=COLOR_WARN, wraplength=260).grid(row=4, column=0, sticky="w", **pad)

        live = ttk.LabelFrame(card, text="Live")
        live.grid(row=5, column=0, sticky="ew", **pad)
        for r, (key, title) in enumerate((("link", "Link"), ("magnet", "Magnet"), ("driver", "Motor driver"),
                                          ("relay", "RS485 relay"), ("angle", "Wheel angle"),
                                          ("current", "12V current"), ("lasterr", "Last move"))):
            self._indicator_row(live, r, key, title)

        saved = ttk.LabelFrame(card, text="Saved on this feeder")
        saved.grid(row=6, column=0, sticky="ew", **pad)
        self.cfg_vars = {}
        for r, (key, title) in enumerate((("component", "Component id"), ("zero", "Tape zero"), ("pitch", "Feed pitch"),
                                          ("width", "Tape width"), ("peeltime", "Peel time"), ("peelrate", "Peel rate"))):
            ttk.Label(saved, text=title).grid(row=r, column=0, sticky="w", padx=(6, 2), pady=1)
            var = tk.StringVar(value="—")
            ttk.Label(saved, textvariable=var, width=26).grid(row=r, column=1, sticky="w", padx=(2, 6))
            self.cfg_vars[key] = var
        ttk.Button(saved, text="Read from feeder", command=self._do_read_config).grid(
            row=len(self.cfg_vars), column=0, columnspan=2, pady=4)
        return card

    def _set_ind(self, key, state, text):
        color = {"ok": COLOR_OK, "bad": COLOR_BAD, "warn": COLOR_WARN, "off": COLOR_OFF}[state]
        dot, var = self.ind[key]
        dot.configure(foreground=color)
        var.set(text)

    def _clear_live(self):
        for key in ("magnet", "driver", "relay", "angle", "current", "lasterr"):
            self._set_ind(key, "off", "—")
        for var in self.cfg_vars.values():
            var.set("—")
        self.cal_onfeeder_var.set("—")
        self.cfg_state.clear()
        self.config_read_for = None

    # --- Bring-up checklist ---
    def _build_bringup_tab(self, parent):
        pad = {"padx": 6, "pady": 5}
        tab = ttk.Frame(parent)
        tab.columnconfigure(3, weight=1)
        ttk.Label(tab, foreground="gray25", wraplength=700, justify="left", text=(
            "A first-test checklist. Ticks come from what the feeder reports, so a feeder that was set up "
            "earlier shows up already done. Work top to bottom; a red cross says why.")
        ).grid(row=0, column=0, columnspan=4, sticky="w", **pad)
        for r, (key, headline) in enumerate(BRINGUP_STEPS, start=1):
            icon = ttk.Label(tab, text=ICONS[None][0], foreground=ICONS[None][1], font=self.icon_font, width=2)
            icon.grid(row=r, column=0, **pad)
            ttk.Label(tab, text=f"{r}. {headline}", font=self.bold_font).grid(row=r, column=1, sticky="w", **pad)
            controls = ttk.Frame(tab)
            controls.grid(row=r, column=2, sticky="w", **pad)
            note = tk.StringVar()
            ttk.Label(tab, textvariable=note, foreground=COLOR_DIM, wraplength=250, justify="left").grid(
                row=r, column=3, sticky="w", **pad)
            self.step_widgets[key] = (icon, note)
            self._bringup_controls(key, controls)
        return tab

    def _bringup_controls(self, key, f):
        pad = {"padx": 2}
        if key == "connect":
            ttk.Button(f, textvariable=self.connect_btn_var, width=11, command=self._toggle_connect).grid(row=0, column=0, **pad)
        elif key == "address":
            ttk.Label(f, text="New addr:").grid(row=0, column=0, **pad)
            ttk.Entry(f, textvariable=self.new_addr_var, width=5).grid(row=0, column=1, **pad)
            ttk.Button(f, text="Scan + assign", command=self._do_scan_assign).grid(row=0, column=2, **pad)
        elif key == "ping":
            ttk.Button(f, text="Ping", command=self._do_ping).grid(row=0, column=0, **pad)
        elif key == "status":
            ttk.Button(f, text="Get status", command=lambda: self._quick(CMD_GET_STATUS)).grid(row=0, column=0, **pad)
        elif key == "width":
            ttk.Combobox(f, textvariable=self.width_var, width=5, state="readonly",
                         values=["8", "12", "16", "24", "32", "44", "56"]).grid(row=0, column=0, **pad)
            ttk.Button(f, text="Write", command=self._do_write_width).grid(row=0, column=1, **pad)
        elif key == "pitch":
            ttk.Combobox(f, textvariable=self.pitch_var, width=5, values=["2", "4", "8", "12", "16", "20", "24"]).grid(row=0, column=0, **pad)
            ttk.Button(f, text="Set", command=self._do_set_pitch).grid(row=0, column=1, **pad)
        elif key == "zero":
            ttk.Button(f, text="Jog ▸", command=lambda: self.tabs.select(self.tab_frames["jog"])).grid(row=0, column=0, **pad)
            ttk.Button(f, text="Set zero here", command=self._do_zero_here).grid(row=0, column=1, **pad)
        elif key == "feed":
            ttk.Button(f, text="Feed once", command=lambda: self._start_cycle(1, "feed only")).grid(row=0, column=0, **pad)
        elif key == "peelrate":
            ttk.Button(f, text="Peel ▸", command=lambda: self.tabs.select(self.tab_frames["peelcal"])).grid(row=0, column=0, **pad)

    def _set_step(self, key, state, note=""):
        """GUI thread only - workers go through _set_step_ui()."""
        if key not in self.step_widgets:
            return
        icon, var = self.step_widgets[key]
        glyph, color = ICONS[state]
        icon.configure(text=glyph, foreground=color)
        var.set(note)

    def _set_step_ui(self, key, state, note=""):
        self._ui(lambda: self._set_step(key, state, note))

    def _step_from_frame(self, key, frame, ok_note=""):
        if frame is not None and frame.cmd == CMD_ACK:
            self._set_step_ui(key, "ok", ok_note)
        elif frame is None:
            self._set_step_ui(key, "fail", "no reply")
        else:
            code = frame.payload[0] if frame.payload else None
            self._set_step_ui(key, "fail", ERROR_CODES.get(code, "NACK") if code is not None else "NACK")

    def _reset_steps(self):
        for key in self.step_widgets:
            self._set_step(key, None, "")

    # --- Jog tab (unchanged) ---
    def _build_jog_tab(self, parent):
        pad = {"padx": 4, "pady": 4}
        tab = ttk.Frame(parent)
        ttk.Label(tab, foreground="gray25", wraplength=700, justify="left", text=(
            "Seat the sprocket hole for the reel's first pocket, then Set zero here. Needs v0.02 firmware. "
            "4 mm = one tooth, 2 mm = half a tooth (the finest feed pitch). Jog moves are relative to where "
            "the wheel is; each reply shows the new raw angle. Jog does not move the peel motor.")
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
        ttk.Button(tab, text="Get status", command=lambda: self._quick(CMD_GET_STATUS)).grid(row=4, column=2, columnspan=2, **pad)
        ttk.Button(tab, text="Feed once", command=lambda: self._start_cycle(1, "feed only")).grid(row=4, column=4, columnspan=2, **pad)
        ttk.Label(tab, foreground="gray40", wraplength=420, justify="left", text=(
            "Set zero here stores the current wheel angle as the tape zero (CMD_ZERO_HERE). "
            "Changing the component id clears it. Flipping the feed direction mirrors the angle, "
            "so set the direction first.")
        ).grid(row=4, column=6, columnspan=6, sticky="w", **pad)
        return tab

    # --- Feed & peel tab ---
    def _build_feed_tab(self, parent):
        pad = {"padx": 4, "pady": 4}
        tab = ttk.Frame(parent)
        tab.columnconfigure(0, weight=1)

        feed = ttk.LabelFrame(tab, text="Feed")
        feed.grid(row=0, column=0, sticky="ew", **pad)
        ttk.Label(feed, text="Pitch (mm):").grid(row=0, column=0, sticky="e", **pad)
        ttk.Combobox(feed, textvariable=self.pitch_var, width=6, values=["2", "4", "8", "12", "16", "20", "24"]).grid(row=0, column=1, sticky="w", **pad)
        ttk.Button(feed, text="Set pitch", command=self._do_set_pitch).grid(row=0, column=2, **pad)
        ttk.Button(feed, text="Feed once", command=lambda: self._start_cycle(1, "feed only")).grid(row=0, column=3, **pad)
        ttk.Button(feed, text="Back once", command=self._do_feed_back).grid(row=0, column=4, **pad)
        ttk.Label(feed, foreground=COLOR_DIM, wraplength=640, justify="left", text=(
            "v0.02b+ with a peel rate saved: Feed once moves the sprocket, then peels; Back once peels in reverse, "
            "then moves back. Without a rate they only move the sprocket. Set the rate on the Peel calibration tab.")
        ).grid(row=1, column=0, columnspan=5, sticky="w", **pad)

        cyc = ttk.LabelFrame(tab, text="Cycle test")
        cyc.grid(row=1, column=0, sticky="ew", **pad)
        ttk.Label(cyc, text="Cycle:").grid(row=0, column=0, sticky="e", **pad)
        self.order_var = tk.StringVar(value=CYCLE_ORDERS[2])
        ttk.Combobox(cyc, textvariable=self.order_var, width=16, state="readonly", values=CYCLE_ORDERS).grid(row=0, column=1, columnspan=2, sticky="w", **pad)
        ttk.Label(cyc, text="Cycles:").grid(row=0, column=3, sticky="e", **pad)
        self.cycles_var = tk.StringVar(value="5")
        ttk.Spinbox(cyc, textvariable=self.cycles_var, from_=1, to=1000, width=6).grid(row=0, column=4, sticky="w", **pad)
        ttk.Label(cyc, text="Pause (ms):").grid(row=0, column=5, sticky="e", **pad)
        self.pause_var = tk.StringVar(value="300")
        ttk.Spinbox(cyc, textvariable=self.pause_var, from_=0, to=10000, increment=100, width=7).grid(row=0, column=6, sticky="w", **pad)
        ttk.Button(cyc, text="Run cycle test", command=lambda: self._start_cycle(None, None)).grid(row=0, column=7, **pad)
        ttk.Label(cyc, foreground=COLOR_DIM, wraplength=640, justify="left", text=(
            "Each step logs its round-trip time, with min/avg/max at the end. With a peel rate saved use 'feed only' "
            "(the feed already peels); the 'then peel' orders are for older firmware or a fixed peel time "
            "(Peel calibration tab, bottom).")
        ).grid(row=1, column=0, columnspan=8, sticky="w", **pad)
        return tab

    # --- Peel calibration tab ---
    PEEL_NUDGES_MS = (50, 100, 250, 500, 1000)

    def _build_peelcal_tab(self, parent):
        pad = {"padx": 4, "pady": 3}
        tab = ttk.Frame(parent)
        tab.columnconfigure(0, weight=1)

        # 1. rate
        rate = ttk.LabelFrame(tab, text="1. Peel rate: ms of peel per mm of feed (v0.02b+)")
        rate.grid(row=0, column=0, sticky="ew", **pad)
        ttk.Label(rate, text="Rate:").grid(row=0, column=0, sticky="e", **pad)
        ttk.Entry(rate, textvariable=self.rate_var, width=8, font=self.bold_font).grid(row=0, column=1, sticky="w", **pad)
        ttk.Button(rate, text="Save to feeder", command=self._do_set_peel_rate).grid(row=0, column=2, **pad)
        ttk.Button(rate, text="Read", command=lambda: self._quick(CMD_GET_PEEL_RATE)).grid(row=0, column=3, **pad)
        ttk.Button(rate, text="Turn off", command=lambda: self._do_set_peel_rate(off=True)).grid(row=0, column=4, **pad)
        ttk.Label(rate, text="On the feeder:").grid(row=0, column=5, sticky="e", **pad)
        ttk.Label(rate, textvariable=self.cal_onfeeder_var, font=self.bold_font, width=18).grid(row=0, column=6, sticky="w", **pad)
        ttk.Label(rate, text="Fine-tune (saves at once):").grid(row=1, column=0, columnspan=2, sticky="e", **pad)
        tune = ttk.Frame(rate)
        tune.grid(row=1, column=2, columnspan=5, sticky="w")
        for i, pct in enumerate((-10, -5, -1, 1, 5, 10)):
            ttk.Button(tune, text=f"{pct:+d}%", width=6, command=lambda v=pct: self._calib_adjust(v)).grid(row=0, column=i, padx=2)

        # 2. measure
        meas = ttk.LabelFrame(tab, text="2. Measure it (optional - or just guess a rate and fine-tune in step 3)")
        meas.grid(row=1, column=0, sticky="ew", **pad)
        ttk.Label(meas, text="Feed").grid(row=0, column=0, sticky="e", **pad)
        ttk.Combobox(meas, textvariable=self.cal_mm_var, width=5, values=["2", "4", "8", "12", "16"]).grid(row=0, column=1, sticky="w", **pad)
        ttk.Label(meas, text="mm with no peel").grid(row=0, column=2, sticky="w", **pad)
        ttk.Button(meas, text="Feed (no peel)", command=self._calib_feed_plain).grid(row=0, column=3, **pad)
        ttk.Label(meas, foreground=COLOR_DIM, text="uses jog, which never touches the peel motor").grid(row=0, column=4, columnspan=4, sticky="w", **pad)

        ttk.Label(meas, text="Peel until taut:").grid(row=1, column=0, sticky="e", **pad)
        nud = ttk.Frame(meas)
        nud.grid(row=1, column=1, columnspan=7, sticky="w")
        col = 0
        for ms in reversed(self.PEEL_NUDGES_MS[:3]):
            ttk.Button(nud, text=f"-{ms}", width=6, command=lambda m=ms: self._calib_nudge(1, m)).grid(row=0, column=col, padx=1)
            col += 1
        ttk.Label(nud, text=" ms ").grid(row=0, column=col)
        col += 1
        for ms in self.PEEL_NUDGES_MS:
            ttk.Button(nud, text=f"+{ms}", width=6, command=lambda m=ms: self._calib_nudge(0, m)).grid(row=0, column=col, padx=1)
            col += 1

        ttk.Label(meas, text="This round:").grid(row=2, column=0, sticky="e", **pad)
        ttk.Label(meas, textvariable=self.cal_net_var, font=self.bold_font, width=10).grid(row=2, column=1, columnspan=2, sticky="w", **pad)
        ttk.Button(meas, text="Reset round", command=lambda: self._calib_set_net(0)).grid(row=2, column=3, **pad)
        ttk.Button(meas, text="Record measurement", command=self._calib_record).grid(row=2, column=4, columnspan=2, **pad)

        self.cal_tree = ttk.Treeview(meas, columns=("n", "mm", "ms", "rate"), show="headings", height=4, selectmode="browse")
        for colname, text, w in (("n", "#", 40), ("mm", "Feed (mm)", 90), ("ms", "Peel (ms)", 90), ("rate", "ms/mm", 90)):
            self.cal_tree.heading(colname, text=text)
            self.cal_tree.column(colname, width=w, anchor="center")
        self.cal_tree.grid(row=3, column=0, columnspan=4, sticky="w", **pad)
        side = ttk.Frame(meas)
        side.grid(row=3, column=4, columnspan=4, sticky="nw", **pad)
        ttk.Label(side, textvariable=self.cal_avg_var, font=self.bold_font, wraplength=260, justify="left").grid(row=0, column=0, columnspan=3, sticky="w")
        ttk.Button(side, text="Use average", command=self._calib_use_average).grid(row=1, column=0, pady=2)
        ttk.Button(side, text="Delete row", command=self._calib_delete_row).grid(row=1, column=1, padx=2, pady=2)
        ttk.Button(side, text="Clear", command=self._calib_clear).grid(row=1, column=2, pady=2)
        ttk.Label(meas, foreground=COLOR_DIM, wraplength=700, justify="left", text=(
            "Every nudge starts with a short soft-start ramp, so a few long nudges match a real feed better than many "
            "short ones; the number is a starting point, step 3 is the real test. Feed, nudge + until the cover tape "
            "is just taut (- to back off), Record, repeat a few times, Use average, Save to feeder.")
        ).grid(row=4, column=0, columnspan=8, sticky="w", **pad)

        # 3. verify
        ver = ttk.LabelFrame(tab, text="3. Verify with the real thing (peel follows feed)")
        ver.grid(row=2, column=0, sticky="ew", **pad)
        ttk.Button(ver, text="Feed (peel follows)", command=lambda: self._start_cycle(1, "feed only")).grid(row=0, column=0, **pad)
        ttk.Button(ver, text="Back (peel reverses first)", command=self._do_feed_back).grid(row=0, column=1, **pad)
        ttk.Label(ver, text="Run").grid(row=0, column=2, sticky="e", **pad)
        ttk.Spinbox(ver, textvariable=self.cal_n_var, from_=1, to=200, width=5).grid(row=0, column=3, **pad)
        ttk.Button(ver, text="feeds in a row", command=lambda: self._start_cycle(self._int_or(self.cal_n_var.get(), 5), "feed only")).grid(row=0, column=4, **pad)
        ttk.Label(ver, foreground=COLOR_DIM, wraplength=700, justify="left", text=(
            "Cover tape slack or bunching up after a feed: rate too low, use +. Tape being pulled, or the next pocket "
            "lifting: rate too high, use -. A backward move should leave a little slack, never pull. The pause between "
            "feeds is the Cycle test pause on the Feed & peel tab.")
        ).grid(row=1, column=0, columnspan=6, sticky="w", **pad)

        # 4. fixed time
        fixed = ttk.LabelFrame(tab, text="Fixed-time peel (v0.02; not tied to feed distance)")
        fixed.grid(row=3, column=0, sticky="ew", **pad)
        ttk.Label(fixed, text="Dir:").grid(row=0, column=0, sticky="e", **pad)
        ttk.Combobox(fixed, textvariable=self.peel_dir_var, width=6, state="readonly", values=["fwd", "rev"]).grid(row=0, column=1, sticky="w", **pad)
        ttk.Label(fixed, text="Time (ms):").grid(row=0, column=2, sticky="e", **pad)
        ttk.Spinbox(fixed, textvariable=self.peel_ms_var, from_=10, to=5000, increment=10, width=7).grid(row=0, column=3, sticky="w", **pad)
        ttk.Button(fixed, text="Peel once", command=lambda: self._start_cycle(1, "peel only")).grid(row=0, column=4, **pad)
        ttk.Button(fixed, text="Save time to feeder", command=self._do_save_peel_cal).grid(row=1, column=0, columnspan=3, **pad)
        ttk.Button(fixed, text="Read saved", command=lambda: self._quick(CMD_GET_PEEL_TIME)).grid(row=1, column=3, **pad)
        ttk.Button(fixed, text="Run saved (fwd)", command=lambda: self._do_peel_cal_run(0)).grid(row=1, column=4, **pad)
        ttk.Button(fixed, text="(rev)", width=6, command=lambda: self._do_peel_cal_run(1)).grid(row=1, column=5, sticky="w", **pad)
        self.peel_use_cal_var = tk.BooleanVar(value=False)
        ttk.Checkbutton(fixed, text="Cycle test peel steps use the saved time", variable=self.peel_use_cal_var).grid(
            row=2, column=0, columnspan=9, sticky="w", **pad)
        return tab

    @staticmethod
    def _int_or(text, default):
        try:
            return max(1, int(text))
        except ValueError:
            return default

    # --- Production tab (TAP-Jig) ---
    PROD_ICONS = {"pending": ("○", COLOR_OFF), "running": ("▶", "#1a5fb4"), "pass": ("✔", COLOR_OK),
                  "fail": ("✖", COLOR_BAD), "error": ("✖", COLOR_BAD), "aborted": ("■", COLOR_WARN)}

    def _build_production_tab(self, parent):
        pad = {"padx": 4, "pady": 3}
        here = os.path.dirname(os.path.abspath(__file__))
        tab = ttk.Frame(parent)
        tab.columnconfigure(0, weight=1)
        tab.rowconfigure(3, weight=1)
        self.jig = None
        self.prod_thread = None
        self.prod_abort = threading.Event()
        self.prod_routine = None
        self.prod_results = {}
        self.prod_counts = {"pass": 0, "fail": 0}
        self.prod_port_var = tk.StringVar()
        self.prod_baud_var = tk.StringVar(value="115200")
        self.prod_status_var = tk.StringVar(value="Jig not connected")
        self.routine_var = tk.StringVar(value=os.path.join(here, "tapjig_routine_feeder_v03.json"))
        self.test_hex_var = tk.StringVar()
        self.prod_hex_var = tk.StringVar()
        self.avrdude_var = tk.StringVar(value="avrdude")
        self.logdir_var = tk.StringVar(value=os.path.join(here, "production_logs"))
        self.prod_width_var = tk.StringVar(value="12")
        self.operator_var = tk.StringVar()
        self.dry_isp_var = tk.BooleanVar(value=False)
        self.stop_on_fail_var = tk.BooleanVar(value=True)
        self.prod_banner_var = tk.StringVar(value="READY")
        self.prod_count_var = tk.StringVar(value="Session: 0 pass / 0 fail")

        jig = ttk.LabelFrame(tab, text="Jig (ATmega32u4 over USB)")
        jig.grid(row=0, column=0, sticky="ew", **pad)
        ttk.Label(jig, text="Port:").grid(row=0, column=0, **pad)
        self.prod_port_combo = ttk.Combobox(jig, textvariable=self.prod_port_var, width=14)
        self.prod_port_combo.grid(row=0, column=1, **pad)
        ttk.Button(jig, text="Refresh", command=self._prod_refresh_ports).grid(row=0, column=2, **pad)
        ttk.Label(jig, text="Baud:").grid(row=0, column=3, **pad)
        ttk.Entry(jig, textvariable=self.prod_baud_var, width=8).grid(row=0, column=4, **pad)
        self.prod_connect_btn = ttk.Button(jig, text="Connect", width=11, command=self._prod_toggle_connect)
        self.prod_connect_btn.grid(row=0, column=5, **pad)
        ttk.Button(jig, text="Safe state", command=self._prod_safe).grid(row=0, column=6, **pad)
        ttk.Label(jig, textvariable=self.prod_status_var).grid(row=0, column=7, sticky="w", **pad)
        self._prod_refresh_ports()

        unit = ttk.LabelFrame(tab, text="Unit, routine and programmer")
        unit.grid(row=1, column=0, sticky="ew", **pad)
        unit.columnconfigure(1, weight=1)
        rows = (("Routine:", self.routine_var, "json", self._prod_load_routine),
                ("Test firmware (.hex):", self.test_hex_var, "hex", None),
                ("Production firmware (.hex):", self.prod_hex_var, "hex", None),
                ("avrdude:", self.avrdude_var, "exe", None),
                ("Log folder:", self.logdir_var, "dir", None))
        for r, (label, var, kind, after) in enumerate(rows):
            ttk.Label(unit, text=label).grid(row=r, column=0, sticky="e", **pad)
            ttk.Entry(unit, textvariable=var).grid(row=r, column=1, sticky="ew", **pad)
            ttk.Button(unit, text="Browse...", command=lambda v=var, k=kind, a=after: self._prod_browse(v, k, a)).grid(row=r, column=2, **pad)
        ttk.Label(unit, text="Tape width (mm):").grid(row=0, column=3, sticky="e", **pad)
        ttk.Combobox(unit, textvariable=self.prod_width_var, width=6, state="readonly",
                     values=["8", "12", "16", "24", "32", "44", "56"]).grid(row=0, column=4, sticky="w", **pad)
        ttk.Label(unit, text="Operator:").grid(row=1, column=3, sticky="e", **pad)
        ttk.Entry(unit, textvariable=self.operator_var, width=14).grid(row=1, column=4, sticky="w", **pad)
        ttk.Checkbutton(unit, text="Dry-run ISP (no programmer; for sim_jig)", variable=self.dry_isp_var).grid(
            row=2, column=3, columnspan=2, sticky="w", **pad)

        ctl = ttk.Frame(tab)
        ctl.grid(row=2, column=0, sticky="ew", **pad)
        ctl.columnconfigure(5, weight=1)
        self.prod_run_btn = ttk.Button(ctl, text="Run all stages", command=lambda: self._prod_run(None))
        self.prod_run_btn.grid(row=0, column=0, **pad)
        self.prod_run_sel_btn = ttk.Button(ctl, text="Run selected stage", command=self._prod_run_selected)
        self.prod_run_sel_btn.grid(row=0, column=1, **pad)
        self.prod_stop_btn = ttk.Button(ctl, text="Stop", command=self._prod_stop, state="disabled")
        self.prod_stop_btn.grid(row=0, column=2, **pad)
        ttk.Checkbutton(ctl, text="Stop at the first failed stage", variable=self.stop_on_fail_var).grid(row=0, column=3, **pad)
        ttk.Label(ctl, textvariable=self.prod_count_var, foreground=COLOR_DIM).grid(row=0, column=4, **pad)
        self.prod_banner = tk.Label(ctl, textvariable=self.prod_banner_var, bg="#9a9996", fg="white", font=self.big_font,
                                    width=34, anchor="w", padx=10, pady=5)
        self.prod_banner.grid(row=1, column=0, columnspan=6, sticky="ew", **pad)

        body = ttk.Frame(tab)
        body.grid(row=3, column=0, sticky="nsew", **pad)
        body.columnconfigure(0, weight=3)
        body.columnconfigure(1, weight=2)
        body.rowconfigure(0, weight=1)
        self.prod_tree = ttk.Treeview(body, columns=("id", "stage", "status", "detail", "time"), show="headings", height=10,
                                      selectmode="browse")
        for col, text, w, anchor in (("id", "#", 34, "center"), ("stage", "Stage", 200, "w"), ("status", "Result", 80, "center"),
                                     ("detail", "Detail", 260, "w"), ("time", "s", 50, "e")):
            self.prod_tree.heading(col, text=text)
            self.prod_tree.column(col, width=w, anchor=anchor, stretch=(col == "detail"))
        for tag, color in (("pass", COLOR_OK), ("fail", COLOR_BAD), ("error", COLOR_BAD), ("running", "#1a5fb4"),
                           ("aborted", COLOR_WARN), ("pending", "gray40")):
            self.prod_tree.tag_configure(tag, foreground=color)
        self.prod_tree.grid(row=0, column=0, sticky="nsew")
        sb = ttk.Scrollbar(body, orient="vertical", command=self.prod_tree.yview)
        self.prod_tree.configure(yscrollcommand=sb.set)
        sb.grid(row=0, column=0, sticky="nse")
        self.prod_tree.bind("<<TreeviewSelect>>", self._prod_show_detail)
        self.prod_detail = scrolledtext.ScrolledText(body, height=10, width=44, wrap="word", state="disabled", font=self.mono_font)
        self.prod_detail.grid(row=0, column=1, sticky="nsew", padx=(6, 0))
        return tab

    def _prod_refresh_ports(self):
        ports = [p.device for p in serial.tools.list_ports.comports()]
        self.prod_port_combo["values"] = ports
        if ports and not self.prod_port_var.get():
            self.prod_port_var.set(ports[-1] if len(ports) > 1 else ports[0])

    def _prod_browse(self, var, kind, after):
        if kind == "dir":
            path = filedialog.askdirectory(initialdir=var.get() or None)
        else:
            types = {"json": [("Routine", "*.json")], "hex": [("Intel hex", "*.hex")], "exe": [("Program", "*")]}[kind]
            path = filedialog.askopenfilename(filetypes=types, initialdir=os.path.dirname(var.get()) or None)
        if path:
            var.set(path)
            if after:
                after()

    def _prod_load_routine(self, quiet=False):
        try:
            self.prod_routine = tapjig.load_routine(self.routine_var.get())
        except (OSError, ValueError, KeyError, SyntaxError) as exc:
            self.prod_routine = None
            self.prod_tree.delete(*self.prod_tree.get_children())
            if not quiet:
                messagebox.showerror("Routine", f"Could not load the routine:\n{exc}")
            return
        self.prod_tree.delete(*self.prod_tree.get_children())
        self.prod_results.clear()
        for st in self.prod_routine["stages"]:
            self.prod_tree.insert("", "end", iid=str(st["id"]), values=(st["id"], st["name"], "", "", ""), tags=("pending",))
        self._prod_banner("READY", "#9a9996")

    def _prod_banner(self, text, color):
        self.prod_banner_var.set(text)
        self.prod_banner.configure(bg=color)

    def _prod_toggle_connect(self):
        if self.jig is not None:
            self.jig.close()
            self.jig = None
            self.prod_connect_btn.configure(text="Connect")
            self.prod_status_var.set("Jig not connected")
            return
        try:
            link = tapjig.JigLink.open(self.prod_port_var.get().strip(), int(self.prod_baud_var.get()))
            hello = link.cmd("HELLO")
        except (serial.SerialException, tapjig.JigError, ValueError, OSError) as exc:
            messagebox.showerror("Jig", f"Could not talk to the jig:\n{exc}")
            return
        self.jig = link
        self.prod_connect_btn.configure(text="Disconnect")
        self.prod_status_var.set("Connected: " + " ".join(f"{k}={v}" for k, v in hello.items()))
        self._log(f"Jig connected on {self.prod_port_var.get()}: {dict(hello)}", "info")

    def _prod_safe(self):
        if self.jig is None or (self.prod_thread and self.prod_thread.is_alive()):
            return
        try:
            self.jig.cmd("SAFE")
            self._log("Jig put in its safe state (PSU off, switches released).", "info")
        except tapjig.JigError as exc:
            self._log(f"Jig SAFE failed: {exc}", "err")

    def _prod_run_selected(self):
        sel = self.prod_tree.selection()
        if not sel:
            messagebox.showinfo("Run selected stage", "Select a stage in the list first.")
            return
        self._prod_run({int(sel[0])})

    def _prod_stop(self):
        self.prod_abort.set()
        self._prod_banner("STOPPING...", COLOR_WARN)

    def _prod_run(self, stage_ids):
        if self.prod_thread and self.prod_thread.is_alive():
            return
        if self.jig is None:
            messagebox.showwarning("Production", "Connect to the jig first.")
            return
        self._prod_load_routine()
        if self.prod_routine is None:
            return
        dry = self.dry_isp_var.get()
        needs_isp = any(any(st.get("do") == "isp.program" for st in stage["steps"])
                        for stage in self.prod_routine["stages"] if stage_ids is None or stage["id"] in stage_ids)
        if needs_isp and not dry:
            missing = [n for n, v in (("test", self.test_hex_var), ("production", self.prod_hex_var)) if not os.path.isfile(v.get())]
            if missing:
                messagebox.showerror("Production", f"Set the {' and '.join(missing)} firmware .hex file(s) first "
                                                   f"(or tick Dry-run ISP when using the simulator).")
                return
        options = {"tape_width_mm": int(self.prod_width_var.get()), "operator": self.operator_var.get().strip()}
        isp = tapjig.Isp(self.prod_routine.get("isp", {}), {"test": self.test_hex_var.get(), "production": self.prod_hex_var.get()},
                         avrdude=self.avrdude_var.get().strip() or "avrdude", dry_run=dry,
                         log=lambda s: self._log("   " + s, "info"))
        self.prod_abort.clear()
        self.prod_results.clear()
        for st in self.prod_routine["stages"]:
            if stage_ids is None or st["id"] in stage_ids:
                self.prod_tree.item(str(st["id"]), values=(st["id"], st["name"], "", "", ""), tags=("pending",))
        self._prod_set_running(True)
        self._prod_banner("RUNNING...", "#1a5fb4")
        self._save_settings()
        stop_on_fail = self.stop_on_fail_var.get()
        log_dir = self.logdir_var.get().strip() or "production_logs"
        link = self.jig
        routine = self.prod_routine

        def on_stage(sid, status, detail):
            self._ui(lambda: self._prod_stage_update(sid, status, detail))

        def on_stage_result(sr):
            self.prod_results[sr["id"]] = sr

        def prompt(msg):
            box = queue.Queue()
            self._ui(lambda: box.put(messagebox.askokcancel("Production", msg)))
            while True:
                try:
                    return box.get(timeout=0.2)
                except queue.Empty:
                    if self.prod_abort.is_set():
                        return False

        def run():
            runner = tapjig.Runner(routine, link, isp, options, on_stage=on_stage, on_log=lambda s: self._log(s, "info"),
                                   prompt=prompt, abort=self.prod_abort, on_stage_result=on_stage_result)
            try:
                result = runner.run(stage_ids, stop_on_fail=stop_on_fail)
            except Exception as exc:  # the runner reports stage errors itself; this is a last resort
                self._log(f"Production run crashed: {exc!r}", "err")
                self._ui(lambda: (self._prod_set_running(False), self._prod_banner(f"ERROR: {exc}", COLOR_BAD)))
                return
            path = ""
            if stage_ids is None:  # a partial run is a debugging aid, not a unit record
                try:
                    path = tapjig.write_log(result, log_dir)
                except OSError as exc:
                    self._log(f"Could not write the log: {exc}", "err")
            self._ui(lambda: self._prod_finished(result, path, stage_ids is None))

        self.prod_thread = threading.Thread(target=run, daemon=True)
        self.prod_thread.start()

    def _prod_set_running(self, running):
        state = "disabled" if running else "normal"
        self.prod_run_btn.configure(state=state)
        self.prod_run_sel_btn.configure(state=state)
        self.prod_stop_btn.configure(state="normal" if running else "disabled")

    def _prod_stage_update(self, sid, status, detail):
        iid = str(sid)
        if not self.prod_tree.exists(iid):
            return
        sr = self.prod_results.get(sid)
        glyph = self.PROD_ICONS.get(status, ("", ""))[0]
        vals = list(self.prod_tree.item(iid, "values"))
        vals[2] = {"pending": "", "running": "running"}.get(status, status.upper())
        vals[3] = detail if status != "running" else ""
        vals[4] = f"{sr['seconds']:.1f}" if sr else ""
        self.prod_tree.item(iid, values=vals, tags=(status,))
        if status == "running":
            self.prod_tree.see(iid)
            name = vals[1]
            self._prod_banner(f"RUNNING  stage {sid}: {name}", "#1a5fb4")
        if sr and self.prod_tree.selection() == (iid,):
            self._prod_show_detail()

    def _prod_finished(self, result, path, full_run):
        self._prod_set_running(False)
        if result["passed"]:
            self._prod_banner(f"PASS    SN {result['serial'] or '-'}", COLOR_OK)
        else:
            bad = next((s for s in result["stages"] if s["status"] != "pass"), None)
            what = f"stage {bad['id']}: {bad['name']}" if bad else "stopped"
            self._prod_banner(f"FAIL    {what}", COLOR_BAD)
        if full_run:
            self.prod_counts["pass" if result["passed"] else "fail"] += 1
            self.prod_count_var.set(f"Session: {self.prod_counts['pass']} pass / {self.prod_counts['fail']} fail")
        self._log(f"Production run {'PASS' if result['passed'] else 'FAIL'} in {result['seconds']} s"
                  + (f", log: {path}" if path else " (partial run, not logged)"), "result" if result["passed"] else "err")

    def _prod_show_detail(self, _event=None):
        sel = self.prod_tree.selection()
        text = ""
        if sel:
            sr = self.prod_results.get(int(sel[0]))
            if sr is None:
                text = "No result yet for this stage."
            else:
                lines = [f"Stage {sr['id']}: {sr['name']}  [{sr['status'].upper()}, {sr['seconds']} s]"]
                if sr["error"]:
                    lines.append(f"ERROR: {sr['error']}")
                lines.append("")
                for c in sr["checks"]:
                    mark = "✔" if c["ok"] else ("~" if c["advisory"] else "✖")
                    lines.append(f"{mark} {c['label']}")
                    if not c["ok"] or True:
                        for k, v in c["values"].items():
                            lines.append(f"     {k} = {json.dumps(v)}")
                text = "\n".join(lines)
        self.prod_detail.configure(state="normal")
        self.prod_detail.delete("1.0", "end")
        self.prod_detail.insert("end", text)
        self.prod_detail.configure(state="disabled")

    # --- Setup tab ---
    def _build_setup_tab(self, parent):
        pad = {"padx": 4, "pady": 4}
        tab = ttk.Frame(parent)
        tab.columnconfigure(0, weight=1)

        addr = ttk.LabelFrame(tab, text="Bus address (feeders boot unassigned, address 0, on every power-up)")
        addr.grid(row=0, column=0, sticky="ew", **pad)
        ttk.Button(addr, text="Scan", command=self._do_scan).grid(row=0, column=0, **pad)
        ttk.Label(addr, text="Nonce:").grid(row=0, column=1, **pad)
        self.nonce_var = tk.StringVar(value="0x0000")
        ttk.Entry(addr, textvariable=self.nonce_var, width=8).grid(row=0, column=2, **pad)
        ttk.Label(addr, text="New addr:").grid(row=0, column=3, **pad)
        ttk.Entry(addr, textvariable=self.new_addr_var, width=6).grid(row=0, column=4, **pad)
        ttk.Button(addr, text="Assign", command=self._do_assign).grid(row=0, column=5, **pad)
        ttk.Button(addr, text="Scan + assign", command=self._do_scan_assign).grid(row=0, column=6, **pad)
        ttk.Button(addr, text="Restore addresses", command=self._do_restore).grid(row=0, column=7, **pad)
        self.scan_tree = ttk.Treeview(addr, columns=("nonce", "component", "width", "last", "pos"), show="headings", height=3, selectmode="browse")
        for col, text, w in (("nonce", "Nonce", 90), ("component", "Component", 90), ("width", "Tape width", 80),
                             ("last", "Last addr", 70), ("pos", "Pos X", 70)):
            self.scan_tree.heading(col, text=text)
            self.scan_tree.column(col, width=w, anchor="center")
        self.scan_tree.grid(row=1, column=0, columnspan=5, sticky="w", **pad)
        ttk.Label(addr, text="Slot position X:").grid(row=2, column=0, columnspan=2, sticky="e", **pad)
        ttk.Entry(addr, textvariable=self.posx_var, width=8).grid(row=2, column=2, sticky="w", **pad)
        ttk.Button(addr, text="Save on feeder", command=self._do_set_position).grid(row=2, column=3, columnspan=2, **pad)
        ttk.Button(addr, text="Read", command=lambda: self._quick(CMD_GET_POSITION)).grid(row=2, column=5, **pad)
        self.scan_tree.bind("<<TreeviewSelect>>", self._on_scan_select)
        ttk.Label(addr, foreground=COLOR_DIM, wraplength=330, justify="left", text=(
            "Scan lists every unassigned feeder. Pick one and Assign, or use Scan + assign when exactly one answers. "
            "Addresses are disposable: they're forgotten at power-off.")
        ).grid(row=1, column=5, columnspan=2, sticky="w", **pad)

        misc = ttk.LabelFrame(tab, text="Tape width and status LED")
        misc.grid(row=1, column=0, sticky="ew", **pad)
        ttk.Label(misc, text="Tape width (mm):").grid(row=0, column=0, sticky="e", **pad)
        ttk.Combobox(misc, textvariable=self.width_var, width=6, state="readonly",
                     values=["8", "12", "16", "24", "32", "44", "56"]).grid(row=0, column=1, sticky="w", **pad)
        ttk.Button(misc, text="Write", command=self._do_write_width).grid(row=0, column=2, **pad)
        ttk.Button(misc, text="Read", command=lambda: self._quick(CMD_GET_HW_INFO)).grid(row=0, column=3, **pad)

        ttk.Label(misc, text="LED brightness:").grid(row=1, column=0, sticky="e", **pad)
        self.led_var = tk.StringVar(value="40")
        ttk.Spinbox(misc, textvariable=self.led_var, from_=1, to=255, width=6).grid(row=1, column=1, sticky="w", **pad)
        ttk.Button(misc, text="Set (v0.02b+)", command=lambda: self._do_set_led(self.led_var.get())).grid(row=1, column=2, columnspan=2, **pad)
        presets = ttk.Frame(misc)
        presets.grid(row=1, column=4, columnspan=4, sticky="w")
        for level in (10, 20, 40, 80, 160, 255):
            ttk.Button(presets, text=str(level), width=4, command=lambda v=level: self._do_set_led(str(v))).grid(row=0, column=presets.grid_size()[0], padx=1)
        ttk.Label(misc, foreground=COLOR_DIM, text="Saved on the feeder. 40 is the default; an SK6812 channel draws ~18 mA at 255.").grid(
            row=2, column=0, columnspan=8, sticky="w", **pad)

        ee = ttk.LabelFrame(tab, text="EEPROM / serial")
        ee.grid(row=2, column=0, sticky="ew", **pad)
        ttk.Label(ee, foreground="gray25", wraplength=700, justify="left", text=(
            "If Get serial answers NACK ERR_I2C but everything else works, the EEPROM is probably a plain AT24C02: "
            "it has no factory serial page, so there is nothing to read. I2C scan tells you which it is. On a plain "
            "AT24C02 you can program your own serial (v0.02+). An AT24CS02's factory serial is read-only.")
        ).grid(row=0, column=0, columnspan=6, sticky="w", **pad)
        ttk.Button(ee, text="I2C scan", command=lambda: self._quick(CMD_I2C_SCAN)).grid(row=1, column=0, **pad)
        ttk.Button(ee, text="Get serial", command=lambda: self._quick(CMD_GET_SERIAL)).grid(row=1, column=1, **pad)
        ttk.Label(ee, text="Serial (32 hex):").grid(row=2, column=0, sticky="e", **pad)
        self.serial_var = tk.StringVar()
        ttk.Entry(ee, textvariable=self.serial_var, width=40, font=self.mono_font).grid(row=2, column=1, columnspan=3, sticky="w", **pad)
        ttk.Button(ee, text="Random", command=lambda: self.serial_var.set(secrets.token_hex(16).upper())).grid(row=2, column=4, **pad)
        ttk.Button(ee, text="Program serial", command=self._do_program_serial).grid(row=2, column=5, **pad)
        ttk.Label(ee, foreground=COLOR_DIM, text="Write it down or keep the log: it is the feeder's identity.").grid(
            row=3, column=1, columnspan=5, sticky="w", **pad)
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
        port = self.port_var.get().strip()
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
            self._set_step("connect", "fail", str(exc)[:60])
            return
        self.status_var.set("Connected")
        self.status_label.configure(foreground=COLOR_OK)
        self.connect_btn_var.set("Disconnect")
        self.rx_wait_since = time.monotonic()
        self._set_step("connect", "ok", f"{port} @ {baud}")
        self._log(f"Connected to {port} @ {baud} baud" + (" (RTS-controlled TX)" if self.rts_var.get() else ""), "info")
        self._save_settings()

    def _disconnect(self):
        self.abort.set()
        if self.link is not None:
            self.link.close()
            self.link = None
        self.status_var.set("Disconnected")
        self.status_label.configure(foreground="red")
        self.connect_btn_var.set("Connect")
        self.last_rx = None
        self.poll_inflight = False
        self.reading_config = False
        self._clear_live()
        self._reset_steps()
        self._log("Disconnected.", "info")

    def _on_close(self):
        self._save_settings()
        self.prod_abort.set()
        if self.jig is not None:
            self.jig.close()
        self._disconnect()
        self.root.destroy()

    def _on_addr_changed(self):
        """A different target means everything on the card is about another feeder."""
        self.last_rx = None
        self.rx_wait_since = time.monotonic()
        self._clear_live()
        for key in ("ping", "status", "width", "pitch", "zero", "feed", "peelrate"):
            self._set_step(key, None, "")
        self._update_preview()

    # ---------------------------
    # Fire-and-forget sends (GUI thread) - replies just show up in the log
    # ---------------------------
    def _require_link(self) -> bool:
        if self.link is None:
            messagebox.showwarning("Not connected", "Connect to a port first.")
            return False
        if self.worker is not None and self.worker.is_alive():
            self._log("A sequence is running - wait for it or press STOP first.", "err")
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

    def _target_addr(self):
        """The Target address without logging - for background pollers. None if unusable."""
        try:
            addr = int(self.addr_var.get(), 0) & 0xFF
        except ValueError:
            return None
        return addr or None

    def _quick(self, cmd: int, payload: bytes = b"", addr: int = None):
        if addr is None:
            addr = self._parse_addr(self.addr_var, "target address")
            if addr is None:
                return
        self._send(addr, cmd, payload)

    def _stop_all(self):
        """Abort whatever sequence is running and broadcast CMD_STOP. Always allowed,
        even mid-sequence; a feeder only hears it while it is not driving a motor."""
        self.abort.set()
        if self.link is None:
            return
        try:
            self.link.send(0x00, CMD_STOP, b"")
        except serial.SerialException as exc:
            self._log(f"Write failed: {exc}", "err")
            return
        self._log("STOP: sequence aborted, CMD_STOP broadcast (a feeder only hears it between moves).", "err")

    # ---------------------------
    # Worker sequences - one at a time, wait for each reply
    # ---------------------------
    def _ui(self, fn):
        self.inbox.put(("call", fn))

    def _start_worker(self, label: str, fn):
        if not self._require_link():
            return
        self.abort.clear()
        self.busy_var.set(f"Running: {label}  (STOP aborts)")

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

    # ---------------------------
    # Live card: polling, rx handling
    # ---------------------------
    def _tick(self):
        now = time.monotonic()
        if self.link is None:
            self._set_ind("link", "off", "disconnected")
        elif self.last_rx is None:
            if self.autopoll_var.get() and now - self.rx_wait_since > LINK_STALE_S:
                self._set_ind("link", "bad", "no reply - wrong address?")
            else:
                self._set_ind("link", "off", "no reply yet")
        else:
            age = now - self.last_rx
            if age <= LINK_STALE_S:
                self._set_ind("link", "ok", "answering" if age < 1.5 else f"answered {age:.0f} s ago")
            elif self.autopoll_var.get():
                self._set_ind("link", "bad", f"no reply for {age:.0f} s")
            else:
                self._set_ind("link", "warn", f"last reply {age:.0f} s ago")

        busy = self.worker is not None and self.worker.is_alive()
        if (self.link is not None and self.autopoll_var.get() and not self.poll_inflight and not busy
                and now - self.last_poll >= STATUS_POLL_S):
            addr = self._target_addr()
            if addr is not None:
                self._start_poll(addr)
        self.root.after(250, self._tick)

    def _start_poll(self, addr):
        link = self.link
        self.poll_inflight = True
        self.last_poll = time.monotonic()

        def run():
            try:
                link.request(addr, CMD_GET_STATUS, b"",
                             lambda f: f.addr == addr and f.cmd in (CMD_STATUS_INFO, CMD_NACK), 0.7)
            except (serial.SerialException, OSError):
                pass
            finally:
                self._ui(lambda: setattr(self, "poll_inflight", False))

        threading.Thread(target=run, daemon=True).start()

    def _handle_rx(self, frame) -> bool:
        """Update the card from a frame off the bus. Returns True if it should be logged."""
        target = self._target_addr()
        quiet = False
        if frame.cmd == CMD_DISCOVER_HERE and len(frame.payload) >= 5:
            self._note_scan_reply(frame.payload)
        if target is not None and frame.addr == target and frame.cmd >= 0x80:
            self.last_rx = time.monotonic()
            quiet = self._absorb(frame)
        return not quiet

    def _absorb(self, frame) -> bool:
        """Feed a reply from the target into the card and the checklist. Returns
        True when it is background chatter (a live-refresh or config-read reply)
        that should stay out of the log."""
        p = frame.payload
        cmd = frame.cmd
        if cmd == CMD_PONG:
            self._set_step("ping", "ok", "")
            self._set_step("address", "ok", f"addr {frame.addr}")
            return False
        if cmd == CMD_STATUS_INFO:
            st = parse_status(p)
            if st is None:
                return False
            self._update_live(st)
            self._set_step("address", "ok", f"addr {frame.addr}")
            if self.config_read_for != frame.addr and not (self.worker and self.worker.is_alive()):
                self.config_read_for = frame.addr
                self._do_read_config(auto=True)
            return self.poll_inflight
        if cmd == CMD_COMPONENT_INFO and len(p) >= 5:
            comp, zero, half = (p[0] << 8) | p[1], (p[2] << 8) | p[3], p[4]
            self.cfg_vars["component"].set("not set" if comp == 0xFFFF else str(comp))
            self.cfg_vars["zero"].set("not set" if zero == 0xFFFF else f"raw {zero} ({zero * 360.0 / 4096.0:.1f}°)")
            self.cfg_vars["pitch"].set("not set" if half == 0xFF else f"{half * 2} mm")
            self._set_step("zero", None if zero == 0xFFFF else "ok", "not set" if zero == 0xFFFF else f"raw {zero}")
            self._set_step("pitch", None if half == 0xFF else "ok", "not set" if half == 0xFF else f"{half * 2} mm")
        elif cmd == CMD_HW_INFO and len(p) >= 1:
            self.cfg_vars["width"].set("not set" if p[0] == 0xFF else f"{p[0]} mm")
            self._set_step("width", None if p[0] == 0xFF else "ok", "not set" if p[0] == 0xFF else f"{p[0]} mm")
        elif cmd == CMD_PEEL_TIME_INFO and len(p) >= 2:
            ms = (p[0] << 8) | p[1]
            self.cfg_vars["peeltime"].set("not set" if ms == 0xFFFF else f"{ms} ms")
            self.cfg_state["peel_ms"] = ms
        elif cmd == CMD_PEEL_RATE_INFO and len(p) >= 2:
            t = (p[0] << 8) | p[1]
            self.cfg_state["peel_rate"] = t
            self.cfg_vars["peelrate"].set("off (no coupling)" if t == 0xFFFF else f"{t / 10:.1f} ms/mm")
            self.cal_onfeeder_var.set("off" if t == 0xFFFF else f"{t / 10:.1f} ms/mm")
            self._set_step("peelrate", None if t == 0xFFFF else "ok", "off" if t == 0xFFFF else f"{t / 10:.1f} ms/mm")
            if t != 0xFFFF and not self.rate_var.get():
                self.rate_var.set(f"{t / 10:g}")
        return self.reading_config

    def _update_live(self, st):
        mag = st["magnet"]
        self._set_ind("magnet", {"OK": "ok", "NONE": "bad"}.get(mag, "warn"),
                      {"OK": "detected", "NONE": "NO MAGNET", "WEAK": "too weak", "STRONG": "too strong"}[mag])
        self._set_ind("driver", "bad" if st["fault"] else "ok", "FAULT" if st["fault"] else "ok")
        if st["relay"] is None:
            self._set_ind("relay", "off", "n/a (older firmware)")
        else:
            self._set_ind("relay", "ok" if st["relay"] else "warn", "bus connected" if st["relay"] else "bus disconnected")
        self._set_ind("angle", "off", f"{st['angle_deg']:.1f}°  (raw {st['angle_raw']})")
        if st["imon_raw"] is None:
            self._set_ind("current", "off", "n/a (older firmware)")
        else:
            self._set_ind("current", "off", f"≈{st['imon_raw'] * IMON_DEFAULT_MA_PER_COUNT:.0f} mA  (raw {st['imon_raw']})")
        self._set_ind("lasterr", "off" if st["last_err"] == "ERR_NONE" else "warn",
                      "ok" if st["last_err"] == "ERR_NONE" else st["last_err"])
        if mag == "OK" and not st["fault"]:
            self._set_step("status", "ok", "magnet ok, no fault")
        else:
            why = "no magnet" if mag == "NONE" else (f"magnet {mag.lower()}" if mag != "OK" else "")
            if st["fault"]:
                why = (why + ", " if why else "") + "driver fault"
            self._set_step("status", "fail", why)

    # --- scan results ---
    def _note_scan_reply(self, p):
        nonce = (p[0] << 8) | p[1]
        comp = (p[2] << 8) | p[3]
        key = f"0x{nonce:04X}"
        if key in self.scan_tree.get_children():
            return
        last, pos = "-", "-"
        if len(p) >= 8:  # v0.02b+: slot identity
            last = "-" if p[5] == 0 else p[5]
            pos = "-" if ((p[6] << 8) | p[7]) == 0xFFFF else ((p[6] << 8) | p[7])
        self.scan_tree.insert("", "end", iid=key, values=(
            key, "not set" if comp == 0xFFFF else comp, "not set" if p[4] == 0xFF else f"{p[4]} mm", last, pos))
        self.nonce_var.set(key)
        self.scan_tree.selection_set(key)

    def _on_scan_select(self, _event=None):
        sel = self.scan_tree.selection()
        if sel:
            self.nonce_var.set(sel[0])

    # --- Address setup ---
    def _do_scan(self):
        self.scan_tree.delete(*self.scan_tree.get_children())
        self._quick(CMD_DISCOVER, addr=0x00)

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
            self._set_step_ui("address", "fail", "no ACK")
            return False
        self._log(f"   feeder 0x{nonce:04X} is now addr {new_addr} - Target addr set", "result")

        def adopt():
            self.addr_var.set(str(new_addr))
            self._set_step("address", "ok", f"addr {new_addr}")
            key = f"0x{nonce:04X}"
            if key in self.scan_tree.get_children():
                self.scan_tree.delete(key)
        self._ui(adopt)
        return True

    def _do_scan_assign(self):
        new_addr = self._parse_addr(self.new_addr_var, "new address")
        if new_addr is not None:
            self.scan_tree.delete(*self.scan_tree.get_children())
            self._start_worker("scan + assign", lambda: self._scan_assign(new_addr))

    def _scan_assign(self, new_addr: int):
        self._log(self._tx_text(build_frame(0, CMD_DISCOVER), 0, CMD_DISCOVER, b""), "tx")
        replies, _ = self.link.request(0x00, CMD_DISCOVER, b"", lambda f: f.cmd == CMD_DISCOVER_HERE,
                                       DISCOVER_WINDOW_S, cancel=self.abort, collect=True)
        if not replies:
            self._log("   nobody answered - no unassigned feeders. Already assigned? Try Ping on its address.", "err")
            self._set_step_ui("address", "fail", "no unassigned feeder answered")
            return
        if len(replies) > 1:
            self._log(f"   {len(replies)} feeders answered - pick one in Setup > Scan and Assign it.", "err")
            self._set_step_ui("address", "warn", f"{len(replies)} feeders answered: use Setup > Scan")
            return
        p = replies[0].payload
        self._assign((p[0] << 8) | p[1], new_addr)

    def _do_restore(self):
        """Scan, then re-assign every feeder the address it had before the power cycle -
        only when that is unambiguous (no two feeders claim the same address)."""
        self.scan_tree.delete(*self.scan_tree.get_children())
        self._start_worker("restore addresses", self._restore)

    def _restore(self):
        self._log(self._tx_text(build_frame(0, CMD_DISCOVER), 0, CMD_DISCOVER, b""), "tx")
        replies, _ = self.link.request(0x00, CMD_DISCOVER, b"", lambda f: f.cmd == CMD_DISCOVER_HERE,
                                       DISCOVER_WINDOW_S + 0.4, cancel=self.abort, collect=True)
        if not replies:
            self._log("   nobody answered - nothing unassigned.", "err")
            return
        found = []
        for f in replies:
            p = f.payload
            if len(p) < 8:
                self._log("   a feeder answered without slot info (firmware older than v0.02b): can't restore it.", "err")
                continue
            found.append(((p[0] << 8) | p[1], p[5], (p[6] << 8) | p[7]))
        claims = {}
        for nonce, last, pos in found:
            claims.setdefault(last, []).append(nonce)
        restored, skipped = 0, 0
        for nonce, last, pos in found:
            if last == 0:
                self._log(f"   0x{nonce:04X}: never had an address - assign it by hand.", "info")
                skipped += 1
            elif len(claims[last]) > 1:
                self._log(f"   0x{nonce:04X}: address {last} is claimed by {len(claims[last])} feeders - not restoring it.", "err")
                skipped += 1
            elif self._assign(nonce, last):
                restored += 1
            else:
                skipped += 1
        self._log(f"Restore: {restored} restored, {skipped} left unassigned. Check Pos X against your layout before trusting it.", "result")

    def _do_set_position(self):
        addr = self._parse_addr(self.addr_var, "target address")
        if addr is None:
            return
        try:
            x = int(self.posx_var.get(), 0)
            if not 0 <= x <= 0xFFFF:
                raise ValueError
        except ValueError:
            self._log(f"Bad position: '{self.posx_var.get()}' (0-65535)", "err")
            return

        def run():
            frame, _ = self._req(addr, CMD_SET_POSITION, struct.pack(">H", x))
            if frame is not None and frame.cmd == CMD_ACK:
                self._req(addr, CMD_GET_POSITION, expect=(CMD_POSITION_INFO,))
        self._start_worker("save position", run)

    def _do_ping(self):
        addr = self._parse_addr(self.addr_var, "target address")
        if addr is None:
            return

        def run():
            frame, _ = self._req(addr, CMD_PING, expect=(CMD_PONG,))
            if frame is None or frame.cmd != CMD_PONG:
                self._set_step_ui("ping", "fail", "no reply - wrong address, or not assigned yet?")
        self._start_worker("ping", run)

    # --- Config read ---
    def _do_read_config(self, auto=False):
        addr = self._target_addr() if auto else self._parse_addr(self.addr_var, "target address")
        if addr is None:
            return
        reads = ((CMD_GET_COMPONENT, CMD_COMPONENT_INFO, ("component", "zero", "pitch")),
                 (CMD_GET_HW_INFO, CMD_HW_INFO, ("width",)),
                 (CMD_GET_PEEL_TIME, CMD_PEEL_TIME_INFO, ("peeltime",)),
                 (CMD_GET_PEEL_RATE, CMD_PEEL_RATE_INFO, ("peelrate",)))

        def run():
            link = self.link
            if link is None:
                return
            self.reading_config = True
            missing = []
            try:
                for cmd, reply, keys in reads:
                    if self.abort.is_set():
                        raise Aborted()
                    frame, _ = link.request(addr, cmd, b"", lambda f, r=reply: f.addr == addr and f.cmd in (r, CMD_NACK),
                                            0.6, cancel=self.abort)
                    if frame is None or frame.cmd == CMD_NACK:
                        missing.extend(keys)
            finally:
                # Reset through the inbox so it lands after every reply this loop just queued
                # (the GUI thread decides what to log when it processes them, not when they arrive).
                self._ui(lambda: setattr(self, "reading_config", False))
            if missing:
                def mark():
                    for k in missing:
                        self.cfg_vars[k].set("n/a (older firmware?)")
                self._ui(mark)
            if not auto:
                self._log(f"Read saved config from addr {addr}" + (f" ({len(missing)} item(s) not supported)" if missing else ""), "info")
        self._start_worker("read config", run)

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
                self._req(addr, CMD_GET_COMPONENT, expect=(CMD_COMPONENT_INFO,))  # read it back; the card updates from it
            else:
                self._step_from_frame("pitch", frame)
        self._start_worker("set pitch", run)

    # --- Peel calibration helpers ---
    def _calib_set_net(self, ms):
        self.cal_net_ms = ms
        self.cal_net_var.set(f"{ms:+d} ms" if ms else "0 ms")

    def _calib_feed_plain(self):
        addr = self._parse_addr(self.addr_var, "target address")
        if addr is None:
            return
        try:
            mm = float(self.cal_mm_var.get())
            payload = COMMAND_SPECS[CMD_JOG].encode([str(mm)])
        except ValueError as exc:
            self._log(f"Bad feed distance: {exc}", "err")
            return
        self._start_worker(f"feed {mm:g} mm, no peel", lambda: self._req(
            addr, CMD_JOG, payload, timeout_s=FEED_REPLY_TIMEOUT_S))

    def _calib_nudge(self, direction, ms):
        addr = self._parse_addr(self.addr_var, "target address")
        if addr is None:
            return
        payload = bytes([direction, ms // 10])

        def run():
            frame, _ = self._req(addr, CMD_PEEL, payload, timeout_s=ms / 1000.0 + PEEL_REPLY_MARGIN_S)
            if frame is not None and frame.cmd == CMD_ACK:
                delta = -ms if direction else ms
                self._ui(lambda: self._calib_set_net(self.cal_net_ms + delta))
        self._start_worker(f"peel {'back ' if direction else ''}{ms} ms", run)

    def _calib_record(self):
        try:
            mm = float(self.cal_mm_var.get())
        except ValueError:
            self._log("Set the feed distance first.", "err")
            return
        if self.cal_net_ms <= 0 or mm <= 0:
            self._log("Nothing to record: peel forward until the tape is taut first (this round is not above 0 ms).", "err")
            return
        n = len(self.cal_tree.get_children()) + 1
        self.cal_tree.insert("", "end", values=(n, f"{mm:g}", self.cal_net_ms, f"{self.cal_net_ms / mm:.1f}"))
        self._calib_set_net(0)
        self._calib_update_avg()

    def _calib_rates(self):
        return [float(self.cal_tree.item(i, "values")[3]) for i in self.cal_tree.get_children()]

    def _calib_update_avg(self):
        rates = self._calib_rates()
        if not rates:
            self.cal_avg_var.set("Average: —")
            return
        spread = f"  (spread {min(rates):.1f}-{max(rates):.1f})" if len(rates) > 1 else ""
        self.cal_avg_var.set(f"Average: {statistics.mean(rates):.1f} ms/mm  n={len(rates)}{spread}")

    def _calib_use_average(self):
        rates = self._calib_rates()
        if not rates:
            self._log("Record at least one measurement first.", "err")
            return
        self.rate_var.set(f"{statistics.mean(rates):.1f}")

    def _calib_delete_row(self):
        for item in self.cal_tree.selection():
            self.cal_tree.delete(item)
        self._calib_update_avg()

    def _calib_clear(self):
        self.cal_tree.delete(*self.cal_tree.get_children())
        self._calib_update_avg()

    def _calib_adjust(self, pct):
        try:
            rate = float(self.rate_var.get())
        except ValueError:
            self._log("Enter a rate first (or press Read).", "err")
            return
        new = max(0.5, min(500.0, round(rate * (1 + pct / 100.0), 1)))
        if new == rate:  # a 1% step on a small rate rounds away - move by the 0.1 resolution instead
            new = max(0.5, min(500.0, round(rate + (0.1 if pct > 0 else -0.1), 1)))
        self.rate_var.set(f"{new:g}")
        self._do_set_peel_rate()

    def _do_feed_back(self):
        addr = self._parse_addr(self.addr_var, "target address")
        if addr is not None:
            self._start_worker("back one pitch", lambda: self._req(
                addr, CMD_FEED_BACK, timeout_s=FEED_REPLY_TIMEOUT_S))

    def _do_set_peel_rate(self, off=False):
        addr = self._parse_addr(self.addr_var, "target address")
        if addr is None:
            return
        try:
            tenths = 0 if off else int(round(float(self.rate_var.get()) * 10))
            if tenths != 0 and not 5 <= tenths <= 5000:
                raise ValueError
        except ValueError:
            self._log(f"Bad peel rate: '{self.rate_var.get()}' (0.5 to 500 ms per mm)", "err")
            return
        payload = struct.pack(">H", tenths)

        def run():
            frame, _ = self._req(addr, CMD_SET_PEEL_RATE, payload)
            if frame is not None and frame.cmd == CMD_ACK:
                self._req(addr, CMD_GET_PEEL_RATE, expect=(CMD_PEEL_RATE_INFO,))  # read it back
            elif frame is None or frame.cmd == CMD_NACK:
                self._log("   Peel rate needs v0.02b or newer firmware.", "err")
        self._start_worker("set peel rate", run)

    def _do_set_led(self, text):
        addr = self._parse_addr(self.addr_var, "target address")
        if addr is None:
            return
        try:
            level = int(text, 0)
            if not 1 <= level <= 255:
                raise ValueError
        except ValueError:
            self._log(f"Bad brightness: '{text}' (1-255)", "err")
            return
        self.led_var.set(str(level))
        self._start_worker("set LED brightness", lambda: self._req(addr, CMD_SET_LED_BRIGHTNESS, bytes([level])))

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
        rate = self.cfg_state.get("peel_rate")
        if "feed" in steps and "peel" in steps and rate not in (None, 0xFFFF):
            self._log(f"Warning: this feeder has a peel rate saved ({rate / 10:.1f} ms/mm), so each feed already "
                      f"peels. This cycle will peel twice per feed - use 'feed only' or turn the rate off.", "err")
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
                        self._step_from_frame("feed", frame, f"{ms:.0f} ms")
                    else:
                        frame, ms = self._req(addr, CMD_PEEL, peel_payload, timeout_s=peel_timeout)
                    if frame is None or frame.cmd != CMD_ACK:
                        code = frame.payload[:1] if frame is not None else b""
                        if code == b"\x06" and step == "feed":
                            self._log("   ERR_NOT_READY: no pitch set - use Set pitch first.", "err")
                        elif code == b"\x06" and step == "peel":
                            self._log("   ERR_NOT_READY: no saved peel time - use Save time to feeder first.", "err")
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
                self._req(addr, CMD_GET_PEEL_TIME, expect=(CMD_PEEL_TIME_INFO,))  # read it back
        self._start_worker("save peel time", run)

    def _do_peel_cal_run(self, direction):
        addr = self._parse_addr(self.addr_var, "target address")
        if addr is not None:
            self._start_worker("saved-time peel", lambda: self._req(
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
        if addr is None:
            return

        def run():
            frame, _ = self._req(addr, CMD_ZERO_HERE)
            if frame is not None and frame.cmd == CMD_ACK:
                self._req(addr, CMD_GET_COMPONENT, expect=(CMD_COMPONENT_INFO,))  # read it back; the card updates from it
            else:
                self._step_from_frame("zero", frame)
        self._start_worker("set zero", run)

    # --- EEPROM / width ---
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

        def run():
            frame, _ = self._req(addr, CMD_SET_HW_INFO, payload, timeout_s=2.0)
            if frame is not None and frame.cmd == CMD_ACK:
                self._req(addr, CMD_GET_HW_INFO, expect=(CMD_HW_INFO,))  # read it back; the card updates from it
            else:
                self._step_from_frame("width", frame)
        self._start_worker("write tape width", run)

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
                    if self._handle_rx(item):
                        self._write_log(f"RX  {item}", "rx")
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
