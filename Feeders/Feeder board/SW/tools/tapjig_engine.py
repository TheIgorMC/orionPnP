"""
TAP-Jig (test-and-program jig) engine: everything the Production tab does,
with no GUI, so it can also run headless (tapjig_run.py) and be tested
against sim_jig.py.

Pieces:
  JigLink   line protocol to the jig's ATmega32u4 over USB-CDC (see TAPJIG.md)
  Dut       the feeder under test, reached over the jig's RS-485 passthrough
  Isp       avrdude/USBasp wrapper (chip erase, fuses, flash + verify)
  Runner    executes a JSON routine (stages -> steps -> checks) and returns a
            per-unit result that is also written to a log

A routine is data: each step is either an action ({"do": "jig.adc", ...}),
a check ({"check": "between(fb.mv, 1180, 1260)"}), a wait or an operator
prompt. Checks are evaluated by a small whitelisting expression evaluator
over the values earlier steps saved, so limits live in the JSON, not here.
"""
import ast
import csv
import datetime
import json
import os
import re
import subprocess
import threading
import time

from rs485_protocol import (
    FrameAssembler,
    build_frame,
    parse_status,
)

CMD_PING, CMD_PONG = 0x01, 0x81
CMD_DISCOVER, CMD_DISCOVER_HERE, CMD_ASSIGN_ADDR = 0x10, 0x90, 0x11
CMD_ACK, CMD_NACK = 0x82, 0x83
CMD_SET_EXT_LED = 0x27
CMD_GET_HW_INFO, CMD_HW_INFO, CMD_SET_HW_INFO = 0x29, 0xA1, 0x2A
CMD_GET_STATUS, CMD_STATUS_INFO = 0x30, 0xA2
CMD_GET_SERIAL, CMD_SERIAL_INFO = 0x33, 0xA3
CMD_I2C_SCAN, CMD_I2C_SCAN_INFO = 0x38, 0xA5
CMD_SET_SERIAL = 0x39
CMD_SET_LED_BRIGHTNESS = 0x3A
CMD_SET_POSITION, CMD_GET_POSITION, CMD_POSITION_INFO = 0x3E, 0x3F, 0xA7
CMD_T_INPUTS, CMD_T_INPUTS_INFO = 0x40, 0xB0
CMD_T_UPTIME, CMD_T_UPTIME_INFO = 0x41, 0xB1
CMD_T_RGB = 0x42
CMD_T_MOTOR = 0x43

IMON_MA_PER_COUNT = 200.0 / 833.0  # factory calibration of the DUT's own IMON reading


class JigError(Exception):
    """The jig answered ERR, did not answer, or answered garbage."""


class StageAbort(Exception):
    """Operator pressed stop."""


class AttrDict(dict):
    """dict with attribute access so routine expressions read `fb.mv`."""
    __getattr__ = dict.get

    def __setattr__(self, k, v):
        self[k] = v


# ---------------------------------------------------------------------------
# Jig link
# ---------------------------------------------------------------------------
def _parse_kv(text: str) -> AttrDict:
    out = AttrDict()
    for tok in text.split():
        if "=" not in tok:
            continue
        k, v = tok.split("=", 1)
        for conv in (int, float):
            try:
                out[k] = conv(v)
                break
            except ValueError:
                continue
        else:
            out[k] = v
    return out


