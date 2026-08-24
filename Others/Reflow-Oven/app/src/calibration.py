"""Calibration session: record and post-process oven thermal response curves.

Workflow:
    session = CalibrationSession("my_oven")
    session.start_run(target=90.0)
    for each sample:
        session.add_sample(temp)
    run = session.finish_run()   # returns SetpointRun with ramp_rate computed
    session.save("calibrations/my_oven.json")

Later:
    session = CalibrationSession.load("calibrations/my_oven.json")
    rate = session.get_ramp_rate(target_temp=130.0)
"""

from __future__ import annotations

import json
import time
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import List, Optional


@dataclass
class CalibrationPoint:
    t: float    # seconds since run start
    temp: float # °C


@dataclass
class SetpointRun:
    target: float
    ramp_rate: float = 0.0         # °C/s — linear fit over ramp phase
    overshoot_delta: float = 0.0   # °C above target at peak
    curve: List[CalibrationPoint] = field(default_factory=list)


class CalibrationSession:
    def __init__(self, name: str = "calibration"):
        self.name = name
        self.runs: List[SetpointRun] = []
        self._current: Optional[SetpointRun] = None
        self._run_start: float = 0.0

    # ── Recording ────────────────────────────────────────────────────────

    def start_run(self, target: float) -> None:
        """Begin recording a new setpoint run."""
        self._current = SetpointRun(target=target)
        self._run_start = time.monotonic()

    def add_sample(self, temp: float) -> None:
        """Append a temperature reading to the active run."""
        if self._current is None:
            return
        elapsed = time.monotonic() - self._run_start
        self._current.curve.append(
            CalibrationPoint(t=round(elapsed, 2), temp=round(temp, 2))
        )

    def finish_run(self) -> SetpointRun:
        """Stop recording, post-process, and store the completed run."""
        if self._current is None:
            raise RuntimeError("No active calibration run to finish")
        run = self._current
        self._current = None
        run = self._post_process(run)
        self.runs.append(run)
        return run

    def abort_run(self) -> None:
        """Discard the current run without storing it."""
        self._current = None

    # ── Post-processing ──────────────────────────────────────────────────

    def _post_process(self, run: SetpointRun) -> SetpointRun:
        """Compute ramp rate (°C/s) and overshoot from the recorded curve."""
        pts = run.curve
        if len(pts) < 4:
            return run

        temps = [p.temp for p in pts]
        times = [p.t    for p in pts]

        # Find the ramp phase: from the first point until temp first reaches target
        ramp_end = len(pts) - 1
        for i in range(1, len(pts)):
            if temps[i] >= run.target:
                ramp_end = i
                break

        dt = times[ramp_end] - times[0]
        dT = temps[ramp_end] - temps[0]
        if dt > 0 and dT > 0:
            run.ramp_rate = round(dT / dt, 4)

        peak = max(temps)
        if peak > run.target:
            run.overshoot_delta = round(peak - run.target, 2)

        return run

    # ── Persistence ──────────────────────────────────────────────────────

    def save(self, path: str | Path) -> None:
        data = {
            "name": self.name,
            "runs": [
                {
                    "target":           r.target,
                    "ramp_rate":        r.ramp_rate,
                    "overshoot_delta":  r.overshoot_delta,
                    "curve":            [asdict(p) for p in r.curve],
                }
                for r in self.runs
            ],
        }
        Path(path).parent.mkdir(parents=True, exist_ok=True)
        with open(path, "w", encoding="utf-8") as f:
            json.dump(data, f, indent=2)

    @classmethod
    def load(cls, path: str | Path) -> CalibrationSession:
        with open(path, encoding="utf-8") as f:
            data = json.load(f)
        session = cls(name=data.get("name", "calibration"))
        for r in data.get("runs", []):
            run = SetpointRun(
                target=r["target"],
                ramp_rate=r.get("ramp_rate", 0.0),
                overshoot_delta=r.get("overshoot_delta", 0.0),
                curve=[CalibrationPoint(**p) for p in r.get("curve", [])],
            )
            session.runs.append(run)
        return session

    # ── Lookup ───────────────────────────────────────────────────────────

    def get_ramp_rate(self, target_temp: float) -> float:
        """Return the ramp rate for the nearest calibrated setpoint."""
        if not self.runs:
            return 1.0  # safe fallback
        nearest = min(self.runs, key=lambda r: abs(r.target - target_temp))
        return nearest.ramp_rate if nearest.ramp_rate > 0.01 else 1.0
