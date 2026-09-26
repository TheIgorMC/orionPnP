"""Follow Me engine — guides the user through a reflow profile in real time.

The engine tracks the current stage, computes the deviation between measured
and target temperature, and generates an instruction prompt every frame.

Usage:
    engine = FollowMeEngine(profile, calibration, tolerance=5.0)
    engine.start()
    while running:
        state = engine.update(current_tc_temp)
        # use state.next_action.prompt, state.alert, state.status
    engine.stop()
"""

from __future__ import annotations

import time
from dataclasses import dataclass
from typing import Optional

from .calibration import CalibrationSession
from .profile import Profile, Stage


@dataclass
class NextAction:
    prompt: str
    direction: Optional[str] = None  # "up" | "down" | None
    urgency: str = "normal"          # "low" | "normal" | "high"


@dataclass
class FollowMeState:
    stage_index: int
    stage: Stage
    elapsed_s: float
    dwell_elapsed_s: float
    current_temp: float
    target_temp: float
    deviation: float         # current_temp − target (+ = above, − = below)
    next_action: NextAction
    alert: bool
    status: str              # "IDLE" | "RAMPING" | "SOAKING" | "COMPLETE"


class FollowMeEngine:
    DEFAULT_TOLERANCE = 5.0  # °C

    def __init__(
        self,
        profile: Profile,
        calibration: Optional[CalibrationSession] = None,
        tolerance: float = DEFAULT_TOLERANCE,
    ):
        self.profile     = profile
        self.calibration = calibration
        self.tolerance   = tolerance

        self._stage_idx:   int            = 0
        self._run_start:   float          = 0.0
        self._stage_start: float          = 0.0
        self._dwell_start: Optional[float] = None
        self._running:     bool           = False
        self._complete:    bool           = False

    # ── Control ──────────────────────────────────────────────────────────

    def start(self) -> None:
        now = time.monotonic()
        self._run_start   = now
        self._stage_start = now
        self._stage_idx   = 0
        self._dwell_start = None
        self._running     = True
        self._complete    = False

    def stop(self) -> None:
        self._running = False

    @property
    def is_running(self) -> bool:
        return self._running

    # ── Per-frame update ─────────────────────────────────────────────────

    def update(self, current_temp: float) -> FollowMeState:
        now            = time.monotonic()
        elapsed_s      = now - self._run_start
        stage_elapsed  = now - self._stage_start
        stage          = self.profile.stages[self._stage_idx]

        if self._complete:
            return self._state(
                current_temp, elapsed_s, 0.0,
                "COMPLETE",
                NextAction("Reflow complete. Open door — allow natural cooling.", urgency="high"),
                alert=False,
            )

        # Detect when the oven has reached the stage target
        reached = current_temp >= (stage.target_temp - self.tolerance)
        if reached and self._dwell_start is None:
            self._dwell_start = now

        dwell_elapsed = (now - self._dwell_start) if self._dwell_start is not None else 0.0

        # Advance to next stage when dwell time expires
        if self._dwell_start is not None and dwell_elapsed >= stage.dwell_seconds:
            next_idx = self._stage_idx + 1
            if next_idx < len(self.profile.stages):
                self._stage_idx   = next_idx
                self._stage_start = now
                self._dwell_start = None
                stage             = self.profile.stages[self._stage_idx]
                dwell_elapsed     = 0.0
            else:
                self._complete = True
                return self._state(
                    current_temp, elapsed_s, dwell_elapsed,
                    "COMPLETE",
                    NextAction("Reflow complete. Open door — allow natural cooling.", urgency="high"),
                    alert=False,
                )

        action = self._compute_action(current_temp, stage, stage_elapsed, dwell_elapsed)
        alert  = (
            abs(current_temp - stage.target_temp) > self.tolerance
            and self._dwell_start is not None  # only alert during dwell, not ramp
        )
        status = "SOAKING" if self._dwell_start is not None else "RAMPING"

        return self._state(current_temp, elapsed_s, dwell_elapsed, status, action, alert)

    # ── Helpers ──────────────────────────────────────────────────────────

    def _compute_action(
        self,
        current_temp: float,
        stage: Stage,
        stage_elapsed: float,
        dwell_elapsed: float,
    ) -> NextAction:
        # ① Already soaking — show countdown
        if self._dwell_start is not None:
            remaining = stage.dwell_seconds - dwell_elapsed
            if remaining > 10:
                return NextAction(
                    f"Maintain {stage.target_temp:.0f} °C — {remaining:.0f} s remaining",
                    direction=None,
                    urgency="low",
                )
            # Near end of dwell — preview next stage
            next_idx = self._stage_idx + 1
            if next_idx < len(self.profile.stages):
                nxt = self.profile.stages[next_idx]
                direction = "up" if nxt.target_temp > stage.target_temp else "down"
                return NextAction(
                    f"Almost done — next: '{nxt.name}' ({nxt.target_temp:.0f} °C). "
                    f"Prepare to turn dial {direction}.",
                    direction=direction,
                    urgency="normal",
                )
            return NextAction("Dwell complete — begin cooling", urgency="normal")

        # ② Ramping toward target — estimate time remaining
        ramp_rate = stage.max_ramp_rate
        if self.calibration:
            ramp_rate = max(self.calibration.get_ramp_rate(stage.target_temp), 0.1)

        delta = stage.target_temp - current_temp
        if delta <= 0:
            return NextAction(
                f"Target {stage.target_temp:.0f} °C reached — hold position.", urgency="low"
            )

        eta_s = delta / ramp_rate
        if eta_s < 30:
            return NextAction(
                f"Approaching {stage.target_temp:.0f} °C (~{eta_s:.0f} s) — "
                f"prepare to reduce dial.",
                direction="down",
                urgency="high",
            )

        return NextAction(
            f"Ramping to {stage.target_temp:.0f} °C — {delta:.1f} °C to go "
            f"(~{eta_s:.0f} s at {ramp_rate:.2f} °C/s)",
            direction="up",
            urgency="normal",
        )

    def _state(
        self,
        current_temp: float,
        elapsed_s: float,
        dwell_elapsed: float,
        status: str,
        action: NextAction,
        alert: bool,
    ) -> FollowMeState:
        stage = self.profile.stages[self._stage_idx]
        return FollowMeState(
            stage_index=self._stage_idx,
            stage=stage,
            elapsed_s=elapsed_s,
            dwell_elapsed_s=dwell_elapsed,
            current_temp=current_temp,
            target_temp=stage.target_temp,
            deviation=current_temp - stage.target_temp,
            next_action=action,
            alert=alert,
            status=status,
        )