class JigLink:
    """One request, one reply line: 'OK k=v ...' or 'ERR reason'."""

    def __init__(self, ser, default_timeout=3.0):
        self.ser = ser
        self.default_timeout = default_timeout
        self.lock = threading.Lock()

    @classmethod
    def open(cls, port, baud=115200):
        import serial  # imported late so the engine's pure parts work without pyserial
        return cls(serial.Serial(port, baud, timeout=0.1))

    def close(self):
        try:
            self.ser.close()
        except Exception:
            pass

    def _readline(self, timeout):
        end = time.monotonic() + timeout
        buf = bytearray()
        while time.monotonic() < end:
            ch = self.ser.read(1)
            if not ch:
                continue
            if ch == b"\n":
                line = buf.decode("ascii", "replace").strip()
                if line:
                    return line
                buf.clear()
                continue
            buf += ch
        raise JigError("jig did not answer in time")

    def cmd(self, line: str, timeout=None) -> AttrDict:
        with self.lock:
            self.ser.reset_input_buffer()
            self.ser.write((line.strip() + "\n").encode("ascii"))
            self.ser.flush()
            reply = self._readline(timeout or self.default_timeout)
        if reply.startswith("ERR"):
            raise JigError(f"{line.split()[0]}: {reply[3:].strip() or 'error'}")
        if not reply.startswith("OK"):
            raise JigError(f"unexpected jig reply: {reply!r}")
        return _parse_kv(reply[2:])

    def rs485(self, frame: bytes, timeout_ms: int):
        """Send a frame to the DUT via the jig's RS-485 master; return (frames, de_seen)."""
        reply = self.cmd(f"RS485 {frame.hex()} {int(timeout_ms)}", timeout=timeout_ms / 1000.0 + 2.0)
        asm = FrameAssembler()
        frames = []
        for b in bytes.fromhex(str(reply.get("frames", "") or "")):
            f = asm.feed(b)
            if f is not None:
                frames.append(f)
        return frames, bool(reply.get("de", 0))


# ---------------------------------------------------------------------------
# DUT over the jig
# ---------------------------------------------------------------------------
class Dut:
    def __init__(self, jig: JigLink, addr: int = 1):
        self.jig = jig
        self.want_addr = addr
        self.addr = None  # the DUT boots unassigned every time - see discover_assign()
        self.last_de = False

    def request(self, cmd, payload=b"", reply_cmds=(), timeout_ms=400, addr=None):
        """Send to the DUT and return the first reply frame whose cmd is ACK/NACK or in reply_cmds, else None."""
        a = self.addr if addr is None else addr
        if a is None:
            raise JigError("DUT has no bus address yet (discover_assign first)")
        frames, de = self.jig.rs485(build_frame(a, cmd, payload), timeout_ms)
        self.last_de = de
        wanted = {CMD_ACK, CMD_NACK, *reply_cmds}
        for f in frames:
            if f.addr == a and f.cmd in wanted:
                return f
        return None

    def discover_assign(self, window_ms=700):
        frames, _ = self.jig.rs485(build_frame(0x00, CMD_DISCOVER), window_ms)
        found = [f for f in frames if f.cmd == CMD_DISCOVER_HERE and len(f.payload) >= 5]
        if len(found) != 1:
            return AttrDict(ok=False, count=len(found), addr=None)
        nonce = bytes(found[0].payload[:2])
        frames, _ = self.jig.rs485(build_frame(0x00, CMD_ASSIGN_ADDR, nonce + bytes([self.want_addr])), 600)
        ok = any(f.addr == self.want_addr and f.cmd == CMD_ACK for f in frames)
        if ok:
            self.addr = self.want_addr
        p = found[0].payload
        slot = {}
        if len(p) >= 8:  # v0.02b+: what the feeder remembered from before this boot
            px = (p[6] << 8) | p[7]
            slot = {"last_addr": p[5], "pos_x": None if px == 0xFFFF else px}
        return AttrDict(ok=ok, count=1, addr=self.addr if ok else None, nonce=int.from_bytes(nonce, "big"), **slot)

    def forget(self):
        self.addr = None


