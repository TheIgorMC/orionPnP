"""Dashboard view — connection panel, live temperature readout, 60 s mini-plot."""

from __future__ import annotations

import dearpygui.dearpygui as dpg

from ..theme import (
    C_ACCENT, C_GREEN, C_RED, C_SUBTEXT, C_TEXT, C_YELLOW,
    make_accent_button_theme, make_danger_button_theme,
)
from ..widgets.temp_plot import TempPlotWidget

_plot = TempPlotWidget("dash", maxlen=120)  # 60 s @ 0.5 s/sample


def build() -> None:
    """Build the Dashboard view group. Called once during UI construction."""
    accent_btn  = make_accent_button_theme()
    danger_btn  = make_danger_button_theme()

    with dpg.group(tag="view_dashboard", show=True):

        # ── Row 1: Connection card + Temperature card ────────────────────
        with dpg.group(horizontal=True):

            # ── Connection card ──────────────────────────────────────────
            with dpg.child_window(tag="card_connection", width=340, height=170, border=True):
                dpg.add_text("Device Connection", color=C_ACCENT)
                dpg.add_separator()
                dpg.add_spacer(height=4)

                with dpg.group(horizontal=True):
                    dpg.add_text("Port", color=C_SUBTEXT)
                    dpg.add_spacer(width=8)
                    dpg.add_combo(
                        tag="combo_port",
                        items=[],
                        default_value="",
                        width=160,
                        callback=_on_port_selected,
                    )
                    dpg.add_spacer(width=6)
                    dpg.add_button(
                        label="Scan",
                        tag="btn_scan",
                        callback=_on_scan,
                        width=60,
                    )

                dpg.add_spacer(height=6)
                with dpg.group(horizontal=True):
                    dpg.add_button(
                        label="Auto-detect",
                        tag="btn_autodetect",
                        callback=_on_autodetect,
                        width=110,
                    )
                    dpg.bind_item_theme("btn_autodetect", accent_btn)
                    dpg.add_spacer(width=8)
                    dpg.add_button(
                        label="Connect",
                        tag="btn_connect",
                        callback=_on_connect,
                        width=80,
                    )
                    dpg.bind_item_theme("btn_connect", accent_btn)
                    dpg.add_spacer(width=8)
                    dpg.add_button(
                        label="Disconnect",
                        tag="btn_disconnect",
                        callback=_on_disconnect,
                        width=90,
                        enabled=False,
                    )
                    dpg.bind_item_theme("btn_disconnect", danger_btn)

                dpg.add_spacer(height=8)
                dpg.add_text("● Not connected", tag="txt_conn_status", color=C_RED)

            dpg.add_spacer(width=12)

            # ── Temperature card ─────────────────────────────────────────
            with dpg.child_window(tag="card_temp", width=220, height=170, border=True):
                dpg.add_text("Live Temperature", color=C_ACCENT)
                dpg.add_separator()
                dpg.add_spacer(height=6)
                dpg.add_text("---.-- °C", tag="txt_tc_large", color=C_TEXT)
                dpg.add_spacer(height=4)
                dpg.add_text("Ref: --.-- °C", tag="txt_ref_small", color=C_SUBTEXT)
                dpg.add_spacer(height=8)
                dpg.add_text("Fault: —", tag="txt_fault_status", color=C_SUBTEXT)

            dpg.add_spacer(width=12)

            # ── Quick actions card ────────────────────────────────────────
            with dpg.child_window(tag="card_quick", width=220, height=170, border=True):
                dpg.add_text("Quick Actions", color=C_ACCENT)
                dpg.add_separator()
                dpg.add_spacer(height=8)
                dpg.add_button(
                    label="Start Calibration",
                    tag="btn_goto_cal",
                    callback=lambda: _nav("calibration"),
                    width=-1,
                )
                dpg.bind_item_theme("btn_goto_cal", accent_btn)
                dpg.add_spacer(height=6)
                dpg.add_button(
                    label="Start Follow Me",
                    tag="btn_goto_fm",
                    callback=lambda: _nav("follow_me"),
                    width=-1,
                )
                dpg.bind_item_theme("btn_goto_fm", accent_btn)
                dpg.add_spacer(height=6)
                dpg.add_text("", tag="txt_log_path", color=C_SUBTEXT, wrap=200)

        dpg.add_spacer(height=10)

        # ── Row 2: Mini live plot ─────────────────────────────────────────
        with dpg.child_window(height=280, border=True):
            dpg.add_text("Last 60 s", color=C_ACCENT)
            dpg.add_separator()
            _plot.build(height=-1, width=-1, label="")


def update() -> None:
    """Called every frame to refresh displayed values from APP state."""
    from ...state import APP

    if APP.connected:
        dpg.set_value("txt_conn_status", f"● Connected  —  {APP.port}")
        dpg.configure_item("txt_conn_status", color=C_GREEN)
        dpg.configure_item("btn_connect",     enabled=False)
        dpg.configure_item("btn_disconnect",  enabled=True)
    else:
        dpg.set_value("txt_conn_status", "● Not connected")
        dpg.configure_item("txt_conn_status", color=C_RED)
        dpg.configure_item("btn_connect",     enabled=True)
        dpg.configure_item("btn_disconnect",  enabled=False)

    if APP.current_tc:
        dpg.set_value("txt_tc_large",    f"{APP.current_tc:>6.2f} °C")
        dpg.set_value("txt_ref_small",   f"Ref: {APP.current_ref:.2f} °C")

    if APP.current_fault:
        fi = APP.last_fault_info
        parts = []
        if fi.get("open_circuit"): parts.append("open circuit")
        if fi.get("short_gnd"):    parts.append("short GND")
        if fi.get("short_vcc"):    parts.append("short VCC")
        dpg.set_value("txt_fault_status", f"Fault: {', '.join(parts) or 'yes'}")
        dpg.configure_item("txt_fault_status", color=C_RED)
    else:
        dpg.set_value("txt_fault_status", "Fault: none")
        dpg.configure_item("txt_fault_status", color=C_SUBTEXT)

    if APP.logger:
        dpg.set_value("txt_log_path", f"Log: {APP.logger.path.name}")

    # Feed the mini-plot
    import time
    if APP.temp_history:
        latest = list(APP.temp_history)[-1]
        elapsed = latest["t"] / 1000.0
        _plot.push_sample(elapsed, latest["tc"])


# ── Callbacks ─────────────────────────────────────────────────────────────

def _nav(view: str) -> None:
    from ..app import switch_view
    switch_view(view)


def _on_scan(sender, app_data) -> None:
    from ...serial_comm import list_ports
    ports = list_ports()
    dpg.configure_item("combo_port", items=ports)
    if ports:
        dpg.set_value("combo_port", ports[0])


def _on_port_selected(sender, app_data) -> None:
    pass  # selection stored in combo widget; read on connect


def _on_autodetect(sender, app_data) -> None:
    from ...state import APP
    if APP.serial and APP.serial.connect_auto():
        APP.port = APP.serial._port or ""
    else:
        dpg.set_value("txt_conn_status", "● Auto-detect failed — no Pico found")
        dpg.configure_item("txt_conn_status", color=C_YELLOW)


def _on_connect(sender, app_data) -> None:
    from ...state import APP
    port = dpg.get_value("combo_port")
    if port and APP.serial:
        APP.serial.connect(port)
        APP.port = port


def _on_disconnect(sender, app_data) -> None:
    from ...state import APP
    if APP.serial:
        APP.serial.disconnect()
    APP.connected = False
    APP.port = ""
