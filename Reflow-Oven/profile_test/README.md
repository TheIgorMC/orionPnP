# Reflow Oven Tester UI (profile_test)

Simplified desktop UI to test current oven status and controls, with a layout that is easier to port to an SPI LCD later.

## Features

- Flat UI structure: status row, live plot, control rows, profile point table
- Serial connection to firmware line format (`time_ms,temp_C,top,bottom`)
- CSV playback simulation mode using existing logs
- Profile load/save and point-based editing (stage/time/temp)
- Preheat mode and profile mode
- Manual element commands (A/T/B/X/S/M/P/R)
- Control rule:
	- In `preheat` and `soak`, if measured temperature is above target, target is clamped to current temperature
	- In `cooldown`, warning `OPEN DOOR (cooldown too slow)` is shown when cooling lags target

## Run

```bash
cd Reflow-Oven/profile_test
pip install -r requirements.txt
python oven_tester_ui.py
```

## Defaults

- Profile: `../v01_test/leaded.json`
- Simulation CSV: `../v01_test/reflowTest_001.csv`
