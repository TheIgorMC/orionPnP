"""
REFLOW OVEN — Step Response Analysis & Control System Tuning
Analyzes heating/cooling curves, inertia, and proportional control parameters.
"""

import argparse
import csv
import json
import sys
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path

import numpy as np
from scipy import signal
from scipy.interpolate import interp1d

try:
    import matplotlib.pyplot as plt
except ImportError:
    plt = None


@dataclass
class DataPoint:
    host_ts: datetime
    device_time_ms: int
    temp_C: float
    top: int
    bottom: int


def load_csv(csv_path: str) -> list[DataPoint]:
    """Load and parse the reflow oven CSV log."""
    points = []
    with open(csv_path, "r", encoding="utf-8") as f:
        reader = csv.DictReader(f)
        for row in reader:
            try:
                temp = float(row["temp_C"])
            except ValueError:
                continue  # Skip error rows

            points.append(
                DataPoint(
                    host_ts=datetime.fromisoformat(row["host_timestamp_iso"]),
                    device_time_ms=int(row["device_time_ms"]),
                    temp_C=temp,
                    top=int(row["top"]),
                    bottom=int(row["bottom"]),
                )
            )
    return points


def load_profile(profile_path: str) -> dict | None:
    """Load a reflow profile JSON (e.g., leaded.json)."""
    if not profile_path:
        return None
    
    try:
        with open(profile_path, "r", encoding="utf-8") as f:
            return json.load(f)
    except (FileNotFoundError, json.JSONDecodeError) as e:
        print(f"Warning: Could not load profile {profile_path}: {e}")
        return None


def interpolate_profile(profile: dict, max_time_s: float) -> tuple[np.ndarray, np.ndarray] | None:
    """Interpolate profile points to create a continuous curve."""
    if not profile or "profile_points" not in profile:
        return None
    
    points = profile["profile_points"]
    if len(points) < 2:
        return None
    
    times = np.array([p["time_s"] for p in points])
    temps = np.array([p["temp_c"] for p in points])
    
    # Interpolate and extend to max_time_s
    interp_func = interp1d(times, temps, kind="linear", fill_value="extrapolate")
    time_dense = np.linspace(0, max_time_s, int(max_time_s * 10))  # 10 Hz
    temp_interp = interp_func(time_dense)
    
    return time_dense, temp_interp


