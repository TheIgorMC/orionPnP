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
shape is known. The **Target addr** box plus Ping / Get Status / Identify /
Stop motors stay visible above the tabs:

- **Address setup.** Feeders boot unassigned every power-up. **Scan +
  assign** does discovery and assignment in one click when exactly one
  unassigned feeder answers, then sets Target addr. Plain **Scan**
  auto-fills the Nonce box for a manual **Assign**.
- **Feed & peel test.** Set the pitch (Set pitch reads it back), then
  Feed once / Peel once, or **Run cycle test**: N cycles of feed-then-peel
  (or peel-then-feed, or just one of them), waiting for each ACK and
  logging how long each step took, with min/avg/max at the end. In
  v0.01a, `CMD_FEED_NEXT` only turns the sprocket and never runs the
  peel motor, so this host-side sequencing is the only way to pair them
  for now. They can't overlap: the feeder doesn't listen to the bus while
  a motor is running.
- **Jog & zero** (v0.02 firmware). Buttons jog the sprocket by -4 ... +4 mm
  (4 mm = one tooth) or a custom distance, each reply shows the new raw
  angle, then **Set zero here** stores it as the tape zero.
- **Peel calibration** (Feed & peel tab, v0.02). Find the peel time with
  the cycle test, **Save time above to feeder** keeps it on that feeder,
  and "Peel steps use the feeder's saved time" makes the cycle test and
  CMD_PEEL use it (leave the packet builder's duration blank for the same).
- **EEPROM / serial** (v0.02). **I2C scan** shows what answers on the
  feeder's bus. If Get serial NACKs but the scan shows 0x50 and no 0x58,
  the chip is a plain AT24C02 with no factory serial: **Random** +
  **Program serial** writes one (read back to verify). Also writes tape
  width.
- **Packet builder.** Pick a command and fill in named fields (pitch in
  mm, peel time in ms, dropdowns for direction/motor/on-off). It shows
  what the command does, which reply to expect, gotchas, and a live
  byte-by-byte preview of the exact frame including the CRC. **Copy hex**
  puts the frame on the clipboard for use in another serial terminal. The
  payload hex box can be edited by hand to send something malformed.
  Broadcast-only commands (Discover/Assign) always go to addr 0x00.

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
