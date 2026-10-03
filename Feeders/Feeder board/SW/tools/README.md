# RS485 command sender

A small PC-side tool for driving the feeder RS485 protocol directly from
a USB-RS485 adapter, instead of only through the debug port's text
commands. See `../PROTOCOL.md` for the frame format and full command
reference this implements against.

## Setup

```
pip install -r requirements.txt
```

## Usage

```
python rs485_sender.py --list              # see available COM ports
python rs485_sender.py --port COM5         # open an interactive REPL
```

If the USB-RS485 adapter needs the host to drive its direction pin
manually (some cheap adapters wire DE to RTS instead of auto-detecting
direction), add `--rts-tx`.

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
