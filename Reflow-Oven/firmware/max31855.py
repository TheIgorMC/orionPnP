"""MAX31855K thermocouple SPI amplifier driver for MicroPython (RP2040 Pico).

Wiring:
    CS   → any GPIO, configured as output (passed in)
    SCK  → SPI SCK
    MISO → SPI MISO (labelled SO on the breakout)
    MOSI → not connected (MAX31855 is read-only; required by SPI constructor)
"""

import struct


class MAX31855:
    def __init__(self, spi, cs):
        self._spi = spi
        self._cs = cs
        self._cs.value(1)

    def read_raw(self) -> int:
        """Return the raw 32-bit register value from the MAX31855K."""
        buf = bytearray(4)
        self._cs.value(0)
        self._spi.readinto(buf)
        self._cs.value(1)
        return struct.unpack(">I", buf)[0]

    def parse(self, raw: int) -> dict:
        """Parse a raw 32-bit register value.

        Returns:
            tc           (float) — thermocouple temperature in °C (0.25 °C resolution)
            ref          (float) — cold-junction reference temperature in °C (0.0625 °C)
            fault        (int)   — 1 if any fault is active, else 0
            open_circuit (bool)  — thermocouple not connected
            short_gnd    (bool)  — thermocouple shorted to GND
            short_vcc    (bool)  — thermocouple shorted to VCC
        """
        fault = (raw >> 16) & 0x1

        # Thermocouple temp: bits 31..18 — 14-bit two's complement, 0.25 °C/LSB
        tc_raw = (raw >> 18) & 0x3FFF
        if tc_raw & 0x2000:  # sign bit set → negative
            tc_raw -= 0x4000
        tc_temp = tc_raw * 0.25

        # Cold-junction reference: bits 15..4 — 12-bit two's complement, 0.0625 °C/LSB
        ref_raw = (raw >> 4) & 0xFFF
        if ref_raw & 0x800:
            ref_raw -= 0x1000
        ref_temp = ref_raw * 0.0625

        return {
            "tc": tc_temp,
            "ref": ref_temp,
            "fault": fault,
            "open_circuit": bool(raw & 0x01),
            "short_gnd": bool(raw & 0x02),
            "short_vcc": bool(raw & 0x04),
        }

    def read(self) -> dict:
        """Read and parse the current temperature in one call."""
        return self.parse(self.read_raw())