# ---------------------------------------------------------------------------
# ISP
# ---------------------------------------------------------------------------
class Isp:
    """avrdude + USBasp. dry_run skips the programmer (bench use with sim_jig)."""

    def __init__(self, cfg: dict, hex_paths: dict, avrdude="avrdude", dry_run=False, log=lambda s: None):
        self.cfg = cfg
        self.hex_paths = hex_paths
        self.avrdude = avrdude
        self.dry_run = dry_run
        self.log = log

    def _run(self, args, timeout=120):
        argv = [self.avrdude, "-c", self.cfg.get("programmer", "usbasp"), "-p", self.cfg.get("part", "m328pb"),
                *self.cfg.get("extra_args", []), *args]
        self.log("$ " + " ".join(argv))
        if self.dry_run:
            return 0, "(dry run)"
        proc = subprocess.run(argv, capture_output=True, text=True, timeout=timeout)
        return proc.returncode, (proc.stdout + proc.stderr).strip()

    def program(self, which: str, fuses: bool, erase: bool = True):
        hex_path = self.hex_paths.get(which) or ("(dry run)" if self.dry_run else "")
        if not hex_path:
            raise JigError(f"no {which} hex file set")
        if not self.dry_run and not os.path.isfile(hex_path):
            raise JigError(f"{which} hex not found: {hex_path}")
        out = []
        steps = []
        if fuses:
            f = self.cfg.get("fuses", {})
            args = []
            for name in ("lfuse", "hfuse", "efuse"):
                if name in f:
                    args += ["-U", f"{name}:w:{f[name]}:m"]
            steps.append(("fuses", args))
        if erase:
            steps.append(("erase", ["-e"]))
        # after an explicit chip erase, -D stops avrdude erasing a second time before the write
        steps.append(("flash", (["-D"] if erase else []) + ["-U", f"flash:w:{hex_path}:i", "-U", f"flash:v:{hex_path}:i"]))
        for name, args in steps:
            rc, text = self._run(args)
            out.append(f"[{name}] rc={rc}")
            if rc != 0:
                return AttrDict(ok=False, step=name, rc=rc, output="\n".join(out) + "\n" + text[-800:])
            time.sleep(float(self.cfg.get("settle_s", 1.0)) if not self.dry_run else 0)
        return AttrDict(ok=True, step="flash", rc=0, output="\n".join(out))


# ---------------------------------------------------------------------------
# Safe expression evaluator for `check`
# ---------------------------------------------------------------------------
def _between(x, lo, hi):
    return x is not None and lo <= x <= hi


def _within(x, target, tol):
    return x is not None and abs(x - target) <= tol


def _angdiff(a, b):
    return (a - b) % 360.0


def _angdist(a, b):
    d = (a - b) % 360.0
    return min(d, 360.0 - d)


SAFE_FUNCS = {"between": _between, "within": _within, "abs": abs, "min": min, "max": max, "len": len,
              "round": round, "angdiff": _angdiff, "angdist": _angdist, "any": any, "all": all}
_ALLOWED_NODES = (ast.Expression, ast.BoolOp, ast.And, ast.Or, ast.UnaryOp, ast.Not, ast.USub, ast.UAdd,
                  ast.Compare, ast.Eq, ast.NotEq, ast.Lt, ast.LtE, ast.Gt, ast.GtE, ast.In, ast.NotIn, ast.Is, ast.IsNot,
                  ast.BinOp, ast.Add, ast.Sub, ast.Mult, ast.Div, ast.Mod, ast.Constant, ast.Name, ast.Load,
                  ast.Attribute, ast.Subscript, ast.Index if hasattr(ast, "Index") else ast.Constant, ast.Call,
                  ast.List, ast.Tuple, ast.IfExp)


