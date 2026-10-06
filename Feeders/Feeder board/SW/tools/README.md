# RS485 command sender

Small PC-side tools for driving the feeder RS485 protocol directly from a
USB-RS485 adapter, instead of only through the debug port's text
commands. See `../PROTOCOL.md` for the frame format and full command
reference these implement against. Two front ends, same protocol logic
(`rs485_protocol.py`, shared by both):

- **`rs485_gui.py`** — Tkinter GUI. No separate install beyond pyserial
  (Tkinter ships with Python). Probably the one to start with.
- **`rs485_sender.py`** — command-line REPL, same commands as the GUI's
  "Custom command" panel, handy for scripting or an SSH session.

## Setup

```
pip install -r requirements.txt
```

## GUI

```
python rs485_gui.py
```

Pick a port and Connect. The baud is pre-filled from the newest firmware
folder's `RS485_BAUD` (read from `../<firmware>/src/main.cpp`); pick a
different firmware in the dropdown if the board runs an older build.
If the adapter needs the host to drive its direction pin manually (some
cheap adapters wire DE to RTS instead of auto-detecting direction), tick
"RTS controls TX (DE)" before connecting.

Every frame that arrives shows up in the log, decoded where the payload
shape is known (the once-a-second live refresh and the config read stay
out of it). Port, address and a few settings are remembered in
`~/.orionpnp_rs485_gui.json`.

**The feeder card** (left, always visible) is the part to watch during
first tests: the target **Address**, Ping / Identify / Refresh, a big red
**STOP** (also **Esc**), and live readouts that refresh once a second while
nothing else is running: link health, magnet (detected / none / too weak /
too strong), motor-driver fault, RS485 relay, wheel angle, 12V current
(approximate, from the factory calibration - the feeder's own `CALI` value
may differ) and the last move error. Below that, **Saved on this feeder**
shows component id, tape zero, pitch, tape width, peel time and peel rate,
read automatically the first time the feeder answers (and with **Read from
feeder**). Fields show "n/a" on firmware that doesn't have them. **STOP**
aborts the running sequence and broadcasts `CMD_STOP`; a feeder only hears
it between moves, because it does not listen to the bus while a motor runs.
The Identify button blinks the feeder's status LED white. v0.02b feeders
also flash their status LED green (blue in peel mode) whenever a frame is
addressed to them, so you can see which one is answering.

Tabs:

- **Bring-up.** A first-test checklist: connect, address, link, magnet and
  fault, tape width, pitch, zero, feed once, peel rate. Each row has its
  own button. Ticks come from what the feeder reports, so a feeder that
  was set up earlier shows up already done, and a red cross says why.
- **Feed & peel.** Pitch and Feed once; the **peel rate** (v0.02b: peel
  follows feed, ms per mm) with a helper that turns a measured peel time
  for a known feed into a rate; the fixed-time peel (v0.02: Peel once,
  save/read the saved time, run it); and **Run cycle test**: N cycles of
  feed / peel in the order you pick, waiting for each ACK and logging how
  long each step took, with min/avg/max at the end. With a peel rate saved
  a feed already peels, so use "feed only" (the GUI warns if a cycle would
  peel twice). On v0.02 and earlier the host-side feed-then-peel cycle is
  the only way to pair them. Nothing overlaps: the feeder doesn't listen
  to the bus while a motor is running.
- **Jog & zero** (v0.02+). Buttons jog the sprocket by -4 ... +4 mm
  (4 mm = one tooth) or a custom distance, each reply shows the new raw
  angle, then **Set zero here** stores it as the tape zero. Jog does not
  move the peel motor.
- **Setup.** Address: **Scan** lists every unassigned feeder (nonce,
  component, tape width) so you can pick one and **Assign** it, or **Scan +
  assign** does both when exactly one answers and sets the address. Tape
  width (write/read). Status-LED brightness (v0.02b: saved on the feeder,
  presets 10-255). EEPROM: **I2C scan** shows what answers on the feeder's
  bus; if Get serial NACKs but the scan shows 0x50 and no 0x58, the chip is
  a plain AT24C02 with no factory serial: **Random** + **Program serial**
  writes one (read back to verify).
- **Packet builder.** Pick a command and fill in named fields (pitch in
  mm, peel time in ms, dropdowns for direction/motor/on-off). It shows
  what the command does, which reply to expect, gotchas, and a live
  byte-by-byte preview of the exact frame including the CRC. **Copy hex**
  puts the frame on the clipboard for use in another serial terminal. The
  payload hex box can be edited by hand to send something malformed.
  Broadcast-only commands (Discover/Assign) always go to addr 0x00.

## Trying it without hardware

`sim_feeder.py` (Linux/macOS only - it uses a pseudo-terminal) behaves
roughly like v0.02b firmware: boots unassigned, answers discovery and the
v0.02/v0.02b commands, and goes deaf while a "motor" runs.

```
python sim_feeder.py [--addr N] [--no-magnet]
```

It prints a device path such as `/dev/pts/5`; type that into the GUI's
Port box (or `rs485_sender.py --port`). It is not a faithful model of the
firmware, only enough to exercise the tools.

## CLI / REPL

```
python rs485_sender.py --list              # see available COM ports
python rs485_sender.py --port COM5         # open an interactive REPL
```

`--baud` defaults to the newest firmware's `RS485_BAUD`, same as the GUI.
If the USB-RS485 adapter needs the host to drive its direction pin
manually, add `--rts-tx` (same situation as the GUI's checkbox above).

Inside the REPL, `help` lists every command. There's a named command for
each protocol command (`ping`, `scan`, `getstatus`, `setpitch`, `peel`,
...) with friendly argument parsing, plus `raw <addr> <cmd> [hexbytes...]`
for sending anything else — custom payloads, malformed frames, whatever
you want to throw at a feeder that the named commands don't cover.

Typical first session on a bus with one feeder:

```
RS485> scan
  broadcasting CMD_DISCOVER, listening for 0.50s...
  RX: addr=0x00 cmd=CMD_DISCOVER_HERE payload=... | nonce=0x1A2B componentId=UNSET tapeWidth=8mm
RS485> assign 0x1A2B 5
  RX: addr=0x05 cmd=CMD_ACK payload=05 | ...
RS485> ping 5
  RX: addr=0x05 cmd=CMD_PONG payload=(empty) |
RS485> getstatus 5
  RX: addr=0x05 cmd=CMD_STATUS_INFO payload=... | angle=12.3deg (raw=140) magnet=OK (MD=1 ML=0 MH=0) fault=0 lastMoveErr=ERR_NONE iMonRaw=0 relay=disconnected
```

Addresses, command codes, and `raw`'s hex bytes all accept either decimal
or `0x..` hex. Bus address `0x00` is broadcast.
