"""Reflow Oven — Phase 1 firmware for RP2040 Pico / MicroPython.

Streams thermocouple readings as JSON lines over USB CDC serial at 500 ms intervals.
Accepts JSON command lines for future Phase 2 SSR control (stubbed here).

── Serial output ─────────────────────────────────────────────────────────────
One JSON line per sample:
  {"t":<uptime_ms>,"tc":<float °C>,"ref":<float °C>,"fault":<0|1>}

Fault detail line (emitted alongside the sample when fault=1):
  {"t":<ms>,"event":"fault","open_circuit":<bool>,"short_gnd":<bool>,"short_vcc":<bool>}

── Accepted input commands (one JSON line) ───────────────────────────────────
  {"cmd":"ping"}
      → {"t":...,"event":"pong"}

  {"cmd":"set_ssr","ssr1":<0|1>,"ssr2":<0|1>}   [Phase 2 stub]
      → {"t":...,"event":"ack","cmd":"set_ssr"}
"""

import json
import sys
import time
from machine import Pin, SPI

from max31855 import MAX31855

# ── Pin assignments ────────────────────────────────────────────────────────
_SCK  = 6
_MOSI = 7   # required by SPI() but not connected to MAX31855 (read-only device)
_MISO = 4
_CS   = 5
_LED  = 25  # onboard LED (GPIO 25 on standard Pico)

# ── Timing ─────────────────────────────────────────────────────────────────
_SAMPLE_MS  = 500
_OK_BLINK   = 40   # heartbeat on-time (ms)
_ERR_BLINK  = 100  # error blink on-time (ms)


def _emit(payload: dict) -> None:
    payload.setdefault("t", time.ticks_ms())
    sys.stdout.write(json.dumps(payload) + "\n")


def _blink(led, on_ms: int, count: int = 1) -> None:
    for i in range(count):
        led.on()
        time.sleep_ms(on_ms)
        led.off()
        if count > 1 and i < count - 1:
            time.sleep_ms(on_ms)


def _read_command() -> "dict | None":
    """Non-blocking stdin read — returns parsed dict or None."""
    try:
        raw = sys.stdin.readline()
        if raw:
            return json.loads(raw.strip())
    except Exception:
        pass
    return None


def _handle_command(cmd: dict) -> None:
    name = cmd.get("cmd")
    if name == "ping":
        _emit({"event": "pong"})
    elif name == "set_ssr":
        # Phase 2 stub — SSR GPIO lines not wired in Phase 1
        _emit({"event": "ack", "cmd": "set_ssr"})
    else:
        _emit({"event": "nack", "reason": "unknown_command"})


def main() -> None:
    led = Pin(_LED, Pin.OUT)
    spi = SPI(0, baudrate=5_000_000, sck=Pin(_SCK), mosi=Pin(_MOSI), miso=Pin(_MISO))
    cs  = Pin(_CS, Pin.OUT, value=1)
    tc  = MAX31855(spi, cs)

    next_sample = time.ticks_ms()
    _emit({"event": "ready", "fw": "orion-reflow-phase1"})

    while True:
        now = time.ticks_ms()

        # ── Non-blocking command input ───────────────────────────────────
        cmd = _read_command()
        if cmd:
            _handle_command(cmd)

        # ── Periodic temperature sample ──────────────────────────────────
        if time.ticks_diff(now, next_sample) >= 0:
            next_sample = time.ticks_add(now, _SAMPLE_MS)
            try:
                data = tc.read()
                _emit({
                    "tc":    round(data["tc"],  2),
                    "ref":   round(data["ref"], 4),
                    "fault": data["fault"],
                })
                if data["fault"]:
                    _emit({
                        "event":        "fault",
                        "open_circuit": data["open_circuit"],
                        "short_gnd":    data["short_gnd"],
                        "short_vcc":    data["short_vcc"],
                    })
                    _blink(led, _ERR_BLINK, 3)
                else:
                    _blink(led, _OK_BLINK)
            except Exception as exc:
                _emit({"event": "error", "msg": str(exc)})
                _blink(led, _ERR_BLINK, 5)

        time.sleep_ms(10)


main()
