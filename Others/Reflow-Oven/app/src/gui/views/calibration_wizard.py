"""Calibration Wizard — guided 5-step procedure for characterising oven thermal response.

Steps:
  0  Intro        — explain the procedure
  1  Record       — user enters target, clicks Start; live plot shows recording
  2  Review       — show extracted ramp rate and overshoot for the completed run
  3  Next?        — offer to add another setpoint or finish
  4  Summary      — table of all runs, name and save the calibration file
"""

from __future__ import annotations

import time
from pathlib import Path

import dearpygui.dearpygui as dpg

from ..theme import (
    C_ACCENT, C_GREEN, C_ORANGE, C_RED, C_SUBTEXT, C_TEXT,
    make_accent_button_theme, make_danger_button_theme,
)
from ..widgets.temp_plot import TempPlotWidget

_plot    = TempPlotWidget("cal", maxlen=3000)
_accent  = None  # theme tags resolved after dpg context exists
_danger  = None

# ── Wizard state ──────────────────────────────────────────────────────────
_wiz = {
    "step":       0,
    "target":     90.0,
    "recording":  False,
    "last_run":   None,    # SetpointRun after finish
}


def build() -> None:
    """Build the Calibration Wizard view group. Called once at startup."""
    global _accent, _danger
    _accent = make_accent_button_theme()
    _danger = make_danger_button_theme()

    with dpg.group(tag="view_calibration", show=False):
        dpg.add_text("Calibration Wizard", color=C_ACCENT)
        dpg.add_separator()
        dpg.add_spacer(height=6)

        # ── Step indicator bar ────────────────────────────────────────────
        with dpg.group(horizontal=True, tag="cal_step_bar"):
            for i, label in enumerate(["Intro", "Record", "Review", "Add more?", "Save"]):
                dpg.add_text(f"  {i+1}. {label}  ", tag=f"cal_step_{i}", color=C_SUBTEXT)

        dpg.add_spacer(height=10)

        # ── Step 0: Intro ─────────────────────────────────────────────────
        with dpg.group(tag="cal_s0"):
            with dpg.child_window(height=220, border=True):
                dpg.add_text("What this wizard does", color=C_ACCENT)
                dpg.add_separator()
                dpg.add_spacer(height=6)
                dpg.add_text(
                    "This wizard characterises the De Longhi oven's thermal response by\n"
                    "recording temperature over time at several power setpoints.\n\n"
                    "Each run:\n"
                    "  1. Start with the oven cold.\n"
                    "  2. Set the oven dial to the target — click Start Recording.\n"
                    "  3. Wait until the oven reaches steady state or overshoots.\n"
                    "  4. Click Stop — the ramp rate and overshoot are extracted.\n"
                    "  5. Let the oven cool, then add the next setpoint or save.",
                    wrap=700,
                )
                dpg.add_spacer(height=10)
                dpg.add_text("Make sure the Pico is connected before starting.", color=C_ORANGE)
            dpg.add_spacer(height=10)
            btn = dpg.add_button(label="Begin  →", callback=_s0_next, width=140)
            dpg.bind_item_theme(btn, _accent)

        # ── Step 1: Record ────────────────────────────────────────────────
        with dpg.group(tag="cal_s1", show=False):
            with dpg.group(horizontal=True):
                with dpg.group():
                    with dpg.child_window(width=300, height=130, border=True):
                        dpg.add_text("Set target temperature", color=C_ACCENT)
                        dpg.add_separator()
                        dpg.add_spacer(height=6)
                        dpg.add_input_float(
                            tag="cal_input_target",
                            label="Target (°C)",
                            default_value=90.0,
                            min_value=50.0, max_value=250.0,
                            step=5.0,
                            width=160,
                        )
                        dpg.add_spacer(height=6)
                        dpg.add_text("Current: ---.-- °C", tag="cal_live_temp", color=C_TEXT)
                    dpg.add_spacer(height=8)
                    with dpg.group(horizontal=True):
                        btn_start = dpg.add_button(label="▶  Start Recording", tag="cal_btn_start",
                                                   callback=_on_start_rec, width=165)
                        dpg.bind_item_theme(btn_start, _accent)
                        dpg.add_spacer(width=8)
                        btn_stop = dpg.add_button(label="■  Stop", tag="cal_btn_stop",
                                                  callback=_on_stop_rec, width=80, enabled=False)
                        dpg.bind_item_theme(btn_stop, _danger)
                    dpg.add_spacer(height=6)
                    dpg.add_text("", tag="cal_rec_status", color=C_SUBTEXT)

                dpg.add_spacer(width=12)
                with dpg.child_window(border=True, height=180):
                    _plot.build(height=-1, label="")

        # ── Step 2: Review ────────────────────────────────────────────────
        with dpg.group(tag="cal_s2", show=False):
            with dpg.child_window(height=200, border=True):
                dpg.add_text("Run results", color=C_ACCENT)
                dpg.add_separator()
                dpg.add_spacer(height=8)
                dpg.add_text("Target temp:   -- °C",    tag="cal_r_target",    color=C_TEXT)
                dpg.add_text("Ramp rate:     -- °C/s",  tag="cal_r_ramp",      color=C_TEXT)
                dpg.add_text("Overshoot:     -- °C",    tag="cal_r_overshoot", color=C_TEXT)
                dpg.add_text("Samples:       --",       tag="cal_r_samples",   color=C_SUBTEXT)
            dpg.add_spacer(height=10)
            btn_next = dpg.add_button(label="Next  →", callback=_s2_next, width=120)
            dpg.bind_item_theme(btn_next, _accent)

        # ── Step 3: Add more? ─────────────────────────────────────────────
        with dpg.group(tag="cal_s3", show=False):
            with dpg.child_window(height=120, border=True):
                dpg.add_text("Add another setpoint run?", color=C_ACCENT)
                dpg.add_separator()
                dpg.add_spacer(height=8)
                dpg.add_text(
                    "Add more runs across the target range for better prediction accuracy.\n"
                    "Recommended: 90, 130, 165 °C for low-temp paste profiles.",
                    wrap=700,
                )
            dpg.add_spacer(height=10)
            with dpg.group(horizontal=True):
                btn_add = dpg.add_button(label="+ Add another run", callback=_s3_add_more, width=180)
                dpg.bind_item_theme(btn_add, _accent)
                dpg.add_spacer(width=12)
                btn_fin = dpg.add_button(label="Finish  →", callback=_s3_finish, width=120)
                dpg.bind_item_theme(btn_fin, _accent)

        # ── Step 4: Save ──────────────────────────────────────────────────
        with dpg.group(tag="cal_s4", show=False):
            with dpg.child_window(height=200, border=True):
                dpg.add_text("Calibration summary", color=C_ACCENT)
                dpg.add_separator()
                dpg.add_spacer(height=6)
                with dpg.table(
                    tag="cal_summary_table",
                    header_row=True,
                    borders_innerV=True,
                    borders_outerH=True,
                    policy=dpg.mvTable_SizingFixedFit,
                ):
                    dpg.add_table_column(label="Target (°C)",  width_fixed=True, init_width_or_weight=130)
                    dpg.add_table_column(label="Ramp (°C/s)",  width_fixed=True, init_width_or_weight=130)
                    dpg.add_table_column(label="Overshoot (°C)", width_fixed=True, init_width_or_weight=140)
                    dpg.add_table_column(label="Samples",      width_fixed=True, init_width_or_weight=90)
            dpg.add_spacer(height=8)
            with dpg.group(horizontal=True):
                dpg.add_text("Name:", color=C_SUBTEXT)
                dpg.add_spacer(width=6)
                dpg.add_input_text(tag="cal_name_input", default_value="my_oven", width=200)
            dpg.add_spacer(height=8)
            with dpg.group(horizontal=True):
                btn_save = dpg.add_button(label="Save calibration", callback=_on_save_cal, width=180)
                dpg.bind_item_theme(btn_save, _accent)
                dpg.add_spacer(width=12)
                dpg.add_text("", tag="cal_save_status", color=C_GREEN)


