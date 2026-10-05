"""
Shared RS485 feeder-protocol primitives - frame building, CRC8, payload
decoding, and incremental frame parsing. Used by both rs485_sender.py
(CLI/REPL) and rs485_gui.py (Tkinter GUI) so the protocol logic only
exists in one place. No pyserial dependency here - FrameReader works
with anything exposing pyserial's read()/timeout interface, and
FrameAssembler is pure bytes-in/Frame-out with no I/O at all.

See ../PROTOCOL.md for the actual spec this implements against:

    [0xAA][ADDR][CMD][LEN][PAYLOAD...][CRC8]

CRC8: poly 0x07, computed over ADDR..PAYLOAD - matches the firmware's
crc8() in src/main.cpp exactly (bit-by-bit MSB-first, no table, no
init/final XOR).
"""
import os
import re
import time

FRAME_START = 0xAA
DEFAULT_BAUD = 9600  # fallback only - firmware_bauds() reads the real value out of each firmware's source
MAX_PAYLOAD = 16

CMD_NAMES = {
    0x01: "CMD_PING", 0x81: "CMD_PONG",
    0x10: "CMD_DISCOVER", 0x90: "CMD_DISCOVER_HERE",
    0x11: "CMD_ASSIGN_ADDR",
    0x20: "CMD_GET_COMPONENT", 0xA0: "CMD_COMPONENT_INFO",
    0x21: "CMD_SET_COMPONENT",
    0x22: "CMD_SET_FEED_CONFIG",
    0x23: "CMD_RESET_CONFIG",
    0x24: "CMD_ZERO_HERE",
    0x25: "CMD_SET_PITCH_MM",
    0x26: "CMD_FEED_NEXT",
    0x27: "CMD_SET_EXT_LED",
    0x28: "CMD_SET_INVERT_DIR",
    0x29: "CMD_GET_HW_INFO", 0xA1: "CMD_HW_INFO",
    0x2A: "CMD_SET_HW_INFO",
    0x30: "CMD_GET_STATUS", 0xA2: "CMD_STATUS_INFO",
    0x31: "CMD_STOP",
    0x32: "CMD_IDENTIFY",
    0x33: "CMD_GET_SERIAL", 0xA3: "CMD_SERIAL_INFO",
    0x34: "CMD_PEEL",
    0x35: "CMD_SET_PEEL_TIME",
    0x36: "CMD_GET_PEEL_TIME", 0xA4: "CMD_PEEL_TIME_INFO",
    0x37: "CMD_JOG",
    0x38: "CMD_I2C_SCAN", 0xA5: "CMD_I2C_SCAN_INFO",
    0x39: "CMD_SET_SERIAL",
    0x82: "CMD_ACK",
    0x83: "CMD_NACK",
}

ERROR_CODES = {
    0x00: "ERR_NONE",
    0x01: "ERR_FAULT",
    0x02: "ERR_MAGNET_LOST",
    0x03: "ERR_STALL",
    0x04: "ERR_TIMEOUT",
    0x05: "ERR_BAD_PARAM",
    0x06: "ERR_NOT_READY",
    0x07: "ERR_I2C",
    0x08: "ERR_LOCKED",
}

def cmd_name(code):
    return CMD_NAMES.get(code, f"0x{code:02X}")


def crc8(data: bytes) -> int:
    crc = 0
    for byte in data:
        crc ^= byte
        for _ in range(8):
            if crc & 0x80:
                crc = ((crc << 1) ^ 0x07) & 0xFF
            else:
                crc = (crc << 1) & 0xFF
    return crc


def build_frame(addr: int, cmd: int, payload: bytes = b"") -> bytes:
    if len(payload) > MAX_PAYLOAD:
        raise ValueError(f"payload too long ({len(payload)} bytes, max {MAX_PAYLOAD})")
    body = bytes([addr & 0xFF, cmd & 0xFF, len(payload)]) + payload
    return bytes([FRAME_START]) + body + bytes([crc8(body)])


class Frame:
    def __init__(self, addr, cmd, payload):
        self.addr = addr
        self.cmd = cmd
        self.payload = payload

    def __str__(self):
        return (f"addr=0x{self.addr:02X} cmd={cmd_name(self.cmd)} "
                f"payload={self.payload.hex(' ') if self.payload else '(empty)'}"
                f"  | {decode_payload(self.cmd, self.payload)}")


