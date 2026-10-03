#!/usr/bin/env python3
"""
RS485 command sender for the OrionPnP feeder board protocol.

Talks to feeders over a USB-RS485 adapter on a COM/serial port, using the
frame format and command set documented in ../PROTOCOL.md:

    [0xAA][ADDR][CMD][LEN][PAYLOAD...][CRC8]

CRC8: poly 0x07, computed over ADDR..PAYLOAD (matches the firmware's
crc8() in src/main.cpp exactly - same bit-by-bit MSB-first algorithm, no
table, no init/final XOR).

Usage:
    python rs485_sender.py --list                  list available COM ports
    python rs485_sender.py --port COM5             open an interactive REPL
    python rs485_sender.py --port COM5 --rts-tx    toggle RTS around each
                                                    write, for USB-RS485
                                                    adapters that use RTS
                                                    as their DE/direction
                                                    control (most auto-
                                                    direction adapters
                                                    don't need this)

Inside the REPL, type `help` for the full command list. Named commands
(ping, scan, getstatus, setpitch, ...) wrap the protocol's commands with
friendly argument parsing; `raw ADDR CMD [hexbytes...]` sends anything
else you want to try, for protocol experiments the named commands don't
cover.
"""
import argparse
import shlex
import sys
import time

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    print("This tool needs pyserial: pip install pyserial", file=sys.stderr)
    sys.exit(1)

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
    except Exception as exc:  # malformed/short payload from a flaky link - don't crash the REPL
        return f"(decode error: {exc})"
    return ""


class FrameReader:
    """Byte-at-a-time frame scanner over an already-open pyserial port."""

    def __init__(self, ser):
        self.ser = ser

    def read_one(self, deadline: float):
        """Read a single valid frame before `deadline` (time.monotonic()), or None."""
        state = "START"
        addr = cmd = plen = 0
        payload = bytearray()
        while time.monotonic() < deadline:
            remaining = max(0.0, deadline - time.monotonic())
            self.ser.timeout = remaining
            b = self.ser.read(1)
            if not b:
                continue
            byte = b[0]
            if state == "START":
                if byte == FRAME_START:
                    state = "ADDR"
            elif state == "ADDR":
                addr = byte
                state = "CMD"
            elif state == "CMD":
                cmd = byte
                state = "LEN"
            elif state == "LEN":
                plen = byte
                payload = bytearray()
                state = "PAYLOAD" if plen else "CRC"
            elif state == "PAYLOAD":
                payload.append(byte)
                if len(payload) >= plen:
                    state = "CRC"
            elif state == "CRC":
                body = bytes([addr, cmd, plen]) + bytes(payload)
                if crc8(body) == byte:
                    return Frame(addr, cmd, bytes(payload))
                # CRC mismatch: drop and resync, matching the firmware's own behaviour
                state = "START"
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


class Bus:
    def __init__(self, ser, rts_tx: bool, verbose: bool):
        self.ser = ser
        self.rts_tx = rts_tx
        self.verbose = verbose
        self.reader = FrameReader(ser)

    def send(self, addr: int, cmd: int, payload: bytes = b""):
        frame = build_frame(addr, cmd, payload)
        if self.verbose:
            print(f"  TX: {frame.hex(' ')}  (addr=0x{addr:02X} cmd={cmd_name(cmd)})")
        if self.rts_tx:
            self.ser.setRTS(True)
        self.ser.write(frame)
        self.ser.flush()
        if self.rts_tx:
            time.sleep(0.002)  # let the last bit clear the wire before releasing DE
            self.ser.setRTS(False)

    def send_and_collect(self, addr: int, cmd: int, payload: bytes = b"", window_s: float = 0.5):
        self.send(addr, cmd, payload)
        frames = self.reader.read_all(window_s)
        if not frames:
            print("  (no response)")
        for f in frames:
            print(f"  RX: {f}")
        return frames


def parse_int(s: str) -> int:
    return int(s, 0)  # accepts decimal or 0x.. hex


def parse_hex_bytes(tokens):
    out = bytearray()
    for t in tokens:
        out.append(int(t, 0) & 0xFF)
    return bytes(out)


# ---------------------------
# Named command handlers - each takes (bus, args: list[str]) and does the
# send/print itself, so they can have whatever argument shape makes sense.
# ---------------------------

def cmd_ping(bus, args):
    addr = parse_int(args[0])
    bus.send_and_collect(addr, 0x01)


def cmd_scan(bus, args):
    window = float(args[0]) if args else 0.5
    print(f"  broadcasting CMD_DISCOVER, listening for {window:.2f}s...")
    bus.send(0x00, 0x10)
    frames = bus.reader.read_all(window)
    if not frames:
        print("  (no feeders responded)")
    for f in frames:
        print(f"  RX: {f}")
    return frames


