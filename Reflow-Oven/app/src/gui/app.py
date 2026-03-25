"""Main Dear PyGui application — layout, navigation, and render loop.

Entry point: call run() from app/main.py.

Layout:
    ┌─────────────────────────────────────────────────┐
    │ sidebar (180 px) │ content area (fill)           │
    │  Dashboard       │  [active view]                │
    │  Calibration     │                               │
    │  Follow Me       │                               │
    │  Profiles        │                               │
    │  Logs            │                               │
    └─────────────────────────────────────────────────┘
"""

from __future__ import annotations

import queue
import time
from pathlib import Path

import dearpygui.dearpygui as dpg

from ..state import APP
from ..serial_comm import SerialManager
from ..logger import SessionLogger
from .theme import apply_global_theme, C_ACCENT, C_SUBTEXT, C_TEXT

# View modules
from .views import dashboard          as _dash
from .views import calibration_wizard as _cal
from .views import follow_me_view     as _fm
from .views import profile_manager    as _prof
from .views import logs_view          as _logs

_VIEWS = {
    "dashboard":   _dash,
    "calibration": _cal,
    "follow_me":   _fm,
    "profiles":    _prof,
    "logs":        _logs,
}

_NAV_LABELS = [
    ("dashboard",   "Dashboard"),
    ("calibration", "Calibration"),
    ("follow_me",   "Follow Me"),
    ("profiles",    "Profiles"),
    ("logs",        "Logs"),
]

_VIEWPORT_W = 1280
_VIEWPORT_H = 800


def switch_view(name: str) -> None:
    """Show the named view and hide all others. Updates sidebar highlight."""
    if name not in _VIEWS:
        return
    for key in _VIEWS:
        dpg.configure_item(f"view_{key}", show=(key == name))
    APP.active_view = name
    # Update sidebar button labels to show the active item
    for key, label in _NAV_LABELS:
        if key == name:
            dpg.configure_item(f"nav_btn_{key}", label=f"▸  {label}")
        else:
            dpg.configure_item(f"nav_btn_{key}", label=f"   {label}")


def run() -> None:
    """Build the UI, start the render loop, and clean up on exit."""
    _setup_app_state()

    dpg.create_context()
    apply_global_theme()

    _build_ui()

    dpg.create_viewport(
        title="Orion Reflow Oven",
        width=_VIEWPORT_W,
        height=_VIEWPORT_H,
        min_width=1024,
        min_height=640,
    )
    dpg.setup_dearpygui()
    dpg.show_viewport()
    dpg.set_primary_window("##main", True)

    # Trigger initial data load for profile manager
    _prof._refresh_list()
    _logs._refresh_list()

    # Render loop
    while dpg.is_dearpygui_running():
        _process_serial()
        _update_views()
        dpg.render_dearpygui_frame()

    # Cleanup
    if APP.serial:
        APP.serial.disconnect()
    if APP.logger:
        APP.logger.close()
    dpg.destroy_context()


# ── Setup ──────────────────────────────────────────────────────────────────

def _setup_app_state() -> None:
    APP.serial = SerialManager(
        on_connect=_on_connect,
        on_disconnect=_on_disconnect,
    )
    APP.logger = SessionLogger(logs_dir=Path("logs"))
    APP.session_start = time.monotonic()


def _build_ui() -> None:
    with dpg.window(tag="##main", no_title_bar=True, no_scrollbar=True,
                    no_move=True, no_resize=True, no_close=True):
        with dpg.group(horizontal=True):
            _build_sidebar()
            dpg.add_spacer(width=1)

            # Content area — all views live here, only one shown at a time
            with dpg.child_window(tag="##content", border=False, no_scrollbar=False):
                _dash.build()
                _cal.build()
                _fm.build()
                _prof.build()
                _logs.build()

    # Ensure only the dashboard is shown at startup
    switch_view("dashboard")


def _build_sidebar() -> None:
    with dpg.child_window(tag="##sidebar", width=175, border=True, no_scrollbar=True):
        dpg.add_spacer(height=8)

        # App title
        dpg.add_text("Orion", color=C_ACCENT)
        dpg.add_text("Reflow Oven", color=C_SUBTEXT)
        dpg.add_spacer(height=6)
        dpg.add_separator()
        dpg.add_spacer(height=8)

        # Nav buttons
        for key, label in _NAV_LABELS:
            dpg.add_button(
                tag=f"nav_btn_{key}",
                label=f"   {label}",
                callback=lambda s, a, u=key: switch_view(u),
                width=-1,
                height=34,
            )
            dpg.add_spacer(height=2)

        dpg.add_spacer(height=10)
        dpg.add_separator()
        dpg.add_spacer(height=8)

        # Live mini status
        dpg.add_text("Connection", color=C_SUBTEXT)
        dpg.add_text("●  --", tag="sidebar_conn", color=C_SUBTEXT)
        dpg.add_spacer(height=6)
        dpg.add_text("Temperature", color=C_SUBTEXT)
        dpg.add_text("---.-- °C", tag="sidebar_temp", color=C_TEXT)

        dpg.add_spacer(height=-1)  # fill remaining space

        # Version footer
        dpg.add_separator()
        dpg.add_spacer(height=4)
        dpg.add_text("Phase 1 — v0.1", color=C_SUBTEXT)


# ── Per-frame processing ───────────────────────────────────────────────────

def _process_serial() -> None:
    """Drain the serial sample queue into APP state."""
    if APP.serial is None:
        return
    APP.connected = APP.serial.connected

    # Drain up to 20 samples per frame
    for _ in range(20):
        try:
            sample = APP.serial.samples.get_nowait()
        except queue.Empty:
            break

        APP.temp_history.append(sample)
        APP.current_tc    = sample.get("tc",    APP.current_tc)
        APP.current_ref   = sample.get("ref",   APP.current_ref)
        APP.current_fault = sample.get("fault", 0)

        if APP.logger:
            stage = APP.fm_state.stage.name if APP.fm_state else ""
            APP.logger.log_sample(
                uptime_ms=sample.get("t", 0),
                tc_temp=APP.current_tc,
                ref_temp=APP.current_ref,
                stage_name=stage,
            )

    # Drain events
    for _ in range(10):
        try:
            evt = APP.serial.events.get_nowait()
            if evt.get("event") == "fault":
                APP.last_fault_info = evt
        except queue.Empty:
            break


def _update_views() -> None:
    """Call each view's update() and update the sidebar status."""
    view_mod = _VIEWS.get(APP.active_view)
    if view_mod:
        view_mod.update()

    # Sidebar live values (always updated)
    if APP.connected:
        dpg.set_value("sidebar_conn", f"●  {APP.port}")
        dpg.configure_item("sidebar_conn", color=(166, 227, 161, 255))  # green
    else:
        dpg.set_value("sidebar_conn", "●  Disconnected")
        dpg.configure_item("sidebar_conn", color=(243, 139, 168, 255))  # red

    if APP.current_tc:
        dpg.set_value("sidebar_temp", f"{APP.current_tc:.2f} °C")


# ── Serial callbacks (called from background thread) ──────────────────────

def _on_connect(port: str) -> None:
    APP.connected = True
    APP.port = port


def _on_disconnect() -> None:
    APP.connected = False
    APP.port = ""
