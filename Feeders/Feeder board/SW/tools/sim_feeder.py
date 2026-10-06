#!/usr/bin/env python3
"""
Simulated feeder for trying the RS485 tools without hardware (Linux/macOS
only - it uses a pseudo-terminal, no Windows equivalent without a virtual
COM pair such as com0com).

    python sim_feeder.py [--addr N]

Prints a device path such as /dev/pts/5. Connect rs485_gui.py or
rs485_sender.py to that "port". It behaves roughly like v0.02b firmware:
boots unassigned, answers discovery/assignment, keeps config in memory, and
is deaf while a "motor" runs (feed/jog/peel sleep for a realistic time).
Not a faithful model of the firmware - just enough to exercise the tools.
"""
import argparse
import os
import pty
import random
import select
import time
import tty

from rs485_protocol import FrameAssembler, build_frame

ERR_BAD_PARAM, ERR_NOT_READY, ERR_I2C = 0x05, 0x06, 0x07


class SimFeeder:
    def __init__(self, addr=0):
        self.addr = addr
        self.nonce = random.randrange(1, 0xFFFF)
        self.component = 0xFFFF
        self.zero = 0xFFFF
        self.half_teeth = 0xFF
        self.width = 0xFF
        self.peel_ms = 0xFFFF
        self.peel_rate = 0xFFFF  # 0.1 ms/mm
        self.led = 40
        self.angle = 1234
        self.magnet_ok = True
        self.fault = False

    def _angle_after(self, mm):
        self.angle = (self.angle + int(round(mm * 4096 / 160.0))) % 4096

    def peel_time_for(self, mm):
        return 0 if self.peel_rate == 0xFFFF else abs(mm) * self.peel_rate / 10.0 / 1000.0

    def handle(self, frame, send, out_delay):
        a, c, p = frame.addr, frame.cmd, bytes(frame.payload)
        if self.addr == 0 and a == 0:
            if c == 0x10:
                time.sleep(random.uniform(0, 0.2))
                send(0, 0x90, bytes([self.nonce >> 8, self.nonce & 255, self.component >> 8, self.component & 255, self.width]))
            elif c == 0x11 and len(p) >= 3 and ((p[0] << 8) | p[1]) == self.nonce:
                self.addr = p[2]
                send(self.addr, 0x82, p[2:3])
            return
        if self.addr == 0 or a not in (0, self.addr):
            return
        ack = lambda pl=b"": send(self.addr, 0x82, pl)
        nack = lambda e=0: send(self.addr, 0x83, bytes([e]))
        if c == 0x01:
            send(self.addr, 0x81, b"")
        elif c == 0x30:
            status = 0x20 if self.magnet_ok else 0x00
            imon = 150 + random.randrange(-3, 4)
            send(self.addr, 0xA2, bytes([self.angle >> 8, self.angle & 255, status, int(self.fault), 0,
                                         imon >> 8, imon & 255, 1]))
        elif c == 0x20:
            send(self.addr, 0xA0, bytes([self.component >> 8, self.component & 255, self.zero >> 8, self.zero & 255, self.half_teeth]))
        elif c == 0x21 and len(p) >= 2:
            new = (p[0] << 8) | p[1]
            if new != self.component:
                self.zero, self.half_teeth = 0xFFFF, 0xFF
            self.component = new
            ack()
        elif c == 0x24:
            self.zero = self.angle
            ack()
        elif c == 0x25 and p:
            self.half_teeth = max(1, round(p[0] / 2))
            ack(p)
        elif c == 0x26:
            if self.half_teeth == 0xFF:
                return nack(ERR_NOT_READY)
            mm = self.half_teeth * 2.0
            time.sleep(0.4)
            self._angle_after(mm)
            time.sleep(self.peel_time_for(mm))
            ack()
        elif c == 0x34:
            if len(p) == 1:
                if self.peel_ms == 0xFFFF:
                    return nack(ERR_NOT_READY)
                time.sleep(self.peel_ms / 1000.0)
            elif len(p) >= 2:
                time.sleep(p[1] * 0.01)
            ack(p)
        elif c == 0x35 and len(p) >= 2:
            ms = (p[0] << 8) | p[1]
            if not 10 <= ms <= 5000:
                return nack(ERR_BAD_PARAM)
            self.peel_ms = ms
            ack(p)
        elif c == 0x36:
            send(self.addr, 0xA4, bytes([self.peel_ms >> 8, self.peel_ms & 255]))
        elif c == 0x37 and len(p) >= 2:
            v = (p[0] << 8) | p[1]
            v = v - 65536 if v & 0x8000 else v
            time.sleep(0.2 + abs(v) / 100.0 * 0.05)
            self._angle_after(v / 10.0)
            ack(bytes([self.angle >> 8, self.angle & 255]))
        elif c == 0x29:
            send(self.addr, 0xA1, bytes([self.width]))
        elif c == 0x2A and p:
            self.width = p[0]
            ack(p)
        elif c == 0x31:
            ack()
        elif c == 0x32:
            time.sleep(0.5)
            ack()
        elif c == 0x33:
            nack(ERR_I2C)
        elif c == 0x38:
            send(self.addr, 0xA5, bytes([0x36, 0x50]))
        elif c == 0x3A and p and p[0]:
            self.led = p[0]
            ack(p)
        elif c == 0x3B and len(p) >= 2:
            t = (p[0] << 8) | p[1]
            if t != 0 and not 5 <= t <= 5000:
                return nack(ERR_BAD_PARAM)
            self.peel_rate = 0xFFFF if t == 0 else t
            ack(p)
        elif c == 0x3C:
            send(self.addr, 0xA6, bytes([self.peel_rate >> 8, self.peel_rate & 255]))
        else:
            nack(ERR_BAD_PARAM)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--addr", type=int, default=0, help="start already assigned to this address (default: unassigned)")
    ap.add_argument("--no-magnet", action="store_true", help="report no magnet")
    args = ap.parse_args()

    master, slave = pty.openpty()
    tty.setraw(master)
    tty.setraw(slave)
    feeder = SimFeeder(args.addr)
    feeder.magnet_ok = not args.no_magnet
    print(os.ttyname(slave), flush=True)
    print(f"sim feeder: addr={args.addr or 'unassigned'} nonce=0x{feeder.nonce:04X}  (Ctrl+C to stop)", flush=True)

    def send(addr, cmd, payload):
        os.write(master, build_frame(addr, cmd, payload))

    asm = FrameAssembler()
    try:
        while True:
            if not select.select([master], [], [], 0.2)[0]:
                continue
            for b in os.read(master, 256):
                frame = asm.feed(b)
                if frame is not None:
                    feeder.handle(frame, send, None)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
