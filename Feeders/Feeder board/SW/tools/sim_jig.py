#!/usr/bin/env python3
"""
Simulated TAP-Jig (the ATmega32u4 side) with a simulated feeder behind its
RS-485 passthrough, for developing and trying the Production tab and
tapjig_run.py without hardware. Linux/macOS only (pseudo-terminal).

    python sim_jig.py [--fail NAME ...]

Prints a device path (e.g. /dev/pts/5): use it as the jig port. Pair it with
`--dry-isp` in the GUI / tapjig_run.py, since there is no real programmer.
It speaks the line protocol in TAPJIG.md and models just enough: power-up
and boot delay, rails, LDR levels, buttons, reset, servo/AS5600, motors and
encoders, firmware personality (test vs production) switching on ISPDONE.

--fail NAME (repeatable) makes one thing wrong, to exercise failure paths:
  cold_short   VIN looks like a short at the pre-power check
  fb_low       buck feedback voltage too low
  no_as5600    AS5600 doesn't answer on I2C
  no_serial    plain AT24C02 (no factory serial)
  no_fiber     fiber LED doesn't light
  stuck_sw1    SW1 reads pressed all the time
"""
import argparse
import os
import pty
import random
import select
import threading
import time
import tty

from rs485_protocol import FrameAssembler, build_frame
from sim_feeder import SimFeeder

BOOT_S = 0.8


