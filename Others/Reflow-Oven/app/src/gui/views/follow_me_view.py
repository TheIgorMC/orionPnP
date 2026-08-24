"""Follow Me view — guided reflow with live plot, instruction card, and alert card."""

from __future__ import annotations

import time

import dearpygui.dearpygui as dpg

from ..theme import (
    C_ACCENT, C_GREEN, C_ORANGE, C_RED, C_SUBTEXT, C_TEXT,
    make_accent_button_theme, make_danger_button_theme,
    make_alert_child_theme,
)
from ..widgets.temp_plot import TempPlotWidget

_plot   = TempPlotWidget("fm", maxlen=3600)
_accent = None
_danger = None
_alert_theme   = None

_STATUS_COLORS = {
    "IDLE":     (166, 173, 200, 255),
    "RAMPING":  (137, 180, 250, 255),
    "SOAKING":  (166, 227, 161, 255),
    "COMPLETE": (166, 227, 161, 255),
    "FAULT":    (243, 139, 168, 255),
}


def build() -> None:
    global _accent, _danger, _alert_theme
    _accent      = make_accent_button_theme()
    _danger      = make_danger_button_theme()
    _alert_theme = make_alert_child_theme()

    with dpg.group(tag="view_follow_me", show=False):

        # ── Top status bar ────────────────────────────────────────────────
        with dpg.group(horizontal=True):
            dpg.add_text("Follow Me", color=C_ACCENT)
            dpg.add_spacer(width=16)
            dpg.add_text("Profile: —", tag="fm_profile_name", color=C_SUBTEXT)
            dpg.add_spacer(width=16)
            with dpg.group(horizontal=True):
                dpg.add_text("Status:", color=C_SUBTEXT)
                dpg.add_spacer(width=4)
                dpg.add_text("IDLE", tag="fm_status_badge", color=C_SUBTEXT)
            dpg.add_spacer(width=16)
            dpg.add_text("Elapsed: 0 s", tag="fm_elapsed", color=C_SUBTEXT)

        dpg.add_separator()
        dpg.add_spacer(height=6)

        # ── Main layout: plot (left) + sidebar (right) ────────────────────
        with dpg.group(horizontal=True):

            # ── Live plot ─────────────────────────────────────────────────
            with dpg.child_window(tag="fm_plot_panel", width=-320, height=460, border=False):
                _plot.build(height=-1, width=-1, label="")

            dpg.add_spacer(width=10)

            # ── Right sidebar ─────────────────────────────────────────────
            with dpg.group(tag="fm_sidebar", width=300):

                # Stage card
                with dpg.child_window(tag="fm_stage_card", height=130, border=True):
                    dpg.add_text("Current Stage", color=C_ACCENT)
                    dpg.add_separator()
                    dpg.add_spacer(height=4)
                    dpg.add_text("—",        tag="fm_stage_name",   color=C_TEXT)
                    dpg.add_text("Target: — °C", tag="fm_stage_target", color=C_SUBTEXT)
                    dpg.add_spacer(height=6)
                    dpg.add_progress_bar(
                        tag="fm_dwell_bar", default_value=0.0, width=-1
                    )
                    dpg.add_text("Dwell: 0 / 0 s", tag="fm_dwell_label", color=C_SUBTEXT)

                dpg.add_spacer(height=8)

                # Instruction card
                with dpg.child_window(tag="fm_action_card", height=120, border=True):
                    dpg.add_text("Instruction", color=C_ACCENT)
                    dpg.add_separator()
                    dpg.add_spacer(height=4)
                    dpg.add_text(
                        "Start a Follow Me session to receive\nguided instructions.",
                        tag="fm_action_text",
                        color=C_TEXT,
                        wrap=280,
                    )

                dpg.add_spacer(height=8)

                # Alert card (hidden until alert fires)
                with dpg.child_window(tag="fm_alert_card", height=70, border=True, show=False):
                    dpg.add_text("⚠ Temperature deviation!", tag="fm_alert_text",
                                 color=C_RED, wrap=280)

                dpg.add_spacer(height=8)

                # Control buttons
                with dpg.group(horizontal=True):
                    btn_start = dpg.add_button(
                        label="▶  Start", tag="fm_btn_start",
                        callback=_on_start, width=120,
                    )
                    dpg.bind_item_theme(btn_start, _accent)
                    dpg.add_spacer(width=8)
                    btn_stop = dpg.add_button(
                        label="■  Stop", tag="fm_btn_stop",
                        callback=_on_stop, width=100, enabled=False,
                    )
                    dpg.bind_item_theme(btn_stop, _danger)

                dpg.add_spacer(height=6)
                dpg.add_text("Select a profile in the Profiles view first.",
                             tag="fm_hint", color=C_SUBTEXT, wrap=280)

        dpg.add_spacer(height=8)

        # ── Run log table ─────────────────────────────────────────────────
        with dpg.child_window(height=140, border=True):
            dpg.add_text("Run Log", color=C_ACCENT)
            dpg.add_separator()
            dpg.add_listbox(
                tag="fm_run_log",
                items=[],
                num_items=5,
                width=-1,
            )


