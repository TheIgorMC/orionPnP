# Reflow Oven — v01 Test Specification

## Overview

The OrionPnP handles component placement, but a reflow oven is required to complete the board assembly workflow. Commercial solutions are expensive and importing purpose-built controllers (e.g. Controleo3) adds further cost and complexity. The chosen approach is a fully DIY system built around a consumer oven, implemented in two progressive phases.

---

## Hardware Base

| Parameter | Value |
|---|---|
| Oven model | De Longhi 3835A |
| Internal volume | 21 L |
| Rated power | 1800 W |
| Future insulation | Reflective shielding (TBD, Phase 2) |

The 1800 W power rating is acceptable for low-temp pastes. For lead-free profiles with faster ramp rates, additional insulation may be required.

---

## Build Phases

### Phase 1 — Non-invasive monitoring (current focus)

**Goal:** Characterise the oven's thermal behaviour and enable guided manual reflow without any permanent hardware modification.

**Hardware:**
- K-type thermocouple (routed through the oven door seal or a small grommet)
- MAX31855K SPI thermocouple amplifier breakout
- **RP2040 Pico** connected to MAX31855K via SPI

**Firmware:**
- **MicroPython** on the RP2040 Pico
- Samples MAX31855K every 500 ms, streams JSON lines over USB CDC serial
- Output format: `{"t":<uptime_ms>,"tc":<float>,"ref":<float>,"fault":<0|1>}`
- Accepts incoming JSON commands (stubbed for Phase 2 SSR control)

**Software:**
- **Dear PyGui** native desktop application (GPU-rendered, Windows/macOS/Linux)
- Reads temperature over USB serial from the Pico (auto-detects by USB VID/PID 0x2E8A)
- Logs time/temperature data to timestamped CSV
- Implements calibration procedure (see below)
- Implements "Follow Me" guided reflow mode (see below)

**Target paste types:** Low-temperature solder pastes (e.g. Sn42Bi58, ~138 °C peak)

---

### Phase 2 — Full hardware mod (future)

**Goal:** Fully automated reflow with closed-loop control, suitable for standard and lead-free pastes.

Phase 2 supports two independent sub-modes that share the same hardware additions. The UI introduced in Phase 1 remains unchanged; a new **SSR Control** panel is added to the PC app for PC-bound mode.

---

#### Phase 2a — PC-bound mode

The host PC runs the PID control loop. The Pico firmware gains GPIO outputs for SSR1/SSR2 and accepts `{"cmd":"set_ssr","ssr1":0,"ssr2":1}` commands already stubbed in Phase 1 firmware.

- PID executes on the PC; USB round-trip latency (~10–50 ms) is negligible for thermal systems with time constants of tens of seconds.
- The Dear PyGui app gains a **SSR Control** panel: target profile selector, Start/Stop, live PID tuning (Kp/Ki/Kd).
- Logging and calibration workflows from Phase 1 are reused unchanged.

#### Phase 2b — Standalone mode

The PID loop and profile runner move onto the Pico. A display module (hardware TBD) replaces the PC for in-field use.

- Pico firmware extended: profile storage (LittleFS or SD), PID controller, display driver.
- The PC app remains available for monitoring, log download, and profile upload over USB.
- Profile and calibration file formats are identical to Phase 1 (JSON) for full compatibility.

---

**Shared hardware additions (both sub-modes):**
- 2× SSR (Solid State Relay): one for the oven heating elements, one for the convection fan
- At least one additional thermocouple (consider two: air + board surface)
- Replacement thermal fuse rated for cycling use
- Phase 2b only: embedded display module (hardware TBD during Phase 2 design)

**Target paste types:** Lead-free (Sn63Pb37 and SAC305, up to ~250 °C peak)

---

## Phase 1 — Software Specification

### Calibration Procedure

The purpose of calibration is to characterise the oven's thermal response curve — specifically how temperature evolves over time when running at **maximum power**, as a function of the temperature setpoint dialled in.

**Approach:**
- Run the oven at **max power only** (no partial-power sweeps).
- Set several discrete temperature targets across the useful range and record how the measured temperature varies over time for each.
- The script logs the full time/temperature curve for each setpoint, capturing ramp rate, overshoot, and settling behaviour.

