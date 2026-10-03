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

Pick a port, Connect, then either use a Quick action button (Scan, Ping,
Get Status, Stop, Identify, Assign) or build a custom addr/cmd/payload
frame in the "Custom command" panel and hit Send. Every frame that
arrives — reply or otherwise — shows up in the log below, decoded where
the payload shape is known (magnet health, error codes, angle in
degrees, ...). If the adapter needs the host to drive its direction pin
manually (some cheap adapters wire DE to RTS instead of auto-detecting
direction), tick "RTS controls TX (DE)" before connecting.

A typical first session on a bus with one feeder: **Scan** → copy the
`nonce` from the reply that shows up in the log into the Nonce field,
pick a new address, **Assign** → **Ping** that address to confirm it
took → **Get Status** for live telemetry.

## CLI / REPL

```
python rs485_sender.py --list              # see available COM ports
python rs485_sender.py --port COM5         # open an interactive REPL
```

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
