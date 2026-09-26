"""Profile Manager view — browse, inspect, and select reflow profiles."""

from __future__ import annotations

from pathlib import Path

import dearpygui.dearpygui as dpg

from ..theme import (
    C_ACCENT, C_GREEN, C_ORANGE, C_RED, C_SUBTEXT, C_TEXT,
    make_accent_button_theme,
)

_PROFILES_DIR = Path("profiles")
_accent = None
_loaded_profiles: dict = {}   # name → Profile


def build() -> None:
    global _accent
    _accent = make_accent_button_theme()

    with dpg.group(tag="view_profiles", show=False):
        dpg.add_text("Profile Manager", color=C_ACCENT)
        dpg.add_separator()
        dpg.add_spacer(height=6)

        with dpg.group(horizontal=True):

            # ── Profile list ──────────────────────────────────────────────
            with dpg.child_window(width=240, height=460, border=True):
                dpg.add_text("Available profiles", color=C_ACCENT)
                dpg.add_separator()
                dpg.add_spacer(height=4)
                dpg.add_listbox(
                    tag="prof_listbox",
                    items=[],
                    num_items=15,
                    width=-1,
                    callback=_on_profile_selected,
                )
                dpg.add_spacer(height=8)
                dpg.add_button(label="Refresh", callback=_refresh_list, width=-1)

            dpg.add_spacer(width=12)

            # ── Profile detail ────────────────────────────────────────────
            with dpg.child_window(border=True, height=460):
                dpg.add_text("Profile Details", color=C_ACCENT)
                dpg.add_separator()
                dpg.add_spacer(height=4)
                dpg.add_text("—", tag="prof_name",       color=C_TEXT)
                dpg.add_text("—", tag="prof_paste_type", color=C_SUBTEXT)
                dpg.add_text("",  tag="prof_duration",   color=C_SUBTEXT)
                dpg.add_spacer(height=8)

                with dpg.table(
                    tag="prof_stage_table",
                    header_row=True,
                    borders_innerV=True,
                    borders_outerH=True,
                    policy=dpg.mvTable_SizingFixedFit,
                ):
                    dpg.add_table_column(label="Stage",         width_fixed=True, init_width_or_weight=110)
                    dpg.add_table_column(label="Target (°C)",   width_fixed=True, init_width_or_weight=110)
                    dpg.add_table_column(label="Ramp (°C/s)",   width_fixed=True, init_width_or_weight=110)
                    dpg.add_table_column(label="Dwell (s)",     width_fixed=True, init_width_or_weight=90)

                dpg.add_spacer(height=12)
                btn = dpg.add_button(
                    label="Select for Follow Me",
                    tag="prof_btn_select",
                    callback=_on_select_profile,
                    width=200,
                    enabled=False,
                )
                dpg.bind_item_theme(btn, _accent)
                dpg.add_spacer(height=4)
                dpg.add_text("", tag="prof_select_status", color=C_GREEN)


def update() -> None:
    """Called every frame — nothing to poll here; updates are event-driven."""
    pass


# ── Helpers ────────────────────────────────────────────────────────────────

def _refresh_list() -> None:
    from ...profile import list_profiles, load_profile
    global _loaded_profiles

    _loaded_profiles.clear()
    paths = list_profiles(_PROFILES_DIR)
    names = []
    for p in paths:
        try:
            profile = load_profile(p)
            _loaded_profiles[profile.name] = profile
            names.append(profile.name)
        except Exception:
            names.append(f"[error] {p.stem}")

    dpg.configure_item("prof_listbox", items=names)
    if names:
        dpg.set_value("prof_listbox", names[0])
        _show_profile(names[0])


def _on_profile_selected(sender, app_data) -> None:
    _show_profile(app_data)


def _show_profile(name: str) -> None:
    profile = _loaded_profiles.get(name)
    if profile is None:
        return

    dpg.set_value("prof_name",       profile.name)
    dpg.set_value("prof_paste_type", f"Paste: {profile.paste_type}")
    dpg.set_value("prof_duration",   f"Total dwell: ~{profile.total_dwell_s()} s")

    # Rebuild stage table rows
    for child in dpg.get_item_children("prof_stage_table", 1):
        dpg.delete_item(child)
    for stage in profile.stages:
        with dpg.table_row(parent="prof_stage_table"):
            dpg.add_text(stage.name)
            dpg.add_text(f"{stage.target_temp:.0f}")
            dpg.add_text(f"{stage.max_ramp_rate:.2f}")
            dpg.add_text(str(stage.dwell_seconds))

    dpg.configure_item("prof_btn_select", enabled=True)


def _on_select_profile(sender, app_data) -> None:
    from ...state import APP

    name = dpg.get_value("prof_listbox")
    profile = _loaded_profiles.get(name)
    if profile:
        APP.current_profile = profile
        dpg.set_value("prof_select_status", f"✓ Selected: {profile.name}")