def update() -> None:
    """Called every frame."""
    from ...state import APP

    if APP.current_tc:
        dpg.set_value("cal_live_temp", f"Current: {APP.current_tc:.2f} °C")

    if _wiz["recording"] and APP.cal_session:
        APP.cal_session.add_sample(APP.current_tc)
        elapsed = time.monotonic()
        if APP.temp_history:
            latest = list(APP.temp_history)[-1]
            _plot.push_sample(latest["t"] / 1000.0, latest["tc"])


# ── Step navigation ────────────────────────────────────────────────────────

def _goto_step(n: int) -> None:
    _wiz["step"] = n
    for i in range(5):
        dpg.configure_item(f"cal_s{i}", show=(i == n))
        dpg.configure_item(
            f"cal_step_{i}",
            color=C_ACCENT if i == n else C_SUBTEXT,
        )


def _s0_next(sender, app_data) -> None:
    _goto_step(1)


def _s2_next(sender, app_data) -> None:
    _goto_step(3)


def _s3_add_more(sender, app_data) -> None:
    _wiz["recording"] = False
    _plot.clear()
    _goto_step(1)


def _s3_finish(sender, app_data) -> None:
    _populate_summary_table()
    _goto_step(4)


# ── Recording callbacks ────────────────────────────────────────────────────

