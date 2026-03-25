"""Logs view — browse and open previous session CSV log files."""

from __future__ import annotations

from pathlib import Path

import dearpygui.dearpygui as dpg

from ..theme import C_ACCENT, C_GREEN, C_SUBTEXT, C_TEXT, make_accent_button_theme

_LOGS_DIR = Path("logs")
_accent   = None


def build() -> None:
    global _accent
    _accent = make_accent_button_theme()

    with dpg.group(tag="view_logs", show=False):
        dpg.add_text("Session Logs", color=C_ACCENT)
        dpg.add_separator()
        dpg.add_spacer(height=6)

        with dpg.group(horizontal=True):
            with dpg.child_window(width=300, height=460, border=True):
                dpg.add_text("Log files", color=C_ACCENT)
                dpg.add_separator()
                dpg.add_spacer(height=4)
                dpg.add_listbox(
                    tag="log_listbox",
                    items=[],
                    num_items=18,
                    width=-1,
                    callback=_on_log_selected,
                )
                dpg.add_spacer(height=8)
                dpg.add_button(label="Refresh", callback=_refresh_list, width=-1)

            dpg.add_spacer(width=12)

            with dpg.child_window(border=True, height=460):
                dpg.add_text("Preview (first 50 rows)", color=C_ACCENT)
                dpg.add_separator()
                dpg.add_spacer(height=4)
                dpg.add_text("Select a log file to preview.", tag="log_preview",
                             color=C_SUBTEXT, wrap=600)


def update() -> None:
    pass


# ── Helpers ────────────────────────────────────────────────────────────────

def _refresh_list() -> None:
    _LOGS_DIR.mkdir(parents=True, exist_ok=True)
    files = sorted(_LOGS_DIR.glob("session_*.csv"), reverse=True)
    dpg.configure_item("log_listbox", items=[f.name for f in files])


def _on_log_selected(sender, app_data) -> None:
    path = _LOGS_DIR / app_data
    if not path.exists():
        return
    try:
        with open(path, encoding="utf-8") as f:
            lines = f.readlines()[:51]   # header + 50 data rows
        preview = "".join(lines)
        dpg.set_value("log_preview", preview)
    except Exception as exc:
        dpg.set_value("log_preview", f"Error reading file: {exc}")
