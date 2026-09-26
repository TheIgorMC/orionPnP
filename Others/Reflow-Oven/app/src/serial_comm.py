"""Serial communication manager for the RP2040 Pico over USB CDC.

Runs a background reader thread that pushes parsed JSON samples into two queues:
  - samples  : temperature readings  (keys: t, tc, ref, fault)
  - events   : non-sample messages   (keys: event, ...)

Usage:
    mgr = SerialManager()
    if mgr.connect_auto():
        ...
    sample = mgr.samples.get_nowait()   # non-blocking
    mgr.send_command({"cmd": "ping"})
    mgr.disconnect()
"""

from __future__ import annotations

import json
import queue
import threading
import time
from typing import Callable, Optional

import serial
import serial.tools.list_ports

# RP2040 USB VID; MicroPython and CircuitPython PIDs
_VID  = 0x2E8A
_PIDS = {0x0005, 0x000A}


def find_rp2040_port() -> Optional[str]:
    """Return the first COM port matching an RP2040 VID/PID, or None."""
    for p in serial.tools.list_ports.comports():
        if p.vid == _VID and p.pid in _PIDS:
            return p.device
    return None


def list_ports() -> list[str]:
    """Return all available COM port names."""
    return [p.device for p in serial.tools.list_ports.comports()]


class SerialManager:
    def __init__(
        self,
        baud: int = 115200,
        on_connect: Optional[Callable[[str], None]] = None,
        on_disconnect: Optional[Callable[[], None]] = None,
    ):
        self.baud = baud
        self.samples: queue.Queue = queue.Queue(maxsize=1000)
        self.events:  queue.Queue = queue.Queue(maxsize=200)
        self.connected: bool = False

        self._port: Optional[str] = None
        self._serial: Optional[serial.Serial] = None
        self._thread: Optional[threading.Thread] = None
        self._stop = threading.Event()
        self.on_connect    = on_connect
        self.on_disconnect = on_disconnect

    # ── Public API ───────────────────────────────────────────────────────

    def connect(self, port: str) -> None:
        """Connect to *port* and start the background reader thread."""
        self.disconnect()
        self._port = port
        self._stop.clear()
        self._thread = threading.Thread(target=self._reader_loop, daemon=True)
        self._thread.start()

    def connect_auto(self) -> bool:
        """Auto-detect the RP2040 and connect. Returns True on success."""
        port = find_rp2040_port()
        if port:
            self.connect(port)
            return True
        return False

    def disconnect(self) -> None:
        """Stop the reader thread and close the serial port."""
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=2.0)
        if self._serial and self._serial.is_open:
            self._serial.close()
        self._serial = None
        self.connected = False

    def send_command(self, cmd: dict) -> None:
        """Write a JSON command line to the Pico (fire-and-forget)."""
        if self._serial and self._serial.is_open:
            try:
                self._serial.write((json.dumps(cmd) + "\n").encode())
            except serial.SerialException:
                pass

    # ── Internal ─────────────────────────────────────────────────────────

    def _reader_loop(self) -> None:
        while not self._stop.is_set():
            try:
                ser = serial.Serial(self._port, self.baud, timeout=1.0)
                self._serial = ser
                self.connected = True
                if self.on_connect:
                    self.on_connect(self._port)

                while not self._stop.is_set():
                    raw = ser.readline()
                    if not raw:
                        continue
                    line = raw.decode("utf-8", errors="ignore").strip()
                    if not line:
                        continue
                    try:
                        data = json.loads(line)
                        if "tc" in data:
                            if not self.samples.full():
                                self.samples.put_nowait(data)
                        else:
                            if not self.events.full():
                                self.events.put_nowait(data)
                    except json.JSONDecodeError:
                        pass

            except (serial.SerialException, OSError):
                pass
            finally:
                if self._serial and self._serial.is_open:
                    self._serial.close()
                was_connected = self.connected
                self.connected = False
                if was_connected and self.on_disconnect:
                    self.on_disconnect()
                if not self._stop.is_set():
                    time.sleep(2.0)  # brief pause before reconnect attempt
