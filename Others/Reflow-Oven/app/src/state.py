"""Orion Reflow Oven — shared application state.

All modules import APP from here. AppState is a simple mutable dataclass;
the render loop in gui/app.py drains the serial queue and updates APP each frame.
"""

from __future__ import annotations

import time
from collections import deque
from dataclasses import dataclass, field
from typing import TYPE_CHECKING, Optional

if TYPE_CHECKING:
    from .serial_comm import SerialManager
    from .logger import SessionLogger
    from .calibration import CalibrationSession
    from .profile import Profile
    from .follow_me import FollowMeEngine, FollowMeState


@dataclass
class AppState:
    # ── Serial / connection ──────────────────────────────────────────────
    serial: Optional[SerialManager] = None
    connected: bool = False
    port: str = ""

    # ── Live temperature data ────────────────────────────────────────────
    # Each entry: {"t": uptime_ms, "tc": float, "ref": float, "fault": int}
    temp_history: deque = field(default_factory=lambda: deque(maxlen=600))
    current_tc: float = 0.0
    current_ref: float = 0.0
    current_fault: int = 0
    last_fault_info: dict = field(default_factory=dict)
    session_start: float = field(default_factory=time.monotonic)

    # ── Logging ──────────────────────────────────────────────────────────
    logger: Optional[SessionLogger] = None

    # ── Calibration ──────────────────────────────────────────────────────
    cal_session: Optional[CalibrationSession] = None
    cal_recording: bool = False
    loaded_calibration: Optional[CalibrationSession] = None

    # ── Follow Me ────────────────────────────────────────────────────────
    current_profile: Optional[Profile] = None
    fm_engine: Optional[FollowMeEngine] = None
    fm_running: bool = False
    fm_state: Optional[FollowMeState] = None
    fm_log: deque = field(default_factory=lambda: deque(maxlen=100))

    # ── UI ───────────────────────────────────────────────────────────────
    active_view: str = "dashboard"
    tolerance: float = 5.0  # °C deviation threshold for Follow Me alerts


APP = AppState()