def update() -> None:
    """Called every frame to refresh Follow Me display."""
    from ...state import APP
    import time as _time

    # ── Push latest sample to plot ────────────────────────────────────────
    if APP.temp_history:
        latest = list(APP.temp_history)[-1]
        elapsed_s = (latest["t"] - list(APP.temp_history)[0]["t"]) / 1000.0
        if APP.fm_running:
            _plot.push_sample(elapsed_s, latest["tc"])

    # ── Engine update ─────────────────────────────────────────────────────
    if APP.fm_engine and APP.fm_running:
        state = APP.fm_engine.update(APP.current_tc)
        APP.fm_state = state

        # Profile name
        dpg.set_value("fm_profile_name", f"Profile: {APP.current_profile.name}")

        # Status badge
        color = _STATUS_COLORS.get(state.status, C_SUBTEXT)
        dpg.set_value("fm_status_badge", state.status)
        dpg.configure_item("fm_status_badge", color=color)

        # Elapsed
        dpg.set_value("fm_elapsed", f"Elapsed: {state.elapsed_s:.0f} s")

        # Stage card
        dpg.set_value("fm_stage_name",   state.stage.name)
        dpg.set_value("fm_stage_target", f"Target: {state.target_temp:.0f} °C    "
                                          f"Current: {state.current_temp:.1f} °C")

        dwell_total = max(state.stage.dwell_seconds, 1)
        dwell_pct   = min(state.dwell_elapsed_s / dwell_total, 1.0)
        dpg.set_value("fm_dwell_bar",   dwell_pct)
        dpg.set_value("fm_dwell_label", f"Dwell: {state.dwell_elapsed_s:.0f} / {dwell_total} s")

        # Instruction
        action = state.next_action
        arrow  = {"up": "↑ Turn DIAL UP", "down": "↓ Turn DIAL DOWN"}.get(
            action.direction, ""
        )
        instruction_text = (f"{arrow}\n{action.prompt}" if arrow else action.prompt)
        dpg.set_value("fm_action_text", instruction_text)
        urgency_color = {
            "low":    C_SUBTEXT,
            "normal": C_TEXT,
            "high":   C_ORANGE,
        }.get(action.urgency, C_TEXT)
        dpg.configure_item("fm_action_text", color=urgency_color)

        # Alert card
        if state.alert:
            dpg.configure_item("fm_alert_card", show=True)
            dpg.bind_item_theme("fm_alert_card", _alert_theme)
            dpg.set_value(
                "fm_alert_text",
                f"⚠ Deviation: {state.deviation:+.1f} °C  "
                f"(tolerance ±{APP.tolerance:.0f} °C)",
            )
        else:
            dpg.configure_item("fm_alert_card", show=False)

        # Log append (only on state changes or every 10 s)
        if APP.fm_log:
            pass  # log display updated in _on_start/_on_stop via fm_log deque

        # Profile overlay — generate target curve from elapsed 0..elapsed+60
        if APP.current_profile:
            total_t = state.elapsed_s + 60.0
            ts = [i * 2.0 for i in range(int(total_t / 2) + 2)]
            ttemps = [APP.current_profile.profile_temp_at(t) for t in ts]
            _plot.set_profile_curve(ts, ttemps)
            _plot.set_tolerance(APP.tolerance)

        # Complete state
        if state.status == "COMPLETE" and APP.fm_running:
            APP.fm_running = False
            dpg.configure_item("fm_btn_start", enabled=True)
            dpg.configure_item("fm_btn_stop",  enabled=False)
            _append_log("Run complete.")


# ── Callbacks ─────────────────────────────────────────────────────────────

def _on_start(sender, app_data) -> None:
    from ...state import APP
    from ...follow_me import FollowMeEngine

    if APP.current_profile is None:
        dpg.set_value("fm_hint", "⚠ No profile selected. Go to Profiles first.")
        dpg.configure_item("fm_hint", color=C_RED)
        return

    APP.fm_engine = FollowMeEngine(
        profile=APP.current_profile,
        calibration=APP.loaded_calibration,
        tolerance=APP.tolerance,
    )
    APP.fm_engine.start()
    APP.fm_running = True
    _plot.clear()

    if APP.logger:
        APP.logger.log_event(0, "follow_me_start", stage_name="")

    dpg.configure_item("fm_btn_start", enabled=False)
    dpg.configure_item("fm_btn_stop",  enabled=True)
    dpg.set_value("fm_hint", "")
    _append_log(f"Started: {APP.current_profile.name}")


def _on_stop(sender, app_data) -> None:
    from ...state import APP
    if APP.fm_engine:
        APP.fm_engine.stop()
    APP.fm_running = False
    dpg.configure_item("fm_btn_start", enabled=True)
    dpg.configure_item("fm_btn_stop",  enabled=False)
    _append_log("Stopped by user.")


def _append_log(msg: str) -> None:
    from ...state import APP
    import time as _t
    entry = f"[{_t.strftime('%H:%M:%S')}] {msg}"
    APP.fm_log.appendleft(entry)
    dpg.configure_item("fm_run_log", items=list(APP.fm_log))
