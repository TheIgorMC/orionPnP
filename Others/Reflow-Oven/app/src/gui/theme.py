"""Dear PyGui theme — Catppuccin Mocha-inspired dark palette.

Call apply_global_theme() once after dpg.create_context().
Use the make_*_theme() helpers to create per-item accent themes.
"""

from __future__ import annotations

import dearpygui.dearpygui as dpg

# ── Palette ───────────────────────────────────────────────────────────────
C_BG       = (30,  30,  46,  255)   # #1e1e2e  — main background
C_BG_PANEL = (24,  24,  37,  255)   # #181825  — sidebar / cards
C_SURFACE  = (49,  50,  68,  255)   # #313244  — frame / button bg
C_HOVER    = (69,  71,  90,  255)   # #45475a  — hovered items
C_BORDER   = (88,  91,  112, 255)   # #585b70  — borders
C_TEXT     = (205, 214, 244, 255)   # #cdd6f4  — primary text
C_SUBTEXT  = (166, 173, 200, 255)   # #a6adc8  — secondary text
C_ACCENT   = (137, 180, 250, 255)   # #89b4fa  — blue  (headings, active)
C_GREEN    = (166, 227, 161, 255)   # #a6e3a1  — connected / OK
C_YELLOW   = (249, 226, 175, 255)   # #f9e2af  — warning
C_ORANGE   = (250, 179, 135, 255)   # #fab387  — target profile line
C_RED      = (243, 139, 168, 255)   # #f38ba8  — danger / fault
C_RED_DIM  = (243, 139, 168, 60)    # #f38ba8  — tolerance band fill


def apply_global_theme() -> None:
    """Apply the Catppuccin Mocha theme globally to all items."""
    with dpg.theme() as global_theme:
        with dpg.theme_component(dpg.mvAll):
            # Window / panels
            dpg.add_theme_color(dpg.mvThemeCol_WindowBg,        C_BG,       category=dpg.mvThemeCat_Core)
            dpg.add_theme_color(dpg.mvThemeCol_ChildBg,         C_BG_PANEL, category=dpg.mvThemeCat_Core)
            dpg.add_theme_color(dpg.mvThemeCol_PopupBg,         C_BG_PANEL, category=dpg.mvThemeCat_Core)
            dpg.add_theme_color(dpg.mvThemeCol_Border,          C_BORDER,   category=dpg.mvThemeCat_Core)
            dpg.add_theme_color(dpg.mvThemeCol_BorderShadow,    (0,0,0,0),  category=dpg.mvThemeCat_Core)
            # Text
            dpg.add_theme_color(dpg.mvThemeCol_Text,            C_TEXT,     category=dpg.mvThemeCat_Core)
            dpg.add_theme_color(dpg.mvThemeCol_TextDisabled,    C_SUBTEXT,  category=dpg.mvThemeCat_Core)
            # Buttons
            dpg.add_theme_color(dpg.mvThemeCol_Button,          C_SURFACE,  category=dpg.mvThemeCat_Core)
            dpg.add_theme_color(dpg.mvThemeCol_ButtonHovered,   C_HOVER,    category=dpg.mvThemeCat_Core)
            dpg.add_theme_color(dpg.mvThemeCol_ButtonActive,    C_ACCENT,   category=dpg.mvThemeCat_Core)
            # Headers (collapsing headers etc.)
            dpg.add_theme_color(dpg.mvThemeCol_Header,          C_SURFACE,  category=dpg.mvThemeCat_Core)
            dpg.add_theme_color(dpg.mvThemeCol_HeaderHovered,   C_HOVER,    category=dpg.mvThemeCat_Core)
            dpg.add_theme_color(dpg.mvThemeCol_HeaderActive,    C_ACCENT,   category=dpg.mvThemeCat_Core)
            # Frames (inputs, combos)
            dpg.add_theme_color(dpg.mvThemeCol_FrameBg,         C_SURFACE,  category=dpg.mvThemeCat_Core)
            dpg.add_theme_color(dpg.mvThemeCol_FrameBgHovered,  C_HOVER,    category=dpg.mvThemeCat_Core)
            dpg.add_theme_color(dpg.mvThemeCol_FrameBgActive,   C_ACCENT,   category=dpg.mvThemeCat_Core)
            # Title bars
            dpg.add_theme_color(dpg.mvThemeCol_TitleBg,         C_BG_PANEL, category=dpg.mvThemeCat_Core)
            dpg.add_theme_color(dpg.mvThemeCol_TitleBgActive,   C_BG_PANEL, category=dpg.mvThemeCat_Core)
            # Scrollbar
            dpg.add_theme_color(dpg.mvThemeCol_ScrollbarBg,     C_BG,       category=dpg.mvThemeCat_Core)
            dpg.add_theme_color(dpg.mvThemeCol_ScrollbarGrab,   C_SURFACE,  category=dpg.mvThemeCat_Core)
            dpg.add_theme_color(dpg.mvThemeCol_ScrollbarGrabHovered, C_HOVER, category=dpg.mvThemeCat_Core)
            # Separator
            dpg.add_theme_color(dpg.mvThemeCol_Separator,       C_BORDER,   category=dpg.mvThemeCat_Core)
            # Plot
            dpg.add_theme_color(dpg.mvThemeCol_PlotLines,       C_ACCENT,   category=dpg.mvThemeCat_Core)
            dpg.add_theme_color(dpg.mvPlotCol_FrameBg,          C_BG,       category=dpg.mvThemeCat_Plots)
            dpg.add_theme_color(dpg.mvPlotCol_PlotBg,           C_BG,       category=dpg.mvThemeCat_Plots)
            dpg.add_theme_color(dpg.mvPlotCol_PlotBorder,       C_BORDER,   category=dpg.mvThemeCat_Plots)
            # Rounding & spacing
            dpg.add_theme_style(dpg.mvStyleVar_WindowRounding,  8,    category=dpg.mvThemeCat_Core)
            dpg.add_theme_style(dpg.mvStyleVar_FrameRounding,   6,    category=dpg.mvThemeCat_Core)
            dpg.add_theme_style(dpg.mvStyleVar_ChildRounding,   8,    category=dpg.mvThemeCat_Core)
            dpg.add_theme_style(dpg.mvStyleVar_GrabRounding,    4,    category=dpg.mvThemeCat_Core)
            dpg.add_theme_style(dpg.mvStyleVar_WindowPadding,   12, 12, category=dpg.mvThemeCat_Core)
            dpg.add_theme_style(dpg.mvStyleVar_FramePadding,    6,  4,  category=dpg.mvThemeCat_Core)
            dpg.add_theme_style(dpg.mvStyleVar_ItemSpacing,     8,  6,  category=dpg.mvThemeCat_Core)
            dpg.add_theme_style(dpg.mvStyleVar_ScrollbarSize,   10,      category=dpg.mvThemeCat_Core)

    dpg.bind_theme(global_theme)


