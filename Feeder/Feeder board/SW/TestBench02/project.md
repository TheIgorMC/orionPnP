Simple test project for the feeders

2x DRV8833 driving two miniatrue DC motors
One feeds tape, one pulls cover tape

Optocoupler as position encoder via holes in the main feed wheel
Based on a STM32F411CC Blackpill

## Test bench: GO/STOP with hard brake

This test bench uses two digital inputs:

- GO input: runs motor A and motor B forward at low speed (sync)
- STOP input: hard-brakes motor A and motor B

Sketch file:

- `drv8833_testbench.ino`

### Pin map (STM32F411CC Blackpill)

Control inputs:

- GO input: `PB12` (button to GND, `INPUT_PULLUP`)
- STOP input: `PB13` (button to GND, `INPUT_PULLUP`)

DRV8833 logic pins:

- `AIN1` -> `PA8` (PWM)
- `AIN2` -> `PA9`
- `BIN1` -> `PA10` (motor B drive)
- `BIN2` -> `PB9` (motor B drive)
- `nSLEEP` -> `PB14` (set HIGH to enable driver)
- `nFAULT` -> `PB15` (input, active LOW)

DRV8833 power and motor pins (not MCU GPIO):

- `VM` -> motor supply (example: 5V to 10.8V as valid for your setup)
- `GND` -> common ground with Blackpill
- `AOUT1`, `AOUT2` -> Motor A terminals
- `BOUT1`, `BOUT2` -> Motor B terminals

### Behavior

- GO pressed: motor A and B run with a short startup kick (`START_KICK_DUTY = 110` for 300 ms), then low duty (`LOW_SPEED_DUTY = 40`, about 16% PWM)
- STOP pressed: hard brake on both channels (`AIN1 = HIGH`, `AIN2 = HIGH`, `BIN1 = HIGH`, `BIN2 = HIGH`)
- Fault asserted (`nFAULT = LOW`): immediate hard brake on both motors

Logic mapping knobs in firmware:

- `BUTTONS_ACTIVE_LOW = true` for pull-up + button-to-GND wiring
- `SWAP_GO_STOP_ACTIONS = true` if physical GO/STOP controls are wired opposite to labels

If your motor does not move at duty 40, increase gradually (for example 50, 60, 70) until motion is stable.

### USB "device not recognized" note

On STM32F411, USB FS data pins are `PA11` (D-) and `PA12` (D+). If your firmware drives `PA11` as GPIO/PWM, USB enumeration can fail and Windows may report an unrecognized USB device.

This project now keeps only `BIN2` off `PA11` (moved to `PB9`) to avoid that conflict when CDC serial is enabled while preserving the rest of the original motor wiring.

## PlatformIO + DFU

This folder now includes a PlatformIO project:

- `platformio.ini`
- `src/main.cpp`

Default environment:

- `blackpill_f411cc_dfu` (board `genericSTM32F411CC`)

Fallback environment (if your PlatformIO package does not include F411CC board ID):

- `blackpill_f411ce_dfu`

### Build

From this folder:

```bash
pio run -e blackpill_f411cc_dfu
```

### Upload with DFU

1. Connect Blackpill by USB.
2. Put MCU in system bootloader mode (BOOT0 high, then reset).
3. Upload:

```bash
pio run -e blackpill_f411cc_dfu -t upload
```

4. Return BOOT0 low and reset to run firmware normally.

If `genericSTM32F411CC` is not recognized, use:

```bash
pio run -e blackpill_f411ce_dfu -t upload
```