def calculate_heating_rate(points: list[DataPoint], window_size: int = 5) -> list[float]:
    """
    Calculate dT/dt (deg/s) using rolling differentiation.
    window_size: number of samples to use for each derivative estimate.
    """
    rates = []
    for i in range(len(points)):
        start = max(0, i - window_size // 2)
        end = min(len(points), i + window_size // 2 + 1)

        if end - start < 2:
            rates.append(0.0)
            continue

        dt_ms = (
            points[end - 1].device_time_ms - points[start].device_time_ms
        )
        dtemp = points[end - 1].temp_C - points[start].temp_C

        if dt_ms > 0:
            rate_deg_per_s = (dtemp / dt_ms) * 1000
        else:
            rate_deg_per_s = 0.0

        rates.append(rate_deg_per_s)

    return rates


def detect_step_changes(points: list[DataPoint]) -> list[dict]:
    """
    Detect when SSR states changed (step input).
    Returns list of events with timing and context.
    """
    events = []
    if not points:
        return events

    prev_state = (points[0].top, points[0].bottom)

    for i, p in enumerate(points):
        current_state = (p.top, p.bottom)
        if current_state != prev_state:
            events.append(
                {
                    "index": i,
                    "device_time_ms": p.device_time_ms,
                    "temp_C": p.temp_C,
                    "prev_state": prev_state,
                    "new_state": current_state,
                    "heating_power": current_state[0] + current_state[1],
                }
            )
            prev_state = current_state

    return events


def analyze_step_response(
    points: list[DataPoint],
    step_event: dict,
    window_after_ms: int = 30000,
) -> dict:
    """
    Analyze the temperature response to a step input.
    Estimates settling time, rise time, and steady-state characteristics.
    """
    step_idx = step_event["index"]
    step_time = step_event["device_time_ms"]
    initial_temp = step_event["temp_C"]

    # Collect samples after step for the analysis window
    post_step = [
        p
        for p in points[step_idx:]
        if p.device_time_ms <= step_time + window_after_ms
    ]

    if len(post_step) < 2:
        return {"error": "Insufficient data after step"}

    temps = [p.temp_C for p in post_step]
    times_ms = np.array([p.device_time_ms - step_time for p in post_step])

    # Steady state approximation (last 20% of window)
    if len(temps) > 5:
        ss_temp = np.mean(temps[-5:])
    else:
        ss_temp = temps[-1]

    # Peak temperature and overshoot
    peak_temp = max(temps)
    overshoot = peak_temp - ss_temp

    # Time constants estimation (naive)
    # Approximate 63% settling (one time constant)
    target_63 = initial_temp + 0.63 * (ss_temp - initial_temp)

    tau_idx = next((i for i, t in enumerate(temps) if t >= target_63), None)
    tau_ms = times_ms[tau_idx] if tau_idx is not None else None

    return {
        "power_level": step_event["heating_power"],
        "initial_temp": initial_temp,
        "steady_state_temp": ss_temp,
        "peak_temp": peak_temp,
        "overshoot": overshoot,
        "overshoot_percent": (overshoot / (ss_temp - initial_temp) * 100)
        if ss_temp != initial_temp
        else 0,
        "tau_ms": tau_ms,
        "settling_time_ms": window_after_ms,
        "heating": step_event["heating_power"] > 0,
    }


def estimate_control_parameters(
    points: list[DataPoint],
    step_events: list[dict],
) -> dict:
    """
    Estimate proportional control parameters from step responses.
    Uses dominant pole estimation and settling time.
    """
    if not step_events:
        return {"error": "No step events found"}

    # Analyze first few steps (both heating and cooling)
    heating_responses = []
    cooling_responses = []

    for event in step_events[:10]:  # Limit to first 10 transitions
        response = analyze_step_response(points, event, window_after_ms=30000)
        if "error" not in response:
            if response["heating"]:
                heating_responses.append(response)
            else:
                cooling_responses.append(response)

    params = {
        "heating_responses": heating_responses,
        "cooling_responses": cooling_responses,
    }

    # Estimate Kp (proportional gain) from steady-state gain
    # For on/off: steady-state temp ≈ Kp * power_input
    if heating_responses:
        heat_gains = [
            r["steady_state_temp"] / (r["power_level"] or 0.1)
            for r in heating_responses
            if r["power_level"] > 0
        ]
        params["avg_heating_gain"] = (
            np.mean(heat_gains) if heat_gains else None
        )
        params["avg_heating_tau_ms"] = np.mean(
            [r["tau_ms"] for r in heating_responses if r["tau_ms"]]
        )

    if cooling_responses:
        params["avg_cooling_rate_degC_per_s"] = np.mean(
            [
                (r["initial_temp"] - r["steady_state_temp"])
                / (r["tau_ms"] / 1000)
                if r["tau_ms"]
                else 0
                for r in cooling_responses
            ]
        )

    return params


def print_report(
    points: list[DataPoint],
    rates: list[float],
    step_events: list[dict],
    control_params: dict,
) -> None:
    """Print a comprehensive analysis report."""
    print("\n" + "=" * 70)
    print("REFLOW OVEN — STEP RESPONSE ANALYSIS REPORT")
    print("=" * 70)

    # Basic stats
    temps = [p.temp_C for p in points]
    
    # Find SSR start time
    first_ssr_time_ms = None
    for p in points:
        if p.top + p.bottom > 0:
            first_ssr_time_ms = p.device_time_ms
            break
    if first_ssr_time_ms is None:
        first_ssr_time_ms = points[0].device_time_ms
    
    print(f"\nData Collection:")
    print(f"  Total duration: {points[-1].device_time_ms / 1000:.1f} seconds")
    print(f"  SSR started at: {first_ssr_time_ms / 1000:.1f} seconds (t=0 reference)")
    print(f"  Samples: {len(points)}")
    print(f"  Temp range: {min(temps):.1f}°C to {max(temps):.1f}°C")

    # Heating/Cooling rates
    valid_rates = [r for r in rates if abs(r) > 0.01]
    print(f"\nHeating/Cooling Rates:")
    heating_rates = [r for r in valid_rates if r > 0]
    cooling_rates = [r for r in valid_rates if r < 0]

    if heating_rates:
        print(f"  Heating:  {np.mean(heating_rates):.2f} ± {np.std(heating_rates):.2f} °C/s")
        print(f"            (max: {max(heating_rates):.2f} °C/s)")
    if cooling_rates:
        print(f"  Cooling:  {np.mean(cooling_rates):.2f} ± {np.std(cooling_rates):.2f} °C/s")
        print(f"            (min: {min(cooling_rates):.2f} °C/s)")

    # Step events
    print(f"\nStep Changes (SSR transitions): {len(step_events)}")
    for i, evt in enumerate(step_events[:5]):  # Show first 5
        print(
            f"  [{i+1}] @ {evt['device_time_ms']/1000:.1f}s: "
            f"{evt['prev_state']} → {evt['new_state']} "
            f"(T={evt['temp_C']:.1f}°C)"
        )

    # Control parameters
    if "error" not in control_params:
        print(f"\nEstimated Control Parameters:")

        if control_params.get("avg_heating_tau_ms"):
            print(
                f"  Heating time constant τ: {control_params['avg_heating_tau_ms']:.0f} ms"
            )
        if control_params.get("avg_heating_gain"):
            print(
                f"  Heating DC gain: {control_params['avg_heating_gain']:.2f} °C per unit power"
            )
        if control_params.get("avg_cooling_rate_degC_per_s"):
            print(
                f"  Cooling rate: {control_params['avg_cooling_rate_degC_per_s']:.3f} °C/s"
            )

        print(f"\nStep Response Summary:")
        if control_params["heating_responses"]:
            h = control_params["heating_responses"][0]
            print(f"  Heating:")
            print(f"    Initial: {h['initial_temp']:.1f}°C → SS: {h['steady_state_temp']:.1f}°C")
            print(f"    Overshoot: {h['overshoot']:.1f}°C ({h['overshoot_percent']:.1f}%)")
            if h["tau_ms"]:
                print(f"    τ (63%): {h['tau_ms']:.0f} ms")

        if control_params["cooling_responses"]:
            c = control_params["cooling_responses"][0]
            print(f"  Cooling:")
            print(f"    Initial: {c['initial_temp']:.1f}°C → SS: {c['steady_state_temp']:.1f}°C")
            if c["tau_ms"]:
                print(f"    τ (63%): {c['tau_ms']:.0f} ms")

    print("\n" + "=" * 70)


def plot_analysis(
    points: list[DataPoint],
    rates: list[float],
    step_events: list[dict],
    output_png: str = "reflow_analysis.png",
    profile: dict | None = None,
) -> None:
    """Generate diagnostic plots, optionally overlaying a reflow profile.\n    Times are aligned to t=0 at first SSR activation.
    """
    if plt is None:
        print("matplotlib not available; skipping plots.")
        return

    # Find first SSR activation (when heating starts)
    first_ssr_time_ms = None
    for p in points:
        if p.top + p.bottom > 0:
            first_ssr_time_ms = p.device_time_ms
            break
    
    # If no SSR activation found, use start of data
    if first_ssr_time_ms is None:
        first_ssr_time_ms = points[0].device_time_ms
    
    fig, axes = plt.subplots(3, 1, figsize=(14, 10))

    # Convert to numpy arrays for easier plotting, with time offset to SSR start
    times_s = np.array([(p.device_time_ms - first_ssr_time_ms) / 1000 for p in points])
    temps = np.array([p.temp_C for p in points])
    ssr_power = np.array(
        [p.top + p.bottom for p in points]
    )

    # Plot 1: Temperature over time
    ax = axes[0]
    ax.plot(times_s, temps, "b-", linewidth=1.5, label="Measured")
    
    # Overlay profile if provided
    if profile:
        profile_interp = interpolate_profile(profile, times_s[-1])
        if profile_interp is not None:
            profile_times, profile_temps = profile_interp
            ax.plot(
                profile_times,
                profile_temps,
                "r--",
                linewidth=2,
                label=f"{profile.get('paste', 'Profile')}",
                alpha=0.8,
            )
            # Add liquidus zone shading
            if "liquidus_temp" in profile:
                ax.axhline(
                    profile["liquidus_temp"],
                    color="orange",
                    linestyle=":",
                    linewidth=1,
                    alpha=0.6,
                    label=f"Liquidus ({profile['liquidus_temp']}°C)",
                )
    
    ax.scatter(
        [(e["device_time_ms"] - first_ssr_time_ms) / 1000 for e in step_events],
        [e["temp_C"] for e in step_events],
        color="red",
        s=100,
        marker="x",
        label="Step events",
        zorder=5,
    )
    ax.set_xlabel("Time (s)")
    ax.set_ylabel("Temperature (°C)")
    ax.set_title("Temperature Profile")
    ax.grid(True, alpha=0.3)
    ax.legend(loc="best")

    # Plot 2: Heating/Cooling rate
    ax = axes[1]
    ax.plot(times_s, rates, "g-", linewidth=1, alpha=0.7, label="dT/dt (rolling)")
    ax.axhline(0, color="k", linestyle="--", alpha=0.3)
    ax.set_xlabel("Time (s)")
    ax.set_ylabel("Rate (°C/s)")
    ax.set_title("Heating / Cooling Rate")
    ax.grid(True, alpha=0.3)
    ax.legend()

    # Plot 3: SSR power input
    ax = axes[2]
    ax.step(times_s, ssr_power, "r-", linewidth=1.5, where="post", label="Power level")
    ax.set_xlabel("Time (s)")
    ax.set_ylabel("Power (2=both, 1=one, 0=off)")
    ax.set_title("SSR Output (Top + Bottom)")
    ax.set_ylim(-0.5, 2.5)
    ax.grid(True, alpha=0.3)
    ax.legend()

    plt.tight_layout()
    plt.savefig(output_png, dpi=150)
    print(f"Plots saved to {output_png}")
    plt.show()


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Analyze reflow oven step response and estimate control parameters."
    )
    parser.add_argument("csv_file", help="Path to reflow_log_*.csv")
    parser.add_argument(
        "--output",
        help="Output PNG for plots (e.g. analysis.png)",
    )
    parser.add_argument(
        "--profile",
        help="Reflow profile JSON (e.g. leaded.json) to overlay on plots",
    )
    parser.add_argument(
        "--no-plot",
        action="store_true",
        help="Skip matplotlib plots.",
    )
    args = parser.parse_args()

    csv_path = Path(args.csv_file)
    if not csv_path.exists():
        print(f"Error: {csv_path} not found.")
        return 1

    print(f"Loading {csv_path}...")
    points = load_csv(str(csv_path))

    if len(points) < 2:
        print("Error: CSV contains fewer than 2 data points.")
        return 1

    print(f"Loaded {len(points)} points.")

    # Load profile if provided
    profile = load_profile(args.profile) if args.profile else None
    if args.profile and profile:
        print(f"Loaded profile: {profile.get('paste', 'Unknown paste')}")

    # Calculate derivatives and detect events
    rates = calculate_heating_rate(points, window_size=5)
    step_events = detect_step_changes(points)
    control_params = estimate_control_parameters(points, step_events)

    # Print report
    print_report(points, rates, step_events, control_params)

    # Plot (optional)
    if not args.no_plot:
        output_png = args.output or "reflow_analysis.png"
        plot_analysis(points, rates, step_events, output_png, profile=profile)

    return 0


if __name__ == "__main__":
    sys.exit(main())