def decode_payload(cmd: int, p: bytes) -> str:
    """Best-effort human-readable decode of a known reply's payload."""
    try:
        if cmd == 0x83 and len(p) >= 1:  # CMD_NACK
            return f"error={ERROR_CODES.get(p[0], f'0x{p[0]:02X}')}"
        if cmd == 0x90 and len(p) >= 5:  # CMD_DISCOVER_HERE
            nonce = (p[0] << 8) | p[1]
            comp = (p[2] << 8) | p[3]
            comp_s = "UNSET" if comp == 0xFFFF else str(comp)
            width = "UNSET" if p[4] == 0xFF else f"{p[4]}mm"
            return f"nonce=0x{nonce:04X} componentId={comp_s} tapeWidth={width}"
        if cmd == 0xA0 and len(p) >= 5:  # CMD_COMPONENT_INFO
            comp = (p[0] << 8) | p[1]
            zero = (p[2] << 8) | p[3]
            comp_s = "UNSET" if comp == 0xFFFF else str(comp)
            zero_s = "UNSET" if zero == 0xFFFF else str(zero)
            pitch_s = "UNSET" if p[4] == 0xFF else f"{p[4]} ({p[4] * 2}mm pitch)"
            return f"componentId={comp_s} tapeZeroRaw={zero_s} feedHalfTeeth={pitch_s}"
        if cmd == 0xA1 and len(p) >= 1:  # CMD_HW_INFO
            width = "UNSET" if p[0] == 0xFF else f"{p[0]}mm"
            return f"tapeWidth={width}"
        if cmd == 0xA2 and len(p) >= 5:  # CMD_STATUS_INFO
            angle_raw = (p[0] << 8) | p[1]
            angle_deg = angle_raw * 360.0 / 4096.0
            status = p[2]
            md = bool(status & 0x20)
            ml = bool(status & 0x10)
            mh = bool(status & 0x08)
            magnet = "NONE" if not md else ("WEAK" if ml else ("STRONG" if mh else "OK"))
            fault = bool(p[3])
            err = ERROR_CODES.get(p[4], f"0x{p[4]:02X}")
            out = (f"angle={angle_deg:.1f}deg (raw={angle_raw}) magnet={magnet} "
                   f"(MD={int(md)} ML={int(ml)} MH={int(mh)}) fault={int(fault)} lastMoveErr={err}")
            if len(p) >= 8:  # beta1+/v0.01a+: iMonRaw, relayEngaged
                i_mon_raw = (p[5] << 8) | p[6]
                relay = bool(p[7])
                out += f" iMonRaw={i_mon_raw} relay={'CONNECTED' if relay else 'disconnected'}"
            return out
        if cmd == 0xA3 and len(p) >= 1:  # CMD_SERIAL_INFO
            return f"serial={p.hex()}"
        if cmd == 0xA4 and len(p) >= 2:  # CMD_PEEL_TIME_INFO (v0.02+)
            ms = (p[0] << 8) | p[1]
            return "peelTime=UNCALIBRATED" if ms == 0xFFFF else f"peelTime={ms}ms"
        if cmd == 0xA5:  # CMD_I2C_SCAN_INFO (v0.02+)
            return describe_i2c_scan(p)
    except Exception as exc:  # malformed/short payload from a flaky link - don't crash the caller
        return f"(decode error: {exc})"
    return ""


I2C_KNOWN = {0x36: "AS5600 encoder"}


def i2c_label(a: int) -> str:
    if a in I2C_KNOWN:
        return I2C_KNOWN[a]
    if 0x50 <= a <= 0x57:
        return "AT24 EEPROM"
    if 0x58 <= a <= 0x5F:
        return "AT24CS02 serial page"
    return "unknown"


def describe_i2c_scan(p: bytes) -> str:
    """What answered on the feeder's I2C bus, plus what that implies for the
    serial number (a plain AT24C02 has no 0x58 serial page)."""
    if not p:
        return "no I2C devices answered"
    out = "i2c: " + ", ".join(f"0x{a:02X} ({i2c_label(a)})" for a in p)
    have_eeprom = any(0x50 <= a <= 0x57 for a in p)
    have_serial = any(0x58 <= a <= 0x5F for a in p)
    if not have_eeprom:
        out += " -> EEPROM not answering: check wiring/soldering of the AT24"
    elif not have_serial:
        out += " -> plain AT24C02 (no factory serial page): use Program serial"
    else:
        out += " -> AT24CS02 with factory serial"
    return out