def cmd_assign(bus, args):
    nonce = parse_int(args[0])
    new_addr = parse_int(args[1])
    payload = bytes([(nonce >> 8) & 0xFF, nonce & 0xFF, new_addr & 0xFF])
    bus.send_and_collect(0x00, 0x11, payload)


def cmd_getcomponent(bus, args):
    bus.send_and_collect(parse_int(args[0]), 0x20)


def cmd_setcomponent(bus, args):
    addr = parse_int(args[0])
    comp_id = parse_int(args[1])
    bus.send_and_collect(addr, 0x21, bytes([(comp_id >> 8) & 0xFF, comp_id & 0xFF]))


def cmd_setfeedconfig(bus, args):
    addr = parse_int(args[0])
    zero = parse_int(args[1])
    half_teeth = parse_int(args[2])
    bus.send_and_collect(addr, 0x22, bytes([(zero >> 8) & 0xFF, zero & 0xFF, half_teeth & 0xFF]))


def cmd_resetconfig(bus, args):
    bus.send_and_collect(parse_int(args[0]), 0x23)


def cmd_zerohere(bus, args):
    bus.send_and_collect(parse_int(args[0]), 0x24)


def cmd_setpitch(bus, args):
    addr = parse_int(args[0])
    mm = parse_int(args[1])  # whole mm only - CMD_SET_PITCH_MM payload is 1 byte
    bus.send_and_collect(addr, 0x25, bytes([mm & 0xFF]))


def cmd_feednext(bus, args):
    bus.send_and_collect(parse_int(args[0]), 0x26)


def cmd_setled(bus, args):
    addr = parse_int(args[0])
    state = parse_int(args[1])
    bus.send_and_collect(addr, 0x27, bytes([1 if state else 0]))


def cmd_setinvert(bus, args):
    addr = parse_int(args[0])
    motor = parse_int(args[1])  # 0=A, 1=B
    state = parse_int(args[2])
    bus.send_and_collect(addr, 0x28, bytes([motor & 0xFF, 1 if state else 0]))


def cmd_gethwinfo(bus, args):
    bus.send_and_collect(parse_int(args[0]), 0x29)


def cmd_sethwinfo(bus, args):
    addr = parse_int(args[0])
    width_mm = parse_int(args[1])
    bus.send_and_collect(addr, 0x2A, bytes([width_mm & 0xFF]))


def cmd_getstatus(bus, args):
    bus.send_and_collect(parse_int(args[0]), 0x30)


def cmd_stop(bus, args):
    bus.send_and_collect(parse_int(args[0]), 0x31)


def cmd_identify(bus, args):
    addr = parse_int(args[0])
    count = parse_int(args[1]) if len(args) > 1 else 0
    bus.send_and_collect(addr, 0x32, bytes([count & 0xFF]), window_s=1.5)  # blinks before replying


def cmd_getserial(bus, args):
    bus.send_and_collect(parse_int(args[0]), 0x33)


def cmd_peel(bus, args):
    addr = parse_int(args[0])
    direction = parse_int(args[1])  # 0=fwd, 1=rev
    duration10ms = parse_int(args[2])  # 1-255, x10ms
    bus.send_and_collect(addr, 0x34, bytes([direction & 0xFF, duration10ms & 0xFF]),
                          window_s=0.3 + duration10ms * 0.01)


def cmd_raw(bus, args):
    addr = parse_int(args[0])
    cmd = parse_int(args[1])
    payload = parse_hex_bytes(args[2:])
    bus.send_and_collect(addr, cmd, payload)