def evaluate(expr: str, ctx: dict):
    """Evaluate a routine expression. Returns (value, {name: value} for every variable it read)."""
    tree = ast.parse(expr, mode="eval")
    used = {}
    for node in ast.walk(tree):
        if not isinstance(node, _ALLOWED_NODES):
            raise ValueError(f"'{type(node).__name__}' is not allowed in a check")
        if isinstance(node, ast.Attribute) and node.attr.startswith("_"):
            raise ValueError("private attributes are not allowed")
        if isinstance(node, ast.Call) and not (isinstance(node.func, ast.Name) and node.func.id in SAFE_FUNCS):
            raise ValueError("only whitelisted functions can be called in a check")

    def ev(n):
        if isinstance(n, ast.Expression):
            return ev(n.body)
        if isinstance(n, ast.Constant):
            return n.value
        if isinstance(n, ast.Name):
            if n.id in SAFE_FUNCS:
                return SAFE_FUNCS[n.id]
            if n.id not in ctx:
                raise KeyError(f"'{n.id}' has no value yet")
            used[n.id] = ctx[n.id]
            return ctx[n.id]
        if isinstance(n, ast.Attribute):
            base = ev(n.value)
            return base.get(n.attr) if isinstance(base, dict) else getattr(base, n.attr)
        if isinstance(n, ast.Subscript):
            sl = n.slice.value if hasattr(ast, "Index") and isinstance(n.slice, ast.Index) else n.slice
            return ev(n.value)[ev(sl)]
        if isinstance(n, ast.BoolOp):
            vals = [ev(v) for v in n.values]
            return all(vals) if isinstance(n.op, ast.And) else any(vals)
        if isinstance(n, ast.UnaryOp):
            v = ev(n.operand)
            return (not v) if isinstance(n.op, ast.Not) else (-v if isinstance(n.op, ast.USub) else +v)
        if isinstance(n, ast.BinOp):
            a, b = ev(n.left), ev(n.right)
            return {ast.Add: lambda: a + b, ast.Sub: lambda: a - b, ast.Mult: lambda: a * b,
                    ast.Div: lambda: a / b, ast.Mod: lambda: a % b}[type(n.op)]()
        if isinstance(n, ast.Compare):
            left = ev(n.left)
            for op, comp in zip(n.ops, n.comparators):
                right = ev(comp)
                ok = {ast.Eq: lambda: left == right, ast.NotEq: lambda: left != right, ast.Lt: lambda: left < right,
                      ast.LtE: lambda: left <= right, ast.Gt: lambda: left > right, ast.GtE: lambda: left >= right,
                      ast.In: lambda: left in right, ast.NotIn: lambda: left not in right,
                      ast.Is: lambda: left is right, ast.IsNot: lambda: left is not right}[type(op)]()
                if not ok:
                    return False
                left = right
            return True
        if isinstance(n, ast.Call):
            return SAFE_FUNCS[n.func.id](*[ev(a) for a in n.args])
        if isinstance(n, (ast.List, ast.Tuple)):
            return [ev(e) for e in n.elts]
        if isinstance(n, ast.IfExp):
            return ev(n.body) if ev(n.test) else ev(n.orelse)
        raise ValueError(f"unsupported expression node {type(n).__name__}")

    return ev(tree), used


def _plain(v):
    if isinstance(v, dict):
        return {k: _plain(x) for k, x in v.items()}
    if isinstance(v, (list, tuple)):
        return [_plain(x) for x in v]
    if isinstance(v, bytes):
        return v.hex()
    return v


def _short(v):
    s = json.dumps(_plain(v), default=str)
    return s if len(s) <= 90 else s[:87] + "..."


# ---------------------------------------------------------------------------
# Runner
# ---------------------------------------------------------------------------
def load_routine(path: str) -> dict:
    with open(path, encoding="utf-8") as f:
        routine = json.load(f)
    for key in ("test_id", "stages"):
        if key not in routine:
            raise ValueError(f"routine is missing '{key}'")
    seen = set()
    for st in routine["stages"]:
        if st["id"] in seen:
            raise ValueError(f"duplicate stage id {st['id']}")
        seen.add(st["id"])
        for step in st.get("steps", []) + st.get("cleanup", []):
            if not any(k in step for k in ("do", "check", "wait_ms", "prompt")):
                raise ValueError(f"stage {st['id']}: step needs do/check/wait_ms/prompt: {step}")
            if "check" in step:
                evaluate_syntax_only(step["check"])
    return routine


def evaluate_syntax_only(expr: str):
    tree = ast.parse(expr, mode="eval")
    for node in ast.walk(tree):
        if not isinstance(node, _ALLOWED_NODES):
            raise ValueError(f"'{type(node).__name__}' is not allowed in a check: {expr}")
        if isinstance(node, ast.Call) and not (isinstance(node.func, ast.Name) and node.func.id in SAFE_FUNCS):
            raise ValueError(f"only whitelisted functions can be called in a check: {expr}")


