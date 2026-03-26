"""MCU simulator — emits synthetic temperature data as JSON lines.

Simulates the RP2040 Pico firmware (firmware/main.py) running the Sn42Bi58
low-temp reflow profile, so the PC app can be developed and tested without
physical hardware.

── Usage (from the app/ directory) ──────────────────────────────────────────

Write to a virtual COM port (Windows com0com / Linux socat):
    python tools/simulate_mcu.py --port COM4

Write to stdout (pipe / redirect):
    python tools/simulate_mcu.py

── Flags ─────────────────────────────────────────────────────────────────────
    --port  PORT     Serial port to write to (e.g. COM4, /dev/ttyS10).
                     Omit to write to stdout.
    --noise SIGMA    Gaussian noise sigma in °C (default: 0.5)
    --speed FACTOR   Time acceleration factor (default: 1.0, use >1 for fast tests)
    --fault          Inject an open-circuit fault after the Soak stage

── Windows virtual COM port setup (com0com) ──────────────────────────────────
    1. Install com0com: https://sourceforge.net/projects/com0com/
    2. In the com0com Setup utility, create a pair e.g. COM4 <-> COM5.
    3. Run:  python tools/simulate_mcu.py --port COM4
    4. In the Orion app, connect to COM5 (or use Auto-detect if Pico is absent).

── Linux / macOS (socat) ─────────────────────────────────────────────────────
    socat -d -d pty,raw,echo=0,link=/tmp/ttyV0 pty,raw,echo=0,link=/tmp/ttyV1
    python tools/simulate_mcu.py --port /tmp/ttyV0
    # connect app to /tmp/ttyV1
"""

from __future__ import annotations

import argparse
import json
import random
import sys
import time
from typing import Optional

try:
    import serial
    _SERIAL_AVAILABLE = True
except ImportError:
    _SERIAL_AVAILABLE = False


# ── Simulated oven thermal model ───────────────────────────────────────────

def oven_sim(stages: list[tuple], noise: float = 0.5, ambient: float = 25.0):
    """Generator yielding (uptime_ms, tc_temp, ref_temp, fault) tuples.

    stages: list of (target_temp, ramp_rate_c_per_s, dwell_s)
    """
    t_ms   = 0
    temp   = ambient
    ref    = ambient

    for i, (target, ramp_rate, dwell_s) in enumerate(stages):
        effective_rate = abs(ramp_rate) if ramp_rate != 0 else 1.0
        direction = 1 if target > temp else -1

        # Ramp phase — first-order approach to target with slight lag
        while True:
            gap = target - temp
            if abs(gap) < 0.3:
                temp = target
                break
            step = direction * effective_rate * 0.5  # 0.5 s sample interval
            # Add a slight nonlinearity: slows near target
            step *= max(0.1, min(1.0, abs(gap) / 20.0))
            temp += step + random.gauss(0, noise * 0.3)
            ref   = ambient + random.gauss(0, 0.1)
            t_ms += 500
            yield t_ms, round(temp, 2), round(ref, 4), 0

        # Dwell phase
        dwell_end_ms = t_ms + int(dwell_s * 1000)
        while t_ms < dwell_end_ms:
            temp = target + random.gauss(0, noise)
            ref  = ambient + random.gauss(0, 0.1)
            t_ms += 500
            yield t_ms, round(temp, 2), round(ref, 4), 0

    # Cool down — natural (fan off)
    while temp > ambient + 5:
        temp = max(ambient, temp - random.uniform(0.8, 1.4))
        ref  = ambient + random.gauss(0, 0.1)
        t_ms += 500
        yield t_ms, round(temp, 2), round(ref, 4), 0


# ── Sn42Bi58 profile stages ────────────────────────────────────────────────
SN42BI58_STAGES = [
    (90,  1.0, 60),   # Preheat
    (130, 0.5, 90),   # Soak
    (165, 1.5, 30),   # Reflow
]


# Module-level output sink — replaced with a serial.Serial in --port mode
_out = None


def main() -> None:
    global _out

    parser = argparse.ArgumentParser(description="Orion Reflow Oven MCU simulator")
    parser.add_argument("--port",   type=str,   default=None,
                        help="Serial port to write to (e.g. COM4, /dev/ttyS10). "
                             "Omit to write to stdout.")
    parser.add_argument("--noise",  type=float, default=0.5,
                        help="Gaussian noise sigma in °C (default: 0.5)")
    parser.add_argument("--speed",  type=float, default=1.0,
                        help="Time acceleration factor (default: 1.0)")
    parser.add_argument("--fault",  action="store_true",
                        help="Inject an open-circuit fault after Soak stage")
    args = parser.parse_args()

    if args.port:
        if not _SERIAL_AVAILABLE:
            print("ERROR: pyserial is not installed. Run: pip install pyserial", file=sys.stderr)
            sys.exit(1)
        import serial as _serial
        print(f"Opening {args.port} at 115200 baud…", file=sys.stderr)
        _out = _serial.Serial(args.port, baudrate=115200, timeout=1)
        print(f"Writing to {args.port}. Connect the app to the other end of the virtual pair.", file=sys.stderr)
    else:
        _out = None  # write to stdout

    interval = 0.5 / max(args.speed, 0.01)
    fault_injected = False

    # Emit a ready event first
    _emit({"event": "ready", "fw": "orion-reflow-phase1-sim"})

    soak_end_ms = int((60 + 90) * 1000)  # approx soak finish

    for t_ms, tc, ref, fault in oven_sim(SN42BI58_STAGES, noise=args.noise):
        # Fault injection: replace real data with open-circuit garbage after soak
        if args.fault and t_ms > soak_end_ms and not fault_injected:
            fault_injected = True
            _emit({"t": t_ms, "tc": 0.0, "ref": ref, "fault": 1})
            _emit({
                "t": t_ms, "event": "fault",
                "open_circuit": True, "short_gnd": False, "short_vcc": False,
            })
            time.sleep(interval)
            continue

        _emit({"t": t_ms, "tc": tc, "ref": ref, "fault": fault})
        time.sleep(interval)

    _emit({"event": "run_complete"})


def _emit(payload: dict) -> None:
    payload.setdefault("t", int(time.monotonic() * 1000))
    line = json.dumps(payload) + "\n"
    if _out is not None:
        _out.write(line.encode())
        _out.flush()
    else:
        sys.stdout.write(line)
        sys.stdout.flush()


if __name__ == "__main__":
    main()
