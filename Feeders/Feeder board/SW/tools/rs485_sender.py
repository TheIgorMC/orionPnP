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

from rs485_protocol import (
    latest_firmware_baud,
    cmd_name,
    build_frame,
    FrameReader,
    parse_int,
    parse_hex_bytes,
)


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
    fw_name, fw_baud = latest_firmware_baud()
    ap.add_argument("--baud", type=int, default=fw_baud,
                    help=f"default {fw_baud} (RS485_BAUD from {fw_name or 'built-in fallback'})")
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