**Spatial variation (optional):**
- The procedure can be repeated **2–3 times** with the thermocouple placed in different positions inside the oven cavity (e.g. centre, front corner, near elements) to map thermal uniformity.
- This is particularly relevant for board placement decisions in Follow Me mode.

**Empirical nature:**
- The actual curve shape is unknown until tested on the real oven. The calibration run will drive the design of the Follow Me prediction model — no assumptions are made upfront about linearity or lag characteristics.

**Procedure steps:**
1. Start with oven cold (ambient temperature).
2. Set oven dial to max power.
3. Dial in the first temperature target; script begins logging temperature vs. time.
4. Record until the oven reaches steady state or overshoots and stabilises.
5. Let the oven cool down, then repeat from step 2 for the next temperature target.
6. Optionally repeat the full sequence with the thermocouple repositioned.
7. Script post-processes the logged curves to extract ramp rate (°C/s) and any overshoot per target.

**Output:** A calibration file (JSON or CSV) per thermocouple position, containing the time/temperature curves and derived ramp characteristics, consumed by the Follow Me mode.

---

### Reflow Profiles

A profile defines the target temperature curve for a reflow cycle. Each profile is a sequence of named stages.

**Profile format (example — Sn42Bi58 low-temp paste):**

| Stage | Target temp (°C) | Ramp rate (°C/s) | Dwell time (s) |
|---|---|---|---|
| Preheat | 90 | ~1.0 | 60 |
| Soak | 130 | ~0.5 | 90 |
| Reflow | 165 | ~1.5 | 30 |
| Cooling | ambient | natural | — |

Profiles are stored as simple JSON files so they can be created and edited without modifying the script.

**Fields per stage:**
```json
{
  "name": "Soak",
  "target_temp": 130,
  "max_ramp_rate": 0.5,
  "dwell_seconds": 90
}
```

---

### Follow Me Mode

The script guides the user through a reflow profile by issuing real-time instructions based on live temperature readings and the calibration delta table.

**Behaviour:**
- Displays a live temperature plot (time on X axis, °C on Y axis) with the target profile overlaid.
- Calculates how far ahead of the current temperature the user needs to act (based on the calibration delta) to hit the next stage on time.
- Issues a prompt when the user must change the oven dial setting, including:
  - Which direction to turn (up/down)
  - How many notches/steps (if the dial positions are enumerated during calibration)
- Alerts if the actual temperature curve deviates outside a configurable tolerance band around the target profile.
- Logs the full run (timestamp, setpoint stage, measured temp, any user actions) to a CSV file for post-run review.

**UI — Dear PyGui desktop application:**
- Dark-themed native window (Catppuccin Mocha palette), 1280 × 800 minimum
- Sidebar navigation: Dashboard | Calibration | Follow Me | Profiles | Logs
- **Dashboard:** live temperature readout, connection panel, 60 s mini-plot
- **Calibration wizard:** step-by-step guided recording with live plot
- **Follow Me:** full-width live plot with profile overlay, action instruction card, alert card
- **Profile Manager:** list and inspect JSON profiles
- Live `TempPlotWidget` shared across views: actual temp (blue), target profile (amber dashed), ±tolerance band (red)

---

## Open Questions / TODOs

- [x] ~~Identify MCU to use for Phase 1~~ → **RP2040 Pico + MicroPython**
- [x] ~~Decide on Python UI approach~~ → **Dear PyGui native desktop app**
- [x] ~~Define profile file naming and storage convention~~ → `profiles/<name>.json`, calibrations in `calibrations/<name>.json`
- [x] ~~Define tolerance band for Follow Me deviation alerts~~ → **±5 °C default** (configurable per session)
- [ ] Confirm thermocouple routing — door seal gap vs. drilled grommet
- [ ] Enumerate oven dial positions (how many discrete settings, approximate power per step) — drives "turn N notches" instruction in Follow Me
- [ ] Test MAX31855K noise at 1800 W load (check for EMI on SPI lines)
- [ ] Phase 2: identify suitable SSR models for 230 V / 1800 W load (Crydom D2425/D4825 or equivalent)
- [ ] Phase 2b: select display module (ILI9341 320×240 TFT vs. SSD1306 128×64 OLED vs. Nextion)