COMMANDS = {
    "ping": (cmd_ping, "ping <addr>", "liveness check -> CMD_PONG"),
    "scan": (cmd_scan, "scan [windowSeconds]", "broadcast CMD_DISCOVER, list every feeder that replies"),
    "assign": (cmd_assign, "assign <nonce> <newAddr>", "CMD_ASSIGN_ADDR broadcast - nonce from scan's output"),
    "getcomponent": (cmd_getcomponent, "getcomponent <addr>", "CMD_GET_COMPONENT"),
    "setcomponent": (cmd_setcomponent, "setcomponent <addr> <id>", "CMD_SET_COMPONENT - resets tape zero+pitch if id changes"),
    "setfeedconfig": (cmd_setfeedconfig, "setfeedconfig <addr> <zeroRaw> <halfTeeth>", "CMD_SET_FEED_CONFIG, low-level"),
    "resetconfig": (cmd_resetconfig, "resetconfig <addr>", "CMD_RESET_CONFIG - clears tape zero+pitch, keeps component id"),
    "zerohere": (cmd_zerohere, "zerohere <addr>", "CMD_ZERO_HERE - capture current position as tape zero"),
    "setpitch": (cmd_setpitch, "setpitch <addr> <mm>", "CMD_SET_PITCH_MM - whole mm only"),
    "feednext": (cmd_feednext, "feednext <addr>", "CMD_FEED_NEXT - advance by configured pitch"),
    "setled": (cmd_setled, "setled <addr> <0|1>", "CMD_SET_EXT_LED"),
    "setinvert": (cmd_setinvert, "setinvert <addr> <motor 0=A|1=B> <0|1>", "CMD_SET_INVERT_DIR - RAM-only"),
    "gethwinfo": (cmd_gethwinfo, "gethwinfo <addr>", "CMD_GET_HW_INFO - tape width"),
    "sethwinfo": (cmd_sethwinfo, "sethwinfo <addr> <widthMm>", "CMD_SET_HW_INFO - assembly/bench-time only"),
    "getstatus": (cmd_getstatus, "getstatus <addr>", "CMD_GET_STATUS - live telemetry"),
    "stop": (cmd_stop, "stop <addr>", "CMD_STOP - immediate brake, both motors"),
    "identify": (cmd_identify, "identify <addr> [blinkCount]", "CMD_IDENTIFY - white LED flashes"),
    "getserial": (cmd_getserial, "getserial <addr>", "CMD_GET_SERIAL - AT24CS02 factory serial"),
    "peel": (cmd_peel, "peel <addr> <dir 0=fwd|1=rev> <duration x10ms, 1-255>", "CMD_PEEL - run peel motor alone"),
    "raw": (cmd_raw, "raw <addr> <cmd> [hexByte ...]", "send any addr/cmd/payload - for anything not wrapped above"),
}


def print_help():
    print("Commands (addr/cmd/bytes accept decimal or 0x hex):")
    width = max(len(syntax) for _, syntax, _ in COMMANDS.values())
    for _, syntax, desc in COMMANDS.values():
        print(f"  {syntax.ljust(width)}  {desc}")
    print(f"  {'verbose'.ljust(width)}  toggle printing raw TX frame bytes")
    print(f"  {'help'.ljust(width)}  this list")
    print(f"  {'quit / exit'.ljust(width)}  close the port and exit")
    print()
    print("addr 0x00 is broadcast. scan/assign always use it regardless of")
    print("what you type, since that's how CMD_DISCOVER/CMD_ASSIGN_ADDR work.")


def list_ports():
    ports = list(serial.tools.list_ports.comports())
    if not ports:
        print("No serial ports found.")
        return
    for p in ports:
        print(f"  {p.device:10s} {p.description}")


def repl(bus):
    print("RS485 command sender ready. Type 'help' for commands, 'quit' to exit.")
    while True:
        try:
            line = input("RS485> ").strip()
        except (EOFError, KeyboardInterrupt):
            print()
            break
        if not line:
            continue
        try:
            tokens = shlex.split(line)
        except ValueError as exc:
            print(f"  (parse error: {exc})")
            continue
        name, args = tokens[0].lower(), tokens[1:]
        if name in ("quit", "exit"):
            break
        if name == "help":
            print_help()
            continue
        if name == "verbose":
            bus.verbose = not bus.verbose
            print(f"  verbose = {bus.verbose}")
            continue
        handler = COMMANDS.get(name)
        if handler is None:
            print(f"  unknown command '{name}' - try 'help'")
            continue
        func, syntax, _ = handler
        try:
            func(bus, args)
        except IndexError:
            print(f"  usage: {syntax}")
        except ValueError as exc:
            print(f"  bad argument: {exc}")
        except serial.SerialException as exc:
            print(f"  serial error: {exc}")
            break


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="serial port, e.g. COM5")
    ap.add_argument("--baud", type=int, default=DEFAULT_BAUD, help=f"default {DEFAULT_BAUD} (matches the firmware)")
    ap.add_argument("--list", action="store_true", help="list available serial ports and exit")
    ap.add_argument("--rts-tx", action="store_true", dest="rts_tx",
                     help="drive RTS high while transmitting, for adapters that use it as DE control")
    ap.add_argument("--verbose", action="store_true", help="print raw TX frame bytes from the start")
    args = ap.parse_args()

    if args.list:
        list_ports()
        return

    if not args.port:
        ap.error("--port is required (use --list to see available ports)")

    try:
        ser = serial.Serial(args.port, args.baud, timeout=0.1)
    except serial.SerialException as exc:
        print(f"Couldn't open {args.port}: {exc}", file=sys.stderr)
        sys.exit(1)

    print(f"Opened {args.port} @ {args.baud} baud" + (" (RTS-controlled TX)" if args.rts_tx else ""))
    bus = Bus(ser, rts_tx=args.rts_tx, verbose=args.verbose)
    try:
        repl(bus)
    finally:
        ser.close()


if __name__ == "__main__":
    main()