class Runner:
    """Runs a routine against a jig. UI hooks are plain callables so the same
    runner serves the GUI (marshalled onto the Tk thread by the caller) and the CLI."""

    def __init__(self, routine, jig: JigLink, isp: Isp, options=None,
                 on_stage=None, on_log=None, prompt=None, abort: threading.Event = None,
                 on_stage_result=None):
        self.routine = routine
        self.jig = jig
        self.isp = isp
        self.options = AttrDict(options or {})
        self.dut = Dut(jig, routine.get("bus_addr", 1))
        self.on_stage = on_stage or (lambda *a: None)
        self.on_stage_result = on_stage_result or (lambda sr: None)
        self.on_log = on_log or (lambda s: None)
        self.prompt = prompt or (lambda msg: True)
        self.abort = abort or threading.Event()
        self.actions = self._build_actions()

    # --- actions ---------------------------------------------------------
    def _build_actions(self):
        jig, dut, isp = self.jig, self.dut, self.isp

        def jig_cmd(fmt):
            return lambda **kw: jig.cmd(fmt.format(**kw))

        def ldr(which, samples=8):
            return jig.cmd(f"LDR {which} {samples}")

        def status():
            f = dut.request(CMD_GET_STATUS, reply_cmds=(CMD_STATUS_INFO,))
            if f is None or f.cmd != CMD_STATUS_INFO:
                return AttrDict(ok=False)
            st = parse_status(bytes(f.payload)) or {}
            return AttrDict(ok=True, **st)

        def ping():
            t0 = time.monotonic()
            f = dut.request(CMD_PING, reply_cmds=(CMD_PONG,))
            return AttrDict(ok=bool(f is not None and f.cmd == CMD_PONG), de=dut.last_de,
                            ms=round((time.monotonic() - t0) * 1000, 1))

        def i2c_scan():
            f = dut.request(CMD_I2C_SCAN, reply_cmds=(CMD_I2C_SCAN_INFO,))
            if f is None or f.cmd != CMD_I2C_SCAN_INFO:
                return AttrDict(ok=False, addrs=[], has_as5600=False, has_eeprom=False, has_serial_page=False)
            addrs = list(f.payload)
            return AttrDict(ok=True, addrs=addrs, has_as5600=0x36 in addrs,
                            has_eeprom=any(0x50 <= a <= 0x57 for a in addrs),
                            has_serial_page=any(0x58 <= a <= 0x5F for a in addrs))

        def serial():
            f = dut.request(CMD_GET_SERIAL, reply_cmds=(CMD_SERIAL_INFO,), timeout_ms=600)
            if f is None or f.cmd != CMD_SERIAL_INFO or len(f.payload) < 16:
                return AttrDict(ok=False, valid=False, hex="")
            return AttrDict(ok=True, valid=any(f.payload), hex=bytes(f.payload).hex().upper())

        def inputs():
            f = dut.request(CMD_T_INPUTS, reply_cmds=(0xB0,))
            if f is None or f.cmd != CMD_T_INPUTS_INFO or len(f.payload) < 5:
                return AttrDict(ok=False)
            p = f.payload
            imon = (p[1] << 8) | p[2]
            return AttrDict(ok=True, sw1=p[0] & 1, sw2=(p[0] >> 1) & 1, fault=(p[0] >> 2) & 1, relay=(p[0] >> 3) & 1,
                            magnet=(p[0] >> 4) & 1, imon_raw=imon, imon_ma=round(imon * IMON_MA_PER_COUNT, 1),
                            v5_raw=(p[3] << 8) | p[4])

        def uptime():
            f = dut.request(CMD_T_UPTIME, reply_cmds=(CMD_T_UPTIME_INFO,), timeout_ms=250)
            if f is None or f.cmd != CMD_T_UPTIME_INFO or len(f.payload) < 5:
                return AttrDict(ok=False)
            p = f.payload
            return AttrDict(ok=True, ms=int.from_bytes(bytes(p[:4]), "big"), mcusr=p[4], ext_reset=bool(p[4] & 0x02))

        def rgb(r=0, g=0, b=0):
            f = dut.request(CMD_T_RGB, bytes([r, g, b]))
            return AttrDict(ok=bool(f is not None and f.cmd == CMD_ACK))

        def ext_led(on=True):
            f = dut.request(CMD_SET_EXT_LED, bytes([1 if on else 0]))
            return AttrDict(ok=bool(f is not None and f.cmd == CMD_ACK))

        def motor(motor, direction=0, ms=1000):
            f = dut.request(CMD_T_MOTOR, bytes([motor, direction, max(1, min(255, ms // 10))]), timeout_ms=ms + 600)
            if f is None or f.cmd != CMD_ACK:
                return AttrDict(ok=False, fault=None)
            return AttrDict(ok=True, fault=bool(f.payload[0]) if f.payload else False)

        def set_position(x):
            f = dut.request(CMD_SET_POSITION, int(x).to_bytes(2, "big"), timeout_ms=600)
            return AttrDict(ok=bool(f is not None and f.cmd == CMD_ACK))

        def get_position():
            f = dut.request(CMD_GET_POSITION, reply_cmds=(CMD_POSITION_INFO,))
            if f is None or f.cmd != CMD_POSITION_INFO or len(f.payload) < 3:
                return AttrDict(ok=False, pos_x=None, last_addr=None)
            px = (f.payload[0] << 8) | f.payload[1]
            return AttrDict(ok=True, pos_x=None if px == 0xFFFF else px, last_addr=f.payload[2])

        def led_brightness(level):
            f = dut.request(CMD_SET_LED_BRIGHTNESS, bytes([int(level)]), timeout_ms=600)
            return AttrDict(ok=bool(f is not None and f.cmd == CMD_ACK))

        def hw_info():
            f = dut.request(CMD_GET_HW_INFO, reply_cmds=(CMD_HW_INFO,))
            if f is None or f.cmd != CMD_HW_INFO or not f.payload:
                return AttrDict(ok=False, width=None)
            return AttrDict(ok=True, width=None if f.payload[0] == 0xFF else f.payload[0])

        def write_width(mm):
            f = dut.request(CMD_SET_HW_INFO, bytes([int(mm)]), timeout_ms=600)
            return AttrDict(ok=bool(f is not None and f.cmd == CMD_ACK))

        def write_serial(hex=None):
            data = bytes.fromhex(hex) if hex else os.urandom(16)
            f = dut.request(CMD_SET_SERIAL, data, timeout_ms=800)
            if f is not None and f.cmd == CMD_NACK:
                return AttrDict(ok=False, locked=bool(f.payload and f.payload[0] == 0x08), serial=data.hex().upper())
            return AttrDict(ok=bool(f is not None and f.cmd == CMD_ACK), locked=False, serial=data.hex().upper())

        def program(which, fuses=False, erase=True):
            r = isp.program(which, fuses=fuses, erase=erase)
            self.on_log(r.output)
            try:
                jig.cmd(f"ISPDONE {which}")  # lets the jig release/cycle the DUT so the new firmware starts
            except JigError:
                pass
            dut.forget()  # a freshly programmed DUT boots unassigned
            return r

        def sleep(ms):
            self._sleep(ms / 1000.0)
            return AttrDict(ok=True)

        return {
            "jig.hello": jig_cmd("HELLO"), "jig.safe": jig_cmd("SAFE"), "jig.rearm": jig_cmd("REARM"),
            "jig.state": jig_cmd("STATE"),
            "jig.cold": jig_cmd("COLD {rail}"), "jig.cont": jig_cmd("CONT {name}"),
            "jig.psu_on": lambda limit_ma=300: jig.cmd(f"PSU ON {int(limit_ma)}"),
            "jig.psu_off": lambda: jig.cmd("PSU OFF"),
            "jig.adc": jig_cmd("ADC {node}"), "jig.dig": jig_cmd("DIG {name}"),
            "jig.sw": lambda name, pressed: jig.cmd(f"SW {name} {1 if pressed else 0}"),
            "jig.reset_pulse": lambda ms=50: jig.cmd(f"RESET {int(ms)}"),
            "jig.ldr": ldr, "jig.servo": lambda deg: jig.cmd(f"SERVO {deg}"),
            "jig.stall": lambda ch, on=True: jig.cmd(f"STALL {ch} {1 if on else 0}"),
            "jig.enc_arm": lambda ch, window_ms=1500: jig.cmd(f"ENCARM {ch} {int(window_ms)}"),
            "jig.enc_read": lambda ch: jig.cmd(f"ENCREAD {ch}"),
            "dut.discover_assign": lambda: dut.discover_assign(),
            "dut.ping": ping, "dut.status": status, "dut.i2c_scan": i2c_scan, "dut.serial": serial,
            "dut.inputs": inputs, "dut.uptime": uptime, "dut.rgb": rgb, "dut.ext_led": ext_led, "dut.motor": motor,
            "dut.set_position": set_position, "dut.get_position": get_position, "dut.led_brightness": led_brightness,
            "dut.hw_info": hw_info, "dut.write_width": write_width, "dut.write_serial": write_serial,
            "isp.program": program, "util.sleep": sleep,
        }

    def _sleep(self, seconds):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            if self.abort.is_set():
                raise StageAbort()
            time.sleep(min(0.05, max(0.0, end - time.monotonic())))

    # --- execution ---------------------------------------------------------
    def _run_steps(self, steps, ctx, stage_result):
        for step in steps:
            if self.abort.is_set():
                raise StageAbort()
            cond = step.get("if")
            if cond is not None:
                value, _ = evaluate(cond, ctx)
                if not value:
                    continue
            if "wait_ms" in step:
                self._sleep(step["wait_ms"] / 1000.0)
            elif "prompt" in step:
                if not self.prompt(step["prompt"]):
                    raise StageAbort()
            elif "do" in step:
                name = step["do"]
                fn = self.actions.get(name)
                if fn is None:
                    raise ValueError(f"unknown action '{name}'")
                params = {k: v for k, v in step.items() if k not in ("do", "save", "if", "label", "args")}
                params.update(step.get("args", {}))
                # string params may reference earlier values: "$name.field"
                for k, v in list(params.items()):
                    if isinstance(v, str) and v.startswith("="):
                        params[k], _ = evaluate(v[1:], ctx)
                result = fn(**params)
                if "save" in step:
                    ctx[step["save"]] = result
                    stage_result["measurements"][step["save"]] = _plain(result)
                if name == "dut.serial" and isinstance(result, dict) and result.get("valid") and not ctx["unit"].get("serial"):
                    ctx["unit"]["serial"] = result["hex"]
            elif "check" in step:
                value, used = evaluate(step["check"], ctx)
                stage_result["checks"].append({
                    "label": step.get("label", step["check"]), "expr": step["check"], "ok": bool(value),
                    "advisory": bool(step.get("advisory", False)),
                    "values": {k: _plain(v) for k, v in used.items()}})

    def run(self, stage_ids=None, stop_on_fail=True, serial_hint=None):
        started = time.time()
        ctx = {"opt": self.options, "unit": AttrDict(serial=serial_hint or ""),
               "p": AttrDict(self.routine.get("params", {}))}
        result = {
            "test_id": self.routine["test_id"], "board_rev": self.routine.get("board_rev", ""),
            "fw_versions": self.routine.get("fw_versions", {}), "options": _plain(self.options),
            "started": datetime.datetime.fromtimestamp(started).isoformat(timespec="seconds"),
            "stages": [], "serial": "", "passed": False,
        }
        stages = [s for s in self.routine["stages"] if stage_ids is None or s["id"] in stage_ids]
        for st in stages:
            self.on_stage(st["id"], "pending", "")
        overall_ok = True
        try:
            for st in stages:
                if self.abort.is_set():
                    break
                sr = {"id": st["id"], "name": st["name"], "status": "running", "checks": [], "measurements": {},
                      "error": "", "seconds": 0.0}
                result["stages"].append(sr)
                self.on_stage(st["id"], "running", "")
                t0 = time.monotonic()
                try:
                    try:
                        self._run_steps(st.get("steps", []), ctx, sr)
                        # advisory checks are recorded and shown but never fail a stage (limits not calibrated yet)
                        sr["status"] = "pass" if all(c["ok"] or c["advisory"] for c in sr["checks"]) else "fail"
                    finally:
                        try:
                            self._run_steps(st.get("cleanup", []), ctx, sr)
                        except StageAbort:
                            raise
                        except Exception as exc:
                            sr["error"] = (sr["error"] + f" cleanup: {exc}").strip()
                except StageAbort:
                    sr["status"] = "aborted"
                except Exception as exc:
                    sr["status"] = "error"
                    sr["error"] = f"{type(exc).__name__}: {exc}"
                sr["seconds"] = round(time.monotonic() - t0, 2)
                bad = [c for c in sr["checks"] if not c["ok"] and not c["advisory"]]
                warn = [c for c in sr["checks"] if not c["ok"] and c["advisory"]]
                detail = sr["error"] or (f"{bad[0]['label']}  {bad[0]['values']}" if bad else
                                         f"{len(sr['checks'])} checks" + (f", {len(warn)} advisory not met" if warn else ""))
                self.on_stage(st["id"], sr["status"], detail)
                self.on_stage_result(sr)
                self.on_log(f"stage {st['id']} {st['name']}: {sr['status'].upper()} ({sr['seconds']}s) {detail if sr['status'] != 'pass' else ''}".rstrip())
                if sr["status"] != "pass":
                    overall_ok = False
                    if sr["status"] == "aborted" or (stop_on_fail and not st.get("continue_on_fail")):
                        break
        finally:
            try:
                self.jig.cmd("SAFE")  # PSU off, switches released, servo parked - whatever happened above
            except Exception as exc:
                overall_ok = False
                self.on_log(f"WARNING: could not put the jig in its safe state: {exc}")
        ran = {s["id"] for s in result["stages"]}
        if stage_ids is None and any(s["id"] not in ran for s in stages):
            overall_ok = False  # stopped early
        result["serial"] = ctx["unit"].get("serial", "")
        result["passed"] = overall_ok and bool(result["stages"]) and not self.abort.is_set()
        result["seconds"] = round(time.time() - started, 1)
        result["finished"] = datetime.datetime.now().isoformat(timespec="seconds")
        return result


def write_log(result: dict, log_dir: str) -> str:
    """One JSON file per unit run plus a one-line-per-run summary.csv."""
    day = result["started"][:10]
    folder = os.path.join(log_dir, day)
    os.makedirs(folder, exist_ok=True)
    stamp = re.sub(r"[^0-9]", "", result["started"])[8:14]
    sn = result.get("serial") or "NOSERIAL"
    verdict = "PASS" if result["passed"] else "FAIL"
    path = os.path.join(folder, f"{sn}_{stamp}_{verdict}.json")
    with open(path, "w", encoding="utf-8") as f:
        json.dump(result, f, indent=1, default=str)
    failed = next((s for s in result["stages"] if s["status"] != "pass"), None)
    summary = os.path.join(log_dir, "summary.csv")
    new = not os.path.exists(summary)
    with open(summary, "a", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        if new:
            w.writerow(["started", "serial", "result", "failed_stage", "seconds", "test_id", "operator"])
        w.writerow([result["started"], sn, verdict, f"{failed['id']} {failed['name']}" if failed else "",
                    result.get("seconds", ""), result["test_id"], result.get("options", {}).get("operator", "")])
    return path
