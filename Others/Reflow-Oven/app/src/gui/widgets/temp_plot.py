"""Reusable live temperature plot widget for Dear PyGui.

Creates a Dear PyGui plot with up to four series:
  - actual    : live thermocouple readings (blue solid)
  - target    : reflow profile target curve (amber dashed)
  - upper     : upper tolerance bound (red, thin)
  - lower     : lower tolerance bound (red, thin)

Usage:
    widget = TempPlotWidget("dash")
    # inside a DPG parent context:
    widget.build(height=300)
    # in the render loop:
    widget.push_sample(elapsed_s, tc_temp)
    widget.set_profile_curve(times, temps)
    widget.set_tolerance(5.0)
"""

from __future__ import annotations

from collections import deque
from typing import Optional

import dearpygui.dearpygui as dpg

from ..theme import C_ACCENT, C_ORANGE, C_RED


class TempPlotWidget:
    def __init__(self, tag_prefix: str, maxlen: int = 600):
        self._p   = tag_prefix
        self._q:  deque = deque(maxlen=maxlen)  # (elapsed_s, tc_temp)
        self._tolerance: float = 5.0
        self._profile_times: list = []
        self._profile_temps: list = []
        self._scroll_window: float = 120.0  # seconds to show in live mode
        self._live_scroll: bool = True

        # DPG tags
        self._plot_tag    = f"{self._p}_plot"
        self._x_tag       = f"{self._p}_xaxis"
        self._y_tag       = f"{self._p}_yaxis"
        self._actual_tag  = f"{self._p}_actual"
        self._target_tag  = f"{self._p}_target"
        self._upper_tag   = f"{self._p}_upper"
        self._lower_tag   = f"{self._p}_lower"

    def build(self, height: int = -1, width: int = -1, label: str = "Temperature") -> None:
        """Build the plot inside the current DPG parent context."""
        with dpg.plot(
            tag=self._plot_tag,
            label=label,
            height=height,
            width=width,
            anti_aliased=True,
        ):
            dpg.add_plot_legend()
            dpg.add_plot_axis(dpg.mvXAxis, label="Time (s)", tag=self._x_tag)
            with dpg.plot_axis(dpg.mvYAxis, label="°C", tag=self._y_tag):
                # Tolerance band (drawn first so lines render on top)
                dpg.add_shade_series(
                    [], [],
                    y2=[],
                    label=f"±{self._tolerance:.0f} °C",
                    tag=self._upper_tag,
                )
                # Target profile overlay
                dpg.add_line_series(
                    [], [],
                    label="Target",
                    tag=self._target_tag,
                )
                # Actual temperature
                dpg.add_line_series(
                    [], [],
                    label="Actual",
                    tag=self._actual_tag,
                )

        # Colour the series items after creation
        with dpg.theme() as actual_theme:
            with dpg.theme_component(dpg.mvLineSeries):
                dpg.add_theme_color(dpg.mvPlotCol_Line, C_ACCENT, category=dpg.mvThemeCat_Plots)
                dpg.add_theme_style(dpg.mvPlotStyleVar_LineWeight, 2, category=dpg.mvThemeCat_Plots)
        dpg.bind_item_theme(self._actual_tag, actual_theme)

        with dpg.theme() as target_theme:
            with dpg.theme_component(dpg.mvLineSeries):
                dpg.add_theme_color(dpg.mvPlotCol_Line, C_ORANGE, category=dpg.mvThemeCat_Plots)
                dpg.add_theme_style(dpg.mvPlotStyleVar_LineWeight, 1, category=dpg.mvThemeCat_Plots)
        dpg.bind_item_theme(self._target_tag, target_theme)

        with dpg.theme() as band_theme:
            with dpg.theme_component(dpg.mvShadeSeries):
                dpg.add_theme_color(dpg.mvPlotCol_Fill, (243, 139, 168, 40), category=dpg.mvThemeCat_Plots)
                dpg.add_theme_color(dpg.mvPlotCol_Line, (243, 139, 168, 80), category=dpg.mvThemeCat_Plots)
        dpg.bind_item_theme(self._upper_tag, band_theme)

    # ── Data update API ──────────────────────────────────────────────────

    def push_sample(self, elapsed_s: float, tc_temp: float) -> None:
        """Add a new temperature reading and refresh the actual series."""
        self._q.append((elapsed_s, tc_temp))
        self._refresh_actual()

    def clear(self) -> None:
        self._q.clear()
        self._profile_times.clear()
        self._profile_temps.clear()
        self._refresh_all()

    def set_tolerance(self, tolerance: float) -> None:
        self._tolerance = tolerance
        self._refresh_band()

    def set_profile_curve(self, times: list[float], temps: list[float]) -> None:
        self._profile_times = times
        self._profile_temps = temps
        self._refresh_target()
        self._refresh_band()

    # ── Internal refresh ─────────────────────────────────────────────────

    def _refresh_actual(self) -> None:
        if not self._q:
            return
        xs = [p[0] for p in self._q]
        ys = [p[1] for p in self._q]
        dpg.set_value(self._actual_tag, [xs, ys])
        if self._live_scroll and xs:
            x_max = xs[-1]
            x_min = max(0.0, x_max - self._scroll_window)
            dpg.set_axis_limits(self._x_tag, x_min, x_max + 5)
        else:
            dpg.set_axis_limits_auto(self._x_tag)

    def _refresh_target(self) -> None:
        dpg.set_value(self._target_tag, [self._profile_times, self._profile_temps])

    def _refresh_band(self) -> None:
        if not self._profile_times:
            return
        upper = [t + self._tolerance for t in self._profile_temps]
        lower = [t - self._tolerance for t in self._profile_temps]
        dpg.set_value(self._upper_tag, [self._profile_times, upper, lower])

    def _refresh_all(self) -> None:
        dpg.set_value(self._actual_tag,  [[], []])
        dpg.set_value(self._target_tag,  [[], []])
        dpg.set_value(self._upper_tag,   [[], [], []])