def make_accent_button_theme() -> int:
    """Blue filled button (for primary actions)."""
    with dpg.theme() as t:
        with dpg.theme_component(dpg.mvButton):
            dpg.add_theme_color(dpg.mvThemeCol_Button,        C_ACCENT,             category=dpg.mvThemeCat_Core)
            dpg.add_theme_color(dpg.mvThemeCol_ButtonHovered, (107, 150, 220, 255), category=dpg.mvThemeCat_Core)
            dpg.add_theme_color(dpg.mvThemeCol_ButtonActive,  (80,  120, 200, 255), category=dpg.mvThemeCat_Core)
            dpg.add_theme_color(dpg.mvThemeCol_Text,          C_BG,                 category=dpg.mvThemeCat_Core)
    return t


def make_danger_button_theme() -> int:
    """Red filled button (destructive / stop actions)."""
    with dpg.theme() as t:
        with dpg.theme_component(dpg.mvButton):
            dpg.add_theme_color(dpg.mvThemeCol_Button,        C_RED,                category=dpg.mvThemeCat_Core)
            dpg.add_theme_color(dpg.mvThemeCol_ButtonHovered, (220, 100, 130, 255), category=dpg.mvThemeCat_Core)
            dpg.add_theme_color(dpg.mvThemeCol_ButtonActive,  (200,  80, 110, 255), category=dpg.mvThemeCat_Core)
            dpg.add_theme_color(dpg.mvThemeCol_Text,          C_BG,                 category=dpg.mvThemeCat_Core)
    return t


def make_alert_child_theme() -> int:
    """Red background child window (fault / deviation alert card)."""
    with dpg.theme() as t:
        with dpg.theme_component(dpg.mvChildWindow):
            dpg.add_theme_color(dpg.mvThemeCol_ChildBg, (80, 30, 40, 200), category=dpg.mvThemeCat_Core)
            dpg.add_theme_color(dpg.mvThemeCol_Border,  C_RED,             category=dpg.mvThemeCat_Core)
    return t


def make_success_child_theme() -> int:
    """Green-tinted child window (connected / OK status card)."""
    with dpg.theme() as t:
        with dpg.theme_component(dpg.mvChildWindow):
            dpg.add_theme_color(dpg.mvThemeCol_ChildBg, (30, 60, 40, 200), category=dpg.mvThemeCat_Core)
            dpg.add_theme_color(dpg.mvThemeCol_Border,  C_GREEN,           category=dpg.mvThemeCat_Core)
    return t