class FrameAssembler:
    """
    Pure byte-at-a-time frame state machine, no I/O. feed(byte) advances
    one step and returns a completed Frame, or None if the frame isn't
    done yet (or was just dropped on a CRC mismatch - matches the
    firmware's own silent-drop-and-resync behaviour, no exception raised).

    State persists across feed() calls, so a byte stream can be fed in
    from anywhere - a background thread's live read loop (GUI) or a
    bounded read-with-deadline loop (FrameReader, below) - without losing
    partial progress at a call boundary.
    """

    def __init__(self):
        self._reset()

    def _reset(self):
        self.state = "START"
        self.addr = self.cmd = self.plen = 0
        self.payload = bytearray()

    def feed(self, byte: int):
        if self.state == "START":
            if byte == FRAME_START:
                self.state = "ADDR"
            return None
        if self.state == "ADDR":
            self.addr = byte
            self.state = "CMD"
            return None
        if self.state == "CMD":
            self.cmd = byte
            self.state = "LEN"
            return None
        if self.state == "LEN":
            self.plen = byte
            self.payload = bytearray()
            self.state = "PAYLOAD" if self.plen else "CRC"
            return None
        if self.state == "PAYLOAD":
            self.payload.append(byte)
            if len(self.payload) >= self.plen:
                self.state = "CRC"
            return None
        if self.state == "CRC":
            body = bytes([self.addr, self.cmd, self.plen]) + bytes(self.payload)
            ok = crc8(body) == byte
            frame = Frame(self.addr, self.cmd, bytes(self.payload)) if ok else None
            self._reset()
            return frame
        self._reset()  # unreachable, but never get stuck
        return None


class FrameReader:
    """Synchronous, deadline-bounded frame reads over an open serial port -
    the request/response pattern the CLI tool uses. `ser` just needs a
    pyserial-shaped read()/timeout."""

    def __init__(self, ser):
        self.ser = ser
        self.assembler = FrameAssembler()

    def read_one(self, deadline: float):
        """Read a single valid frame before `deadline` (time.monotonic()), or None."""
        while time.monotonic() < deadline:
            remaining = max(0.0, deadline - time.monotonic())
            self.ser.timeout = remaining
            b = self.ser.read(1)
            if not b:
                continue
            frame = self.assembler.feed(b[0])
            if frame is not None:
                return frame
        return None

    def read_all(self, window_s: float):
        """Collect every valid frame that arrives within window_s from now."""
        deadline = time.monotonic() + window_s
        frames = []
        while True:
            f = self.read_one(deadline)
            if f is None:
                break
            frames.append(f)
        return frames


def parse_int(s: str) -> int:
    return int(s, 0)  # accepts decimal or 0x.. hex


def parse_hex_bytes(tokens):
    out = bytearray()
    for t in tokens:
        out.append(int(t, 0) & 0xFF)
    return bytes(out)


# ---------------------------
# Firmware baud lookup
# ---------------------------
# The bus baud isn't a protocol constant - it's whatever RS485_BAUD the
# flashed firmware was built with. Read it straight out of each firmware
# folder's src/main.cpp (siblings of this tools/ folder) instead of
# hardcoding a copy here that can silently drift.
SW_ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
_RS485_BAUD_RE = re.compile(r"^\s*const\s+unsigned\s+long\s+RS485_BAUD\s*=\s*(\d+)", re.MULTILINE)


def _firmware_sort_key(name: str):
    n = name.lower()
    if re.match(r"v\d", n):
        rank = 3  # release line (v0.01a, ...)
    elif n.startswith("beta"):
        rank = 2
    elif n.startswith("alpha"):
        rank = 1
    else:
        rank = 0  # test benches etc.
    return (rank, n)


def firmware_bauds(sw_root: str = SW_ROOT):
    """[(folder name, RS485_BAUD)] for every firmware folder under sw_root
    whose src/main.cpp defines RS485_BAUD, newest release line first."""
    found = []
    try:
        entries = os.listdir(sw_root)
    except OSError:
        return found
    for name in entries:
        main_cpp = os.path.join(sw_root, name, "src", "main.cpp")
        if not os.path.isfile(main_cpp):
            continue
        try:
            with open(main_cpp, encoding="utf-8", errors="replace") as f:
                m = _RS485_BAUD_RE.search(f.read())
        except OSError:
            continue
        if m:
            found.append((name, int(m.group(1))))
    found.sort(key=lambda item: _firmware_sort_key(item[0]), reverse=True)
    return found


def latest_firmware_baud(sw_root: str = SW_ROOT):
    """(folder name, baud) of the newest firmware found, or (None, DEFAULT_BAUD)."""
    found = firmware_bauds(sw_root)
    return found[0] if found else (None, DEFAULT_BAUD)