def _on_start_rec(sender, app_data) -> None:
    from ...state import APP
    from ...calibration import CalibrationSession

    target = dpg.get_value("cal_input_target")
    _wiz["target"] = target

    if APP.cal_session is None:
        APP.cal_session = CalibrationSession("oven_cal")
    APP.cal_session.start_run(target)
    APP.cal_recording = True
    _wiz["recording"] = True

    _plot.clear()
    dpg.configure_item("cal_btn_start", enabled=False)
    dpg.configure_item("cal_btn_stop",  enabled=True)
    dpg.set_value("cal_rec_status", f"Recording at target {target:.0f} °C…")
    dpg.configure_item("cal_rec_status", color=C_ORANGE)


def _on_stop_rec(sender, app_data) -> None:
    from ...state import APP

    if APP.cal_session is None or not APP.cal_recording:
        return

    run = APP.cal_session.finish_run()
    APP.cal_recording = False
    _wiz["recording"]  = False
    _wiz["last_run"]   = run

    dpg.configure_item("cal_btn_start", enabled=True)
    dpg.configure_item("cal_btn_stop",  enabled=False)
    dpg.set_value("cal_rec_status", "Recording stopped.")
    dpg.configure_item("cal_rec_status", color=C_GREEN)

    # Populate review panel
    dpg.set_value("cal_r_target",    f"Target temp:   {run.target:.0f} °C")
    dpg.set_value("cal_r_ramp",      f"Ramp rate:     {run.ramp_rate:.3f} °C/s")
    dpg.set_value("cal_r_overshoot", f"Overshoot:     {run.overshoot_delta:.2f} °C")
    dpg.set_value("cal_r_samples",   f"Samples:       {len(run.curve)}")
    _goto_step(2)


def _populate_summary_table() -> None:
    from ...state import APP
    # Clear existing rows
    for child in dpg.get_item_children("cal_summary_table", 1):
        dpg.delete_item(child)
    if APP.cal_session:
        for run in APP.cal_session.runs:
            with dpg.table_row(parent="cal_summary_table"):
                dpg.add_text(f"{run.target:.0f}")
                dpg.add_text(f"{run.ramp_rate:.3f}")
                dpg.add_text(f"{run.overshoot_delta:.2f}")
                dpg.add_text(str(len(run.curve)))


def _on_save_cal(sender, app_data) -> None:
    from ...state import APP
    from pathlib import Path

    if APP.cal_session is None:
        dpg.set_value("cal_save_status", "No data to save.")
        return

    name = dpg.get_value("cal_name_input").strip() or "my_oven"
    path = Path("calibrations") / f"{name}.json"
    try:
        APP.cal_session.save(path)
        APP.loaded_calibration = APP.cal_session
        dpg.set_value("cal_save_status", f"Saved: {path}")
    except Exception as exc:
        dpg.set_value("cal_save_status", f"Error: {exc}")
        dpg.configure_item("cal_save_status", color=C_RED)
