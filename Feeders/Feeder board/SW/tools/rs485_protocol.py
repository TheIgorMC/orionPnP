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
import time

FRAME_START = 0xAA
DEFAULT_BAUD = 9600  # both feeder UARTs run off the 8MHz internal RC osc - see PROTOCOL.md
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
}

# Payload shape for each *request* command - for the GUI's hint label and
# anyone reading this as a quick reference. Reply payload shapes are
# decoded in decode_payload() below instead, since those matter more at
# runtime than as a typing hint.
PAYLOAD_HINTS = {
    0x01: "(no payload)",
    0x10: "(no payload, broadcast)",
    0x11: "nonceHi nonceLo newAddr  (broadcast)",
    0x20: "(no payload)",
    0x21: "idHi idLo",
    0x22: "zeroHi zeroLo feedHalfTeeth",
    0x23: "(no payload)",
    0x24: "(no payload)",
    0x25: "mm  (1 byte, whole mm)",
    0x26: "(no payload)",
    0x27: "state  (0=off, nonzero=on)",
    0x28: "motor(0=A,1=B) state(0/1)",
    0x29: "(no payload)",
    0x2A: "tapeWidthMm",
    0x30: "(no payload)",
    0x31: "(no payload)",
    0x32: "blinkCount  (0 = default 3)",
    0x33: "(no payload)",
    0x34: "dir(0=fwd,1=rev) duration_x10ms(1-255)",
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
            return f"componentId={comp_s} tapeZeroRaw={zero_s} feedHalfTeeth={p[4]}"
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
    except Exception as exc:  # malformed/short payload from a flaky link - don't crash the caller
        return f"(decode error: {exc})"
    return ""


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
