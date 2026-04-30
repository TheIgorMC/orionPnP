"""Simplified reflow oven tester UI.

Design goal: keep a flat, explicit state-machine structure so the same logic can
be moved to a small SPI LCD UI with minimal refactor.

Run:
    python oven_tester_ui.py
"""

from __future__ import annotations

import csv
import json
import queue
import threading
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

import tkinter as tk
from tkinter import filedialog, messagebox, ttk

from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg
from matplotlib.figure import Figure

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    serial = None
    list_ports = None


ROOT = Path(__file__).resolve().parent
DEFAULT_PROFILE = ROOT.parent / "v01_test" / "leaded.json"
DEFAULT_CSV = ROOT.parent / "v01_test" / "reflowTest_001.csv"


@dataclass
class Sample:
    t_ms: int
    temp_c: float
    top: int
    bottom: int


@dataclass
class ProfilePoint:
    stage: str
    time_s: float
    temp_c: float


class SerialLink:
    def __init__(self, on_sample):
        self.on_sample = on_sample
        self.connected_port = ""
        self._stop = threading.Event()
        self._thread: Optional[threading.Thread] = None
        self._ser = None

    def available_ports(self) -> list[str]:
        if list_ports is None:
            return []
        return [p.device for p in list_ports.comports()]

    def connect(self, port: str, baud: int = 115200) -> None:
        if serial is None:
            raise RuntimeError("pyserial not installed")
        self.disconnect()
        self._stop.clear()
        self.connected_port = port
        self._thread = threading.Thread(target=self._reader_loop, args=(port, baud), daemon=True)
        self._thread.start()

    def disconnect(self) -> None:
        self._stop.set()
        if self._thread and self._thread.is_alive():
            self._thread.join(timeout=1.0)
        self._thread = None
        if self._ser is not None:
            try:
                if self._ser.is_open:
                    self._ser.close()
            except Exception:
                pass
        self._ser = None
        self.connected_port = ""

    def send(self, cmd: str) -> None:
        if self._ser is None or not self._ser.is_open:
            return
        self._ser.write(cmd.encode("ascii"))
        self._ser.flush()

    def _reader_loop(self, port: str, baud: int) -> None:
        try:
            with serial.Serial(port, baud, timeout=0.8) as ser:
                self._ser = ser
                while not self._stop.is_set():
                    raw = ser.readline()
                    if not raw:
                        continue
                    line = raw.decode("utf-8", errors="ignore").strip()
                    if not line:
                        continue
                    if line.startswith("#") or line.lower().startswith("time_ms"):
                        continue
                    sample = parse_data_line(line)
                    if sample is not None:
                        self.on_sample(sample)
        finally:
            self._ser = None