# ---------------------------
# Per-command spec - drives the GUI's packet builder
# ---------------------------
class Field:
    """One payload field. `kind`:
      u8     - one byte, plain integer
      u16    - two bytes, big-endian
      ms10   - entered in ms, sent as one byte of ms/10 (CMD_PEEL)
      choice - one byte picked from `choices` [(label, value), ...]
      mm10   - entered in mm (decimal, signed), sent as signed 16-bit 0.1mm units (CMD_JOG)
      hex16  - 32 hex digits sent as 16 raw bytes (CMD_SET_SERIAL)
    """

    def __init__(self, name, kind, default, help="", choices=None, lo=0, hi=None, optional=False):
        self.optional = optional  # a blank value is left out of the payload (trailing fields only)
        self.name = name
        self.kind = kind
        self.default = default
        self.help = help
        self.choices = choices or []
        self.lo = lo
        self.hi = hi if hi is not None else {"u16": 0xFFFF, "ms10": 2550}.get(kind, 255)

    def encode(self, text: str) -> bytes:
        text = str(text).strip()
        if self.kind == "mm10":
            tenths = round(float(text) * 10)
            if tenths == 0 or not -self.hi <= tenths <= self.hi:
                raise ValueError(f"{self.name}: {text}mm out of range (0.1 to {self.hi / 10:g} mm, either sign)")
            return (tenths & 0xFFFF).to_bytes(2, "big")
        if self.kind == "hex16":
            digits = text.replace(" ", "").replace("-", "")
            if len(digits) != 32:
                raise ValueError(f"{self.name}: need 32 hex digits, got {len(digits)}")
            return bytes.fromhex(digits)
        if self.kind == "choice":
            for label, value in self.choices:
                if text == label:
                    return bytes([value])
        v = int(text, 0)  # a choice field also takes a raw number
        if not self.lo <= v <= self.hi:
            raise ValueError(f"{self.name}: {v} out of range {self.lo}-{self.hi}")
        if self.kind == "ms10":
            return bytes([max(1, min(255, round(v / 10)))])
        if self.kind == "u16":
            return bytes([(v >> 8) & 0xFF, v & 0xFF])
        return bytes([v & 0xFF])


class CmdSpec:
    def __init__(self, summary, reply, fields=(), notes="", broadcast=False):
        self.summary = summary
        self.reply = reply
        self.fields = list(fields)
        self.notes = notes
        self.broadcast = broadcast  # firmware only honors it when sent to addr 0x00

    def encode(self, values) -> bytes:
        parts = []
        for f, v in zip(self.fields, values):
            if f.optional and not str(v).strip():
                break
            parts.append(f.encode(v))
        return b"".join(parts)


_ON_OFF = [("off", 0), ("on", 1)]

