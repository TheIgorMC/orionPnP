#!/usr/bin/env python3
"""
RS485 command sender GUI for the OrionPnP feeder board protocol.

Tkinter (stdlib - no extra dependency beyond pyserial) front end for the
same protocol logic rs485_sender.py's CLI/REPL uses (see
rs485_protocol.py). Connects to a USB-RS485 adapter on a COM port, shows
every frame that comes off the bus in a live log, and lets you send
either a quick named command or a fully custom addr/cmd/payload frame.

Usage:
    python rs485_gui.py
"""
import queue
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
    PAYLOAD_HINTS,
    cmd_name,
    build_frame,
    decode_payload,
    FrameAssembler,
)

POLL_INTERVAL_MS = 50
READ_CHUNK_TIMEOUT_S = 0.1  # how often the reader thread wakes up to check the stop flag


class SerialLink:
    """Owns the open port + background reader thread. Frames (and read
    errors) land on `inbox` for the GUI thread to drain on its own
    schedule - keeps all Tk widget updates on the main thread."""

    def __init__(self, port: str, baud: int, rts_tx: bool):
        self.ser = serial.Serial(port, baud, timeout=READ_CHUNK_TIMEOUT_S)
        self.rts_tx = rts_tx
        self.inbox = queue.Queue()
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

    def send(self, addr: int, cmd: int, payload: bytes = b""):
        frame = build_frame(addr, cmd, payload)
        if self.rts_tx:
            self.ser.setRTS(True)
        self.ser.write(frame)
        self.ser.flush()
        if self.rts_tx:
            time.sleep(0.002)
            self.ser.setRTS(False)
        return frame

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


# (code, display name) pairs for the custom-command dropdown, sorted by code
CMD_CHOICES = [(code, f"0x{code:02X} {name}") for code, name in sorted(CMD_NAMES.items())
               if code < 0x80]  # requests only - replies are never something you'd send


