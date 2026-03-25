"""Reflow profile data model and JSON persistence.

Profiles are stored as JSON files under profiles/.
Example: profiles/sn42bi58.json

Profile format:
{
  "name": "Sn42Bi58 Low-Temp",
  "paste_type": "Sn42Bi58",
  "stages": [
    {"name": "Preheat", "target_temp": 90, "max_ramp_rate": 1.0, "dwell_seconds": 60},
    ...
  ]
}
"""

from __future__ import annotations

import json
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import List


@dataclass
class Stage:
    name: str
    target_temp: float   # °C
    max_ramp_rate: float # °C/s — expected ramp toward this stage's target
    dwell_seconds: int   # time to hold at target_temp before advancing


@dataclass
class Profile:
    name: str
    paste_type: str
    stages: List[Stage] = field(default_factory=list)

    def total_dwell_s(self) -> int:
        """Sum of all dwell times (lower-bound estimate of run duration)."""
        return sum(s.dwell_seconds for s in self.stages)

    def profile_temp_at(self, elapsed_s: float) -> float:
        """Rough piecewise linear target temperature at a given elapsed time.
        Used to render the profile overlay on the live plot."""
        t = 0.0
        prev_temp = 25.0
        for stage in self.stages:
            if stage.max_ramp_rate == 0:
                ramp_time = 0.0
            else:
                ramp_time = abs(stage.target_temp - prev_temp) / abs(stage.max_ramp_rate)
            dwell_end = t + ramp_time + stage.dwell_seconds
            if elapsed_s <= t + ramp_time:
                frac = (elapsed_s - t) / ramp_time if ramp_time > 0 else 1.0
                return prev_temp + frac * (stage.target_temp - prev_temp)
            if elapsed_s <= dwell_end:
                return stage.target_temp
            t = dwell_end
            prev_temp = stage.target_temp
        return prev_temp


def load_profile(path: str | Path) -> Profile:
    with open(path, "r", encoding="utf-8") as f:
        data = json.load(f)
    stages = [Stage(**s) for s in data["stages"]]
    return Profile(name=data["name"], paste_type=data["paste_type"], stages=stages)


def save_profile(profile: Profile, path: str | Path) -> None:
    data = {
        "name":       profile.name,
        "paste_type": profile.paste_type,
        "stages":     [asdict(s) for s in profile.stages],
    }
    with open(path, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2)


def list_profiles(profiles_dir: str | Path) -> list[Path]:
    return sorted(Path(profiles_dir).glob("*.json"))