class CsvReplay:
    def __init__(self, on_sample):
        self.on_sample = on_sample
        self.running = False
        self._stop = threading.Event()
        self._thread: Optional[threading.Thread] = None

    def start(self, csv_path: Path) -> None:
        self.stop()
        self._stop.clear()
        self.running = True
        self._thread = threading.Thread(target=self._loop, args=(csv_path,), daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        if self._thread and self._thread.is_alive():
            self._thread.join(timeout=1.0)
        self._thread = None
        self.running = False

    def _loop(self, csv_path: Path) -> None:
        try:
            with csv_path.open("r", encoding="utf-8", newline="") as f:
                rows = [r for r in csv.DictReader(f) if r.get("device_time_ms")]

            for idx, row in enumerate(rows):
                if self._stop.is_set():
                    return
                try:
                    sample = Sample(
                        t_ms=int(float(row["device_time_ms"])),
                        temp_c=float(row["temp_C"]),
                        top=int(float(row.get("top", 0) or 0)),
                        bottom=int(float(row.get("bottom", 0) or 0)),
                    )
                except ValueError:
                    continue

                self.on_sample(sample)

                if idx + 1 < len(rows):
                    t0 = int(float(rows[idx]["device_time_ms"]))
                    t1 = int(float(rows[idx + 1]["device_time_ms"]))
                    dt = max(0.05, min(1.0, (t1 - t0) / 1000.0))
                    end_t = time.time() + dt
                    while time.time() < end_t and not self._stop.is_set():
                        time.sleep(0.01)
        finally:
            self.running = False


class ProfileModel:
    def __init__(self):
        self.path: Optional[Path] = None
        self.paste = ""
        self.liquidus_temp = 183
        self.points: list[ProfilePoint] = []

    def load(self, path: Path) -> None:
        data = json.loads(path.read_text(encoding="utf-8"))
        raw_points = data.get("profile_points", [])
        points = []
        for item in raw_points:
            points.append(
                ProfilePoint(
                    stage=str(item.get("stage", "Stage")),
                    time_s=float(item["time_s"]),
                    temp_c=float(item["temp_c"]),
                )
            )
        if len(points) < 2:
            raise ValueError("Profile must contain at least two profile_points")

        self.path = path
        self.paste = str(data.get("paste", ""))
        self.liquidus_temp = int(data.get("liquidus_temp", 183))
        self.points = sorted(points, key=lambda p: p.time_s)

    def save(self, path: Optional[Path] = None) -> Path:
        target = path or self.path
        if target is None:
            raise ValueError("No profile path selected")
        data = {
            "paste": self.paste,
            "liquidus_temp": self.liquidus_temp,
            "profile_points": [
                {"stage": p.stage, "time_s": p.time_s, "temp_c": p.temp_c}
                for p in self.points
            ],
            "constraints": {
                "ramp_up_max": 3.0,
                "ramp_down_max": 4.0,
                "time_above_liquidus_sec": [30, 90],
            },
        }
        target.write_text(json.dumps(data, indent=2), encoding="utf-8")
        self.path = target
        return target

    def target_at(self, elapsed_s: float) -> tuple[float, str]:
        points = self.points
        if elapsed_s <= points[0].time_s:
            return points[0].temp_c, points[0].stage

        for i in range(len(points) - 1):
            p0 = points[i]
            p1 = points[i + 1]
            if p0.time_s <= elapsed_s <= p1.time_s:
                if p1.time_s == p0.time_s:
                    return p1.temp_c, p1.stage
                alpha = (elapsed_s - p0.time_s) / (p1.time_s - p0.time_s)
                temp = p0.temp_c + alpha * (p1.temp_c - p0.temp_c)
                return temp, p1.stage

        last = points[-1]
        return last.temp_c, last.stage


def parse_data_line(line: str) -> Optional[Sample]:
    parts = [p.strip() for p in line.split(",")]
    if len(parts) < 4:
        return None
    try:
        return Sample(
            t_ms=int(parts[0]),
            temp_c=float(parts[1]),
            top=int(parts[2]),
            bottom=int(parts[3]),
        )
    except ValueError:
        return None


class ControlEngine:
    def __init__(self):
        self.mode = "idle"  # idle|preheat|profile
        self.preheat_target = 100.0
        self.hysteresis = 2.0
        self.profile_start_ms: Optional[int] = None
        self.last_warning = ""

    def start_preheat(self, target: float) -> None:
        self.mode = "preheat"
        self.preheat_target = target
        self.profile_start_ms = None
        self.last_warning = ""

    def start_profile(self, start_ms: int) -> None:
        self.mode = "profile"
        self.profile_start_ms = start_ms
        self.last_warning = ""

    def stop(self) -> None:
        self.mode = "idle"
        self.profile_start_ms = None
        self.last_warning = ""

    def step(self, sample: Sample, profile: Optional[ProfileModel]) -> tuple[Optional[float], str, Optional[str]]:
        target: Optional[float] = None
        phase = "idle"
        warning = ""

        if self.mode == "preheat":
            target = self.preheat_target
            phase = "preheat"

        elif self.mode == "profile" and profile is not None and profile.points:
            if self.profile_start_ms is None:
                self.profile_start_ms = sample.t_ms
            elapsed = max(0.0, (sample.t_ms - self.profile_start_ms) / 1000.0)
            target, stage = profile.target_at(elapsed)
            phase = stage.lower()

            # Requested behavior: in preheat/soak, if current > target keep current.
            if ("preheat" in phase or "soak" in phase) and sample.temp_c > target:
                target = sample.temp_c

            # Requested behavior: in cooldown, warn to open the door.
            if "cool" in phase and sample.temp_c > target + 5.0:
                warning = "OPEN DOOR (cooldown too slow)"

        if target is None:
            return None, phase, warning

        low = target - self.hysteresis
        high = target + self.hysteresis
        command = None
        if sample.temp_c < low:
            command = "A"
        elif sample.temp_c > high:
            command = "X"

        self.last_warning = warning
        return target, phase, command


class App:
    def __init__(self, root: tk.Tk):
        self.root = root
        self.root.title("Reflow Tester - Simplified")
        self.root.geometry("1180x760")

        self.sample_queue: queue.Queue[Sample] = queue.Queue(maxsize=2000)
        self.samples: list[Sample] = []
        self.targets: list[Optional[float]] = []

        self.serial = SerialLink(self._push_sample)
        self.replay = CsvReplay(self._push_sample)
        self.profile = ProfileModel()
        self.engine = ControlEngine()

        self._build_ui()
        self._load_default_profile()
        self._refresh_ports()
        self._tick()

    def _build_ui(self) -> None:
        root = ttk.Frame(self.root, padding=8)
        root.pack(fill=tk.BOTH, expand=True)

        status = ttk.Frame(root)
        status.pack(fill=tk.X)

        self.status_var = tk.StringVar(value="Disconnected")
        self.temp_var = tk.StringVar(value="Temp: --.- C")
        self.target_var = tk.StringVar(value="Target: --.- C")
        self.phase_var = tk.StringVar(value="Phase: idle")
        self.ssr_var = tk.StringVar(value="SSR: 0/0")
        self.warn_var = tk.StringVar(value="")

        ttk.Label(status, textvariable=self.status_var, width=24).pack(side=tk.LEFT)
        ttk.Label(status, textvariable=self.temp_var, width=16).pack(side=tk.LEFT)
        ttk.Label(status, textvariable=self.target_var, width=16).pack(side=tk.LEFT)
        ttk.Label(status, textvariable=self.phase_var, width=24).pack(side=tk.LEFT)
        ttk.Label(status, textvariable=self.ssr_var, width=14).pack(side=tk.LEFT)
        ttk.Label(status, textvariable=self.warn_var, foreground="#b30000").pack(side=tk.LEFT, padx=8)

        figure = Figure(figsize=(9, 5), dpi=100)
        self.ax = figure.add_subplot(111)
        self.ax.set_title("Temperature")
        self.ax.set_xlabel("Time (s)")
        self.ax.set_ylabel("C")
        self.ax.grid(True, alpha=0.3)
        self.meas_line, = self.ax.plot([], [], label="Measured", color="#1f77b4")
        self.tgt_line, = self.ax.plot([], [], label="Target", color="#ff7f0e", linestyle="--")
        self.ax.legend(loc="upper left")

        canvas = FigureCanvasTkAgg(figure, master=root)
        self.canvas = canvas
        canvas.get_tk_widget().pack(fill=tk.BOTH, expand=True, pady=6)

        controls = ttk.Frame(root)
        controls.pack(fill=tk.X)

        self.port_combo = ttk.Combobox(controls, state="readonly", width=10)
        self.port_combo.grid(row=0, column=0, padx=2, pady=2)
        ttk.Button(controls, text="Refresh", command=self._refresh_ports).grid(row=0, column=1, padx=2, pady=2)
        ttk.Button(controls, text="Connect", command=self._connect).grid(row=0, column=2, padx=2, pady=2)
        ttk.Button(controls, text="Disconnect", command=self._disconnect).grid(row=0, column=3, padx=2, pady=2)

        self.csv_var = tk.StringVar(value=str(DEFAULT_CSV if DEFAULT_CSV.exists() else ""))
        ttk.Entry(controls, textvariable=self.csv_var, width=28).grid(row=0, column=4, padx=2, pady=2)
        ttk.Button(controls, text="CSV", command=self._pick_csv).grid(row=0, column=5, padx=2, pady=2)
        ttk.Button(controls, text="Play", command=self._play_csv).grid(row=0, column=6, padx=2, pady=2)
        ttk.Button(controls, text="Stop", command=self._stop_csv).grid(row=0, column=7, padx=2, pady=2)

        self.preheat_var = tk.DoubleVar(value=100.0)
        ttk.Label(controls, text="Preheat C").grid(row=1, column=0, padx=2, pady=2)
        tk.Spinbox(controls, from_=30, to=240, increment=1.0, textvariable=self.preheat_var, width=6).grid(row=1, column=1, padx=2, pady=2)
        ttk.Button(controls, text="Start Preheat", command=self._start_preheat).grid(row=1, column=2, padx=2, pady=2)
        ttk.Button(controls, text="Start Profile", command=self._start_profile).grid(row=1, column=3, padx=2, pady=2)
        ttk.Button(controls, text="Stop Auto", command=self._stop_auto).grid(row=1, column=4, padx=2, pady=2)

        ttk.Button(controls, text="A", command=lambda: self._send_manual("A")).grid(row=1, column=5, padx=2, pady=2)
        ttk.Button(controls, text="T", command=lambda: self._send_manual("T")).grid(row=1, column=6, padx=2, pady=2)
        ttk.Button(controls, text="B", command=lambda: self._send_manual("B")).grid(row=1, column=7, padx=2, pady=2)
        ttk.Button(controls, text="X", command=lambda: self._send_manual("X")).grid(row=1, column=8, padx=2, pady=2)
        ttk.Button(controls, text="S", command=lambda: self._send_manual("S")).grid(row=1, column=9, padx=2, pady=2)
        ttk.Button(controls, text="M", command=lambda: self._send_manual("M")).grid(row=1, column=10, padx=2, pady=2)
        ttk.Button(controls, text="P", command=lambda: self._send_manual("P")).grid(row=1, column=11, padx=2, pady=2)
        ttk.Button(controls, text="R", command=lambda: self._send_manual("R")).grid(row=1, column=12, padx=2, pady=2)

        profile_row = ttk.Frame(root)
        profile_row.pack(fill=tk.X, pady=(6, 0))

        self.profile_path_var = tk.StringVar(value="")
        ttk.Entry(profile_row, textvariable=self.profile_path_var, width=52).pack(side=tk.LEFT, padx=2)
        ttk.Button(profile_row, text="Load", command=self._load_profile_dialog).pack(side=tk.LEFT, padx=2)
        ttk.Button(profile_row, text="Save", command=self._save_profile).pack(side=tk.LEFT, padx=2)
        ttk.Button(profile_row, text="Save As", command=self._save_profile_as).pack(side=tk.LEFT, padx=2)

        editor = ttk.Frame(root)
        editor.pack(fill=tk.X, pady=(4, 0))

        self.points_tree = ttk.Treeview(editor, columns=("stage", "time", "temp"), show="headings", height=6)
        self.points_tree.heading("stage", text="Stage")
        self.points_tree.heading("time", text="Time (s)")
        self.points_tree.heading("temp", text="Temp (C)")
        self.points_tree.column("stage", width=220)
        self.points_tree.column("time", width=100, anchor=tk.E)
        self.points_tree.column("temp", width=100, anchor=tk.E)
        self.points_tree.pack(side=tk.LEFT, fill=tk.X, expand=True)

        point_controls = ttk.Frame(editor)
        point_controls.pack(side=tk.LEFT, padx=6)

        self.stage_var = tk.StringVar(value="Stage")
        self.time_var = tk.DoubleVar(value=0.0)
        self.temp_set_var = tk.DoubleVar(value=25.0)

        ttk.Entry(point_controls, textvariable=self.stage_var, width=16).pack(pady=1)
        tk.Spinbox(point_controls, from_=0, to=2000, increment=1.0, textvariable=self.time_var, width=10).pack(pady=1)
        tk.Spinbox(point_controls, from_=0, to=300, increment=1.0, textvariable=self.temp_set_var, width=10).pack(pady=1)
        ttk.Button(point_controls, text="Add", command=self._add_point).pack(fill=tk.X, pady=1)
        ttk.Button(point_controls, text="Update", command=self._update_point).pack(fill=tk.X, pady=1)
        ttk.Button(point_controls, text="Delete", command=self._delete_point).pack(fill=tk.X, pady=1)
        self.points_tree.bind("<<TreeviewSelect>>", self._on_point_select)

    def _push_sample(self, sample: Sample) -> None:
        try:
            self.sample_queue.put_nowait(sample)
        except queue.Full:
            pass

    def _refresh_ports(self) -> None:
        ports = self.serial.available_ports()
        self.port_combo["values"] = ports
        if ports:
            self.port_combo.set(ports[0])

    def _connect(self) -> None:
        self._stop_csv()
        port = self.port_combo.get().strip()
        if not port:
            messagebox.showwarning("Serial", "Select a COM port")
            return
        self._reset_traces()
        self.serial.connect(port)
        self.status_var.set(f"Connected: {port}")

    def _disconnect(self) -> None:
        self.serial.disconnect()
        self.status_var.set("Disconnected")

    def _pick_csv(self) -> None:
        selected = filedialog.askopenfilename(
            title="Select CSV",
            filetypes=[("CSV", "*.csv"), ("All", "*.*")],
            initialdir=str(ROOT.parent / "v01_test"),
        )
        if selected:
            self.csv_var.set(selected)

    def _play_csv(self) -> None:
        path = Path(self.csv_var.get().strip())
        if not path.exists():
            messagebox.showerror("CSV", "CSV file not found")
            return
        self._disconnect()
        self._reset_traces()
        self.replay.start(path)
        self.status_var.set(f"Simulation: {path.name}")

    def _stop_csv(self) -> None:
        self.replay.stop()

    def _load_default_profile(self) -> None:
        if DEFAULT_PROFILE.exists():
            self._load_profile(DEFAULT_PROFILE)

    def _load_profile_dialog(self) -> None:
        selected = filedialog.askopenfilename(
            title="Open profile",
            filetypes=[("JSON", "*.json"), ("All", "*.*")],
            initialdir=str(ROOT.parent),
        )
        if selected:
            self._load_profile(Path(selected))

    def _load_profile(self, path: Path) -> None:
        try:
            self.profile.load(path)
        except Exception as exc:
            messagebox.showerror("Profile", str(exc))
            return
        self.profile_path_var.set(str(path))
        self._refresh_points_tree()

    def _save_profile(self) -> None:
        try:
            path_text = self.profile_path_var.get().strip()
            path = Path(path_text) if path_text else None
            saved = self.profile.save(path)
            self.profile_path_var.set(str(saved))
        except Exception as exc:
            messagebox.showerror("Profile", str(exc))

    def _save_profile_as(self) -> None:
        selected = filedialog.asksaveasfilename(
            title="Save profile as",
            defaultextension=".json",
            filetypes=[("JSON", "*.json"), ("All", "*.*")],
            initialdir=str(ROOT),
        )
        if not selected:
            return
        try:
            saved = self.profile.save(Path(selected))
            self.profile_path_var.set(str(saved))
        except Exception as exc:
            messagebox.showerror("Profile", str(exc))

    def _refresh_points_tree(self) -> None:
        for iid in self.points_tree.get_children():
            self.points_tree.delete(iid)
        for idx, p in enumerate(self.profile.points):
            self.points_tree.insert("", tk.END, iid=str(idx), values=(p.stage, f"{p.time_s:.1f}", f"{p.temp_c:.1f}"))

    def _on_point_select(self, _event=None) -> None:
        selected = self.points_tree.selection()
        if not selected:
            return
        idx = int(selected[0])
        if idx < 0 or idx >= len(self.profile.points):
            return
        p = self.profile.points[idx]
        self.stage_var.set(p.stage)
        self.time_var.set(p.time_s)
        self.temp_set_var.set(p.temp_c)

    def _add_point(self) -> None:
        self.profile.points.append(
            ProfilePoint(
                stage=self.stage_var.get().strip() or "Stage",
                time_s=float(self.time_var.get()),
                temp_c=float(self.temp_set_var.get()),
            )
        )
        self.profile.points.sort(key=lambda p: p.time_s)
        self._refresh_points_tree()

    def _update_point(self) -> None:
        selected = self.points_tree.selection()
        if not selected:
            return
        idx = int(selected[0])
        if idx < 0 or idx >= len(self.profile.points):
            return
        self.profile.points[idx] = ProfilePoint(
            stage=self.stage_var.get().strip() or "Stage",
            time_s=float(self.time_var.get()),
            temp_c=float(self.temp_set_var.get()),
        )
        self.profile.points.sort(key=lambda p: p.time_s)
        self._refresh_points_tree()

    def _delete_point(self) -> None:
        selected = self.points_tree.selection()
        if not selected:
            return
        idx = int(selected[0])
        if idx < 0 or idx >= len(self.profile.points):
            return
        del self.profile.points[idx]
        self._refresh_points_tree()

    def _start_preheat(self) -> None:
        self.engine.start_preheat(float(self.preheat_var.get()))

    def _start_profile(self) -> None:
        if len(self.profile.points) < 2:
            messagebox.showwarning("Profile", "Load or create a profile with at least 2 points")
            return
        start_ms = self.samples[-1].t_ms if self.samples else 0
        self.engine.start_profile(start_ms)

    def _stop_auto(self) -> None:
        self.engine.stop()
        self._send_manual("X")

    def _send_manual(self, cmd: str) -> None:
        self.serial.send(cmd)

    def _reset_traces(self) -> None:
        self.samples.clear()
        self.targets.clear()
        while not self.sample_queue.empty():
            try:
                self.sample_queue.get_nowait()
            except queue.Empty:
                break

    def _update_plot(self) -> None:
        if not self.samples:
            return

        base = self.samples[0].t_ms
        xs = [(s.t_ms - base) / 1000.0 for s in self.samples]
        ys = [s.temp_c for s in self.samples]
        self.meas_line.set_data(xs, ys)

        tx = []
        ty = []
        for i, t in enumerate(self.targets):
            if t is not None and i < len(xs):
                tx.append(xs[i])
                ty.append(t)
        self.tgt_line.set_data(tx, ty)

        y_all = ys + ty if ty else ys
        self.ax.set_xlim(max(0.0, xs[-1] - 300), xs[-1] + 5)
        self.ax.set_ylim(min(y_all) - 5, max(y_all) + 5)
        self.canvas.draw_idle()

    def _tick(self) -> None:
        updated = False
        while True:
            try:
                s = self.sample_queue.get_nowait()
            except queue.Empty:
                break
            self.samples.append(s)
            if len(self.samples) > 6000:
                self.samples = self.samples[-6000:]
                self.targets = self.targets[-6000:]
            updated = True

        if updated and self.samples:
            latest = self.samples[-1]
            target, phase, command = self.engine.step(latest, self.profile if self.profile.points else None)
            self.targets.append(target)

            if command is not None:
                self.serial.send(command)

            self.temp_var.set(f"Temp: {latest.temp_c:.1f} C")
            self.target_var.set(f"Target: {target:.1f} C" if target is not None else "Target: --.- C")
            self.phase_var.set(f"Phase: {phase}")
            self.ssr_var.set(f"SSR: {latest.top}/{latest.bottom}")
            self.warn_var.set(self.engine.last_warning)

            self._update_plot()

        self.root.after(120, self._tick)


def main() -> None:
    root = tk.Tk()
    app = App(root)
    try:
        root.mainloop()
    finally:
        app.serial.disconnect()
        app.replay.stop()


if __name__ == "__main__":
    main()