class App:
    def __init__(self, root: tk.Tk):
        self.root = root
        self.root.title("OrionPnP Feeder RS485 Sender")
        self.link: SerialLink | None = None

        self._build_widgets()
        self._refresh_ports()
        self.root.protocol("WM_DELETE_WINDOW", self._on_close)
        self._poll_inbox()

    # ---------------------------
    # Layout
    # ---------------------------
    def _build_widgets(self):
        pad = {"padx": 4, "pady": 4}

        conn = ttk.LabelFrame(self.root, text="Connection")
        conn.grid(row=0, column=0, sticky="ew", **pad)
        conn.columnconfigure(5, weight=1)

        ttk.Label(conn, text="Port:").grid(row=0, column=0, **pad)
        self.port_var = tk.StringVar()
        self.port_combo = ttk.Combobox(conn, textvariable=self.port_var, width=12, state="readonly")
        self.port_combo.grid(row=0, column=1, **pad)
        ttk.Button(conn, text="Refresh", command=self._refresh_ports).grid(row=0, column=2, **pad)

        ttk.Label(conn, text="Baud:").grid(row=0, column=3, **pad)
        self.baud_var = tk.StringVar(value=str(DEFAULT_BAUD))
        ttk.Entry(conn, textvariable=self.baud_var, width=8).grid(row=0, column=4, **pad)

        self.rts_var = tk.BooleanVar(value=False)
        ttk.Checkbutton(conn, text="RTS controls TX (DE)", variable=self.rts_var).grid(row=0, column=5, sticky="w", **pad)

        self.connect_btn = ttk.Button(conn, text="Connect", command=self._toggle_connect)
        self.connect_btn.grid(row=0, column=6, **pad)

        self.status_var = tk.StringVar(value="Disconnected")
        self.status_label = ttk.Label(conn, textvariable=self.status_var, foreground="red")
        self.status_label.grid(row=0, column=7, **pad)

        quick = ttk.LabelFrame(self.root, text="Quick actions")
        quick.grid(row=1, column=0, sticky="ew", **pad)

        ttk.Label(quick, text="Addr:").grid(row=0, column=0, **pad)
        self.addr_var = tk.StringVar(value="1")
        ttk.Entry(quick, textvariable=self.addr_var, width=6).grid(row=0, column=1, **pad)

        ttk.Button(quick, text="Scan (CMD_DISCOVER)", command=self._do_scan).grid(row=0, column=2, **pad)
        ttk.Button(quick, text="Ping", command=self._do_ping).grid(row=0, column=3, **pad)
        ttk.Button(quick, text="Get Status", command=self._do_getstatus).grid(row=0, column=4, **pad)
        ttk.Button(quick, text="Stop", command=self._do_stop).grid(row=0, column=5, **pad)
        ttk.Button(quick, text="Identify", command=self._do_identify).grid(row=0, column=6, **pad)

        ttk.Label(quick, text="Nonce:").grid(row=1, column=0, **pad)
        self.nonce_var = tk.StringVar(value="0x0000")
        ttk.Entry(quick, textvariable=self.nonce_var, width=8).grid(row=1, column=1, **pad)
        ttk.Label(quick, text="New addr:").grid(row=1, column=2, **pad)
        self.new_addr_var = tk.StringVar(value="1")
        ttk.Entry(quick, textvariable=self.new_addr_var, width=6).grid(row=1, column=3, **pad)
        ttk.Button(quick, text="Assign (CMD_ASSIGN_ADDR)", command=self._do_assign).grid(row=1, column=4, **pad)
        ttk.Label(quick, text="(nonce from a Scan reply above)").grid(row=1, column=5, columnspan=2, sticky="w", **pad)

        custom = ttk.LabelFrame(self.root, text="Custom command")
        custom.grid(row=2, column=0, sticky="ew", **pad)
        custom.columnconfigure(5, weight=1)

        ttk.Label(custom, text="Addr:").grid(row=0, column=0, **pad)
        self.custom_addr_var = tk.StringVar(value="1")
        ttk.Entry(custom, textvariable=self.custom_addr_var, width=6).grid(row=0, column=1, **pad)

        ttk.Label(custom, text="Command:").grid(row=0, column=2, **pad)
        self.cmd_choice_var = tk.StringVar()
        self.cmd_combo = ttk.Combobox(custom, textvariable=self.cmd_choice_var, width=26, state="readonly",
                                       values=[label for _, label in CMD_CHOICES])
        self.cmd_combo.current(0)
        self.cmd_combo.grid(row=0, column=3, **pad)
        self.cmd_combo.bind("<<ComboboxSelected>>", self._on_cmd_choice)

        ttk.Label(custom, text="Payload (hex bytes):").grid(row=1, column=0, **pad)
        self.payload_var = tk.StringVar()
        ttk.Entry(custom, textvariable=self.payload_var, width=30).grid(row=1, column=1, columnspan=3, sticky="ew", **pad)
        ttk.Button(custom, text="Send", command=self._do_custom_send).grid(row=1, column=4, **pad)

        self.hint_var = tk.StringVar()
        ttk.Label(custom, textvariable=self.hint_var, foreground="gray25").grid(row=2, column=0, columnspan=5, sticky="w", **pad)
        self._on_cmd_choice()

        log_frame = ttk.LabelFrame(self.root, text="Log")
        log_frame.grid(row=3, column=0, sticky="nsew", **pad)
        self.root.rowconfigure(3, weight=1)
        self.root.columnconfigure(0, weight=1)
        log_frame.rowconfigure(0, weight=1)
        log_frame.columnconfigure(0, weight=1)

        self.log = scrolledtext.ScrolledText(log_frame, height=18, state="disabled", wrap="word")
        self.log.grid(row=0, column=0, columnspan=4, sticky="nsew", **pad)
        self.log.tag_configure("tx", foreground="#1a5fb4")
        self.log.tag_configure("rx", foreground="#26a269")
        self.log.tag_configure("err", foreground="#c01c28")
        self.log.tag_configure("info", foreground="gray40")

        self.verbose_var = tk.BooleanVar(value=False)
        ttk.Checkbutton(log_frame, text="Show raw TX bytes", variable=self.verbose_var).grid(row=1, column=0, sticky="w", **pad)
        ttk.Button(log_frame, text="Clear", command=self._clear_log).grid(row=1, column=1, **pad)
        ttk.Button(log_frame, text="Save...", command=self._save_log).grid(row=1, column=2, **pad)

    # ---------------------------
    # Connection management
    # ---------------------------
    def _refresh_ports(self):
        ports = [p.device for p in serial.tools.list_ports.comports()]
        self.port_combo["values"] = ports
        if ports and not self.port_var.get():
            self.port_var.set(ports[0])

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
            self.link = SerialLink(port, baud, self.rts_var.get())
        except serial.SerialException as exc:
            messagebox.showerror("Connect failed", str(exc))
            return
        self.status_var.set(f"Connected ({port} @ {baud})")
        self.status_label.configure(foreground="#26a269")
        self.connect_btn.configure(text="Disconnect")
        self._log(f"Connected to {port} @ {baud} baud" + (" (RTS-controlled TX)" if self.rts_var.get() else ""), "info")

    def _disconnect(self):
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
    # Sending helpers
    # ---------------------------
    def _require_link(self) -> bool:
        if self.link is None:
            messagebox.showwarning("Not connected", "Connect to a port first.")
            return False
        return True

    def _send(self, addr: int, cmd: int, payload: bytes = b""):
        if not self._require_link():
            return
        try:
            frame = self.link.send(addr, cmd, payload)
        except serial.SerialException as exc:
            self._log(f"Write failed: {exc}", "err")
            self._disconnect()
            return
        if self.verbose_var.get():
            self._log(f"TX: {frame.hex(' ')}  (addr=0x{addr:02X} cmd={cmd_name(cmd)})", "tx")
        else:
            self._log(f"TX  addr=0x{addr:02X} cmd={cmd_name(cmd)} "
                       f"payload={payload.hex(' ') if payload else '(empty)'}", "tx")

    def _parse_addr(self, var: tk.StringVar, label: str):
        try:
            return int(var.get(), 0) & 0xFF
        except ValueError:
            self._log(f"Bad {label}: '{var.get()}'", "err")
            return None

    # ---------------------------
    # Quick actions
    # ---------------------------
    def _do_scan(self):
        self._send(0x00, 0x10)

    def _do_ping(self):
        addr = self._parse_addr(self.addr_var, "address")
        if addr is not None:
            self._send(addr, 0x01)

    def _do_getstatus(self):
        addr = self._parse_addr(self.addr_var, "address")
        if addr is not None:
            self._send(addr, 0x30)

    def _do_stop(self):
        addr = self._parse_addr(self.addr_var, "address")
        if addr is not None:
            self._send(addr, 0x31)

    def _do_identify(self):
        addr = self._parse_addr(self.addr_var, "address")
        if addr is not None:
            self._send(addr, 0x32, bytes([0]))

    def _do_assign(self):
        try:
            nonce = int(self.nonce_var.get(), 0) & 0xFFFF
        except ValueError:
            self._log(f"Bad nonce: '{self.nonce_var.get()}'", "err")
            return
        new_addr = self._parse_addr(self.new_addr_var, "new address")
        if new_addr is None:
            return
        self._send(0x00, 0x11, bytes([(nonce >> 8) & 0xFF, nonce & 0xFF, new_addr]))

    # ---------------------------
    # Custom command
    # ---------------------------
    def _on_cmd_choice(self, _event=None):
        idx = self.cmd_combo.current()
        if idx < 0:
            return
        code, _ = CMD_CHOICES[idx]
        self.hint_var.set(f"payload: {PAYLOAD_HINTS.get(code, '(unknown)')}")

    def _do_custom_send(self):
        addr = self._parse_addr(self.custom_addr_var, "address")
        if addr is None:
            return
        idx = self.cmd_combo.current()
        if idx < 0:
            self._log("Choose a command first.", "err")
            return
        code, _ = CMD_CHOICES[idx]
        tokens = self.payload_var.get().replace(",", " ").split()
        try:
            payload = bytes(_parse_hex_token(t) for t in tokens)
        except ValueError:
            self._log(f"Bad payload bytes: '{self.payload_var.get()}'", "err")
            return
        self._send(addr, code, payload)

    # ---------------------------
    # Log / inbox
    # ---------------------------
    def _log(self, text: str, tag: str = None):
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
        if self.link is not None:
            try:
                while True:
                    kind, payload = self.link.inbox.get_nowait()
                    if kind == "frame":
                        self._log(f"RX  {payload}", "rx")
                    elif kind == "error":
                        self._log(f"Read error: {payload}", "err")
                        self._disconnect()
                        break
            except queue.Empty:
                pass
        self.root.after(POLL_INTERVAL_MS, self._poll_inbox)


def main():
    root = tk.Tk()
    App(root)
    root.mainloop()


if __name__ == "__main__":
    main()