COMMAND_SPECS = {
    0x01: CmdSpec("Is this address alive?", "CMD_PONG"),
    0x10: CmdSpec("Ask every UNASSIGNED feeder to announce itself. Each replies after a random 0-200ms delay.",
                  "CMD_DISCOVER_HERE from addr 0x00: nonce, componentId, tapeWidth",
                  broadcast=True,
                  notes="Feeders forget their address on every power-up, so Scan + Assign after each reboot."),
    0x11: CmdSpec("Give the feeder with this session nonce a bus address.",
                  "CMD_ACK [newAddr], sent from the new address",
                  [Field("nonce", "u16", "0x0000", "from the CMD_DISCOVER_HERE reply"),
                   Field("newAddr", "u8", "1", "1-247", lo=1, hi=247)],
                  broadcast=True),
    0x20: CmdSpec("Read component id + feed config.", "CMD_COMPONENT_INFO: componentId, tapeZeroRaw, feedHalfTeeth"),
    0x21: CmdSpec("Set the loaded component id.", "CMD_ACK (echo)",
                  [Field("componentId", "u16", "1", "0-65534", hi=65534)],
                  notes="A DIFFERENT id clears tape zero and pitch - set the pitch again afterwards."),
    0x22: CmdSpec("Set tape zero and pitch in one go.", "CMD_ACK (echo)",
                  [Field("tapeZeroRaw", "u16", "0", "AS5600 count 0-4095", hi=4095),
                   Field("feedHalfTeeth", "u8", "2", "pitch / 2mm (2 = 4mm)", lo=1)]),
    0x23: CmdSpec("Clear tape zero and pitch.", "CMD_ACK"),
    0x24: CmdSpec("Save the current wheel angle as tape zero.", "CMD_ACK"),
    0x25: CmdSpec("Set feed pitch. Rounded to the nearest 2mm.", "CMD_ACK (echo)",
                  [Field("mm", "u8", "4", "whole mm: 2, 4, 8, 12...", lo=1)],
                  notes="CMD_FEED_NEXT returns NACK ERR_NOT_READY until a pitch is set."),
    0x26: CmdSpec("Advance the sprocket by one pitch. Motor A only: the peel motor does NOT run.",
                  "CMD_ACK when done, or CMD_NACK [errCode]",
                  notes="Blocks up to 6s. The feeder can't hear the bus while moving, so wait for the reply."),
    0x27: CmdSpec("External LED on/off.", "CMD_ACK (echo)",
                  [Field("state", "choice", "on", choices=_ON_OFF)]),
    0x28: CmdSpec("Flip a motor's direction (RAM only, lost on reboot).", "CMD_ACK (echo)",
                  [Field("motor", "choice", "B (peel)", choices=[("A (feed)", 0), ("B (peel)", 1)]),
                   Field("state", "choice", "on", choices=_ON_OFF)],
                  notes="Flipping motor A mirrors the angle, so a saved tape zero no longer lines up."),
    0x29: CmdSpec("Read tape width.", "CMD_HW_INFO: tapeWidthMm"),
    0x2A: CmdSpec("Write tape width to the AT24CS02 (assembly time).", "CMD_ACK (echo), or CMD_NACK if invalid",
                  [Field("tapeWidthMm", "choice", "8",
                         choices=[(str(w), w) for w in (8, 12, 16, 24, 32, 44, 56)])]),
    0x30: CmdSpec("Live telemetry.", "CMD_STATUS_INFO: angle, magnet, fault, lastMoveErr, iMonRaw, relay"),
    0x31: CmdSpec("Brake both motors.", "CMD_ACK",
                  notes="Not heard during FEED_NEXT/PEEL: those block the feeder until finished."),
    0x32: CmdSpec("Blink the status LED white.", "CMD_ACK after blinking",
                  [Field("blinkCount", "u8", "3", "0 = default 3")]),
    0x33: CmdSpec("Read the AT24CS02 factory serial.", "CMD_SERIAL_INFO (16 bytes), or CMD_NACK"),
    0x34: CmdSpec("Run the peel motor alone for a set time.", "CMD_ACK (echo) after the run, or CMD_NACK",
                  [Field("dir", "choice", "fwd", choices=[("fwd", 0), ("rev", 1)]),
                   Field("duration", "ms10", "300", "ms, 10-2550 (sent as ms/10); BLANK = feeder's calibrated time (v0.02+)",
                         lo=10, optional=True)],
                  notes="Open loop, no encoder. Blocks the feeder for the whole run."),
    0x35: CmdSpec("Save this feeder's calibrated peel time (stored on the feeder, survives reboots and component changes).",
                  "CMD_ACK (echo), or CMD_NACK ERR_BAD_PARAM",
                  [Field("peelMs", "u16", "1570", "ms, 10-5000", lo=10, hi=5000)],
                  notes="v0.02+. Use it with CMD_PEEL by leaving the duration blank."),
    0x36: CmdSpec("Read this feeder's calibrated peel time.", "CMD_PEEL_TIME_INFO: peelMs (0xFFFF = uncalibrated)",
                  notes="v0.02+."),
    0x37: CmdSpec("Move the sprocket by a distance, relative to where it is (negative = backwards).",
                  "CMD_ACK [angleRawHi, angleRawLo] = new position, or CMD_NACK [errCode]",
                  [Field("mm", "mm10", "0.5", "mm, signed, 0.1 resolution, up to 160", hi=1600)],
                  notes="v0.02+. Use it to seat a sprocket hole, then CMD_ZERO_HERE. Blocks up to 6s."),
    0x38: CmdSpec("List the I2C addresses that answer (diagnoses the AT24 EEPROM).",
                  "CMD_I2C_SCAN_INFO: list of addresses", notes="v0.02+."),
    0x39: CmdSpec("Program a 16-byte serial into a plain AT24C02 (read back and verified).",
                  "CMD_ACK, or CMD_NACK ERR_LOCKED (factory serial exists) / ERR_I2C (EEPROM problem)",
                  [Field("serial", "hex16", "", "32 hex digits")],
                  notes="v0.02+. Not needed on an AT24CS02: its factory serial is read-only and always wins."),
}


def describe_frame(frame: bytes) -> str:
    """Annotated byte breakdown of a frame built by build_frame()."""
    if len(frame) < 5:
        return frame.hex(" ").upper()
    parts = [f"{frame[0]:02X} start", f"{frame[1]:02X} addr", f"{frame[2]:02X} cmd", f"{frame[3]:02X} len"]
    if len(frame) > 5:
        parts.append(f"{frame[4:-1].hex(' ').upper()} payload")
    parts.append(f"{frame[-1]:02X} crc")
    return " | ".join(parts)