class SimJig:
    def __init__(self, feeder: SimFeeder, faults):
        self.f = feeder
        self.faults = set(faults)
        self.powered = False
        self.limit = 300
        self.latched = False
        self.ready_at = 0.0
        self.servo = 0.0
        self.stall = {1: False, 2: False}
        self.enc_armed = {}
        self.enc_result = {}
        self.f.on_motor = self._motor_spun
        self.f.factory_serial = None if "no_serial" in self.faults else bytes.fromhex("A1B2C3D4E5F60718293A4B5C6D7E8F90")
        self.f.has_as5600 = "no_as5600" not in self.faults
        self.f.magnet_ok = False
        self._apply_stuck()

    def _apply_stuck(self):
        if "stuck_sw1" in self.faults:
            self.f.sw[0] = 1

    # --- world ---
    def booted(self):
        return self.powered and time.monotonic() >= self.ready_at

    def power(self, on):
        if on and not self.powered:
            self.powered = True
            self.f.reboot(0x01)
            self.ready_at = time.monotonic() + BOOT_S
        elif not on:
            self.powered = False
            self.f.addr = 0

    def _motor_spun(self, motor, ms):
        ch = motor + 1
        if ch in self.enc_armed:
            self.enc_result[ch] = dict(pulses=40, rev_ms=520)

    def ldr(self, which):
        if which == "FIBER":
            return 640 if (self.f.ext_led and "no_fiber" not in self.faults and self.booted()) else 105
        if not self.booted():
            return 100
        if self.f.rgb_hold:
            r, g, b = self.f.rgb_hold
            return int(100 + 0.8 * r + 1.0 * g + 0.5 * b)
        if not self.f.magnet_ok or self.f.fault:
            return int(100 + 0.8 * 255 * 0.16)  # normal status LED: red when no magnet / fault
        return int(100 + 0.5 * 255 * 0.16)

    # --- command handling ---
    def handle(self, line: str) -> str:
        parts = line.split()
        if not parts:
            return "ERR empty"
        cmd, args = parts[0].upper(), parts[1:]
        try:
            return getattr(self, "c_" + cmd.lower(), self._unknown)(*args)
        except TypeError:
            return "ERR bad arguments"

    def _unknown(self, *a):
        return "ERR unknown command"

    def c_hello(self):
        return "OK fw=sim rev=3"

    def c_psu(self, state, limit="300"):
        if state.upper() == "OFF":
            self.power(False)
            return "OK"
        if "cold_short" in self.faults:
            self.latched = True
            return "ERR TRIP"
        self.limit = int(limit)
        self.power(True)
        return "OK"

    def c_state(self):
        return f"OK psu={int(self.powered)} latched={int(self.latched)} ma={25 if self.booted() else 6}"

    def c_rearm(self):
        self.latched = False
        return "OK"

    def c_safe(self):
        self.power(False)
        self.f.sw = [0, 0]
        self._apply_stuck()
        self.servo = 0.0
        self.f.magnet_ok = False
        self.stall = {1: False, 2: False}
        self.f.stalled = False
        self.f.rgb_hold = None
        return "OK"

    def c_cold(self, rail):
        ohms = {"VIN": 20000, "V5": 6500, "VMOT": 6500}.get(rail.upper())
        if ohms is None:
            return "ERR unknown rail"
        if "cold_short" in self.faults and rail.upper() == "VIN":
            ohms = 0.3
        return f"OK ohms={ohms}"

    def c_cont(self, name):
        table = {"VIN_PRE_POST": 0.2, "GNDP": 0.3, "MODE_RTN": 1000000,
                 "R485_POST": 0.5 if (self.booted() and self.f.relay) else 1000000}
        if name.upper() not in table:
            return "ERR unknown net"
        return f"OK ohms={table[name.upper()]}"

    def c_adc(self, node):
        up = node.upper()
        on = self.booted()
        vals = {
            "FB": (1224 if "fb_low" not in self.faults else 1010) if self.powered else 0,
            "V5": 5000 if self.powered else 0,
            "VIN": 12000 if self.powered else 0,
            "VINPROT": 11950 if self.powered else 0,
            "IMON": int(25 * 4070 / 200) if on else 0,
        }
        if up == "IIN":
            return f"OK ma={25 if on else 6}"
        if up not in vals:
            return "ERR unknown node"
        return f"OK mv={vals[up] + random.randint(-3, 3) if vals[up] else 0}"

    def c_dig(self, name):
        up = name.upper()
        table = {"FAULT": 1, "V5RDY": int(self.booted()), "FLT": 1, "EN485": int(self.booted() and self.f.relay)}
        if up not in table:
            return "ERR unknown signal"
        if up == "FAULT" and self.f.stalled and self.f.fault:
            return "OK v=0"
        return f"OK v={table[up]}"

    def c_sw(self, name, pressed):
        idx = {"SW1": 0, "SW2": 1}.get(name.upper())
        if idx is None:
            return "ERR unknown switch"
        self.f.sw[idx] = int(pressed)
        self._apply_stuck()
        return "OK"

    def c_reset(self, ms="50"):
        if self.powered:
            self.f.reboot(0x02)
            self.ready_at = time.monotonic() + BOOT_S
        return "OK"

    def c_ldr(self, which, samples="8"):
        if which.upper() not in ("FIBER", "RGB"):
            return "ERR unknown ldr"
        return f"OK raw={self.ldr(which.upper()) + random.randint(-2, 2)}"

    def c_servo(self, deg):
        self.servo = float(deg)
        self.f.magnet_ok = self.servo != 0.0
        self.f.angle = int((self.servo % 360) * 4096 / 360) % 4096
        return "OK"

    def c_stall(self, ch, on):
        self.stall[int(ch)] = bool(int(on))
        if int(ch) == 1:
            self.f.stalled = bool(int(on))
            if not int(on):
                self.f.fault = False
        return "OK"

    def c_encarm(self, ch, window_ms):
        self.enc_armed[int(ch)] = True
        self.enc_result.pop(int(ch), None)
        return "OK"

    def c_encread(self, ch):
        r = self.enc_result.get(int(ch), dict(pulses=0, rev_ms=0))
        return f"OK pulses={r['pulses']} rev_ms={r['rev_ms']}"

    def c_ispdone(self, which):
        self.f.test_fw = which.lower() == "test"
        if self.powered:
            self.f.reboot(0x01)
            self.ready_at = time.monotonic() + BOOT_S
        return "OK"

    def c_rs485(self, hexframe, timeout_ms="300"):
        if not self.booted():
            return "OK frames= de=0 n=0"
        out = bytearray()

        def send(addr, cmd, payload):
            out.extend(build_frame(addr, cmd, payload))

        asm = FrameAssembler()
        for b in bytes.fromhex(hexframe):
            frame = asm.feed(b)
            if frame is not None:
                self.f.handle(frame, send, None)
        de = 1 if out and "no_de" not in self.faults else 0
        return f"OK frames={bytes(out).hex()} de={de} n={len(out)}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fail", action="append", default=[], help="inject a fault (see the module docstring)")
    args = ap.parse_args()

    master, slave = pty.openpty()
    tty.setraw(master)
    tty.setraw(slave)
    sim = SimJig(SimFeeder(0), args.fail)
    print(os.ttyname(slave), flush=True)
    print(f"sim jig ready (faults: {args.fail or 'none'})  Ctrl+C to stop", flush=True)

    buf = bytearray()
    try:
        while True:
            if not select.select([master], [], [], 0.2)[0]:
                continue
            buf += os.read(master, 1024)
            while b"\n" in buf:
                line, _, rest = bytes(buf).partition(b"\n")
                buf = bytearray(rest)
                reply = sim.handle(line.decode("ascii", "replace").strip())
                os.write(master, (reply + "\n").encode("ascii"))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
