#!/usr/bin/env python3
"""Batch feeder generation from CSV with search-grounded Gemini inference.

Flow:
1) Group parts primarily by footprint.
2) Inside each footprint, split into variation groups by inferred series.
3) Query a search-enabled Gemini model per variation group to infer tape specs.
4) Generate one openPnP BlindsFeeder XML snippet per part.
5) Emit a report CSV so variations can be reviewed quickly.
"""

import argparse
import csv
import json
import os
import re
import sys
from collections import defaultdict
from pathlib import Path
from typing import Dict, List, Tuple

from dotenv import load_dotenv
from google import genai
from google.genai import types

from datasheet_to_feeder import build_feeder_xml


SERIES_PROMPT_TEMPLATE = """\
You are an expert in SMT packaging and tape-and-reel standards.

I will provide a footprint family, an inferred series, and example manufacturer part numbers.
Use web search to find likely datasheets and infer conservative tape-and-reel specs for pick and place.

Return ONLY a raw JSON object (no markdown) with exactly these keys:
{
  "tape_width_mm": <number>,
  "feed_pitch_mm": <number>,
  "pocket_width_mm": <number or null>,
  "pocket_length_mm": <number or null>,
  "pocket_depth_mm": <number or null>,
  "component_width_mm": <number or null>,
  "component_length_mm": <number or null>,
  "component_height_mm": <number or null>,
  "sprocket_pitch_mm": <number>,
  "tape_center_offset_mm": <number or null>,
  "datasheet_urls": <array of strings>,
  "confidence": <number from 0 to 1>,
  "reasoning_short": <short string>
}

Rules:
- Prefer conservative defaults when uncertain: tape_width=8, feed_pitch=4, sprocket_pitch=4.
- If the series includes 0402/0603/0805 passives, assume 8 mm tape, 4 mm feed pitch.
- If there is insufficient evidence for a field, use null (except tape_width/feed_pitch/sprocket_pitch).

Footprint group: {footprint}
Inferred series: {series}
Example manufacturer part numbers: {examples}
"""


def normalize_code(code: str) -> str:
    return (code or "").strip().upper()


def infer_series(code: str) -> str:
    """Infer a series-ish key from a manufacturer code."""
    token = normalize_code(code)
    if not token:
        return "UNKNOWN"

    token = re.sub(r"([-/_,]?(TR|TAPE|REEL|CUT|CT|TB|DKR|A|B|C))+$", "", token)
    head = re.split(r"[-_/,\s]", token)[0]

    match = re.match(r"^([A-Z]+\d{1,4})", head)
    if match:
        return match.group(1)

    match = re.match(r"^([A-Z]{2,8})", head)
    if match:
        return match.group(1)

    return head[:10] if head else "UNKNOWN"


def parse_json_response(raw: str) -> Dict[str, object]:
    text = (raw or "").strip()
    if text.startswith("```"):
        parts = text.split("```")
        if len(parts) > 1:
            text = parts[1]
        if text.startswith("json"):
            text = text[4:]
    return json.loads(text.strip() or "{}")


def estimate_specs_for_group(
    client: genai.Client,
    footprint: str,
    series: str,
    rows: List[Dict[str, str]],
) -> Dict[str, object]:
    examples = [normalize_code(r.get("manufacturer_code", "")) for r in rows]
    examples = [x for x in examples if x][:8]

    prompt = SERIES_PROMPT_TEMPLATE.format(
        footprint=footprint,
        series=series,
        examples=", ".join(examples) or "unknown",
    )

    response = client.models.generate_content(
        model="gemini-2.5-flash",
        contents=prompt,
        config=types.GenerateContentConfig(
            temperature=0.1,
            tools=[types.Tool(google_search=types.GoogleSearch())],
        ),
    )
    specs = parse_json_response(response.text)

    specs["tape_width_mm"] = float(specs.get("tape_width_mm") or 8.0)
    specs["feed_pitch_mm"] = float(specs.get("feed_pitch_mm") or 4.0)
    specs["sprocket_pitch_mm"] = float(specs.get("sprocket_pitch_mm") or 4.0)

    urls = specs.get("datasheet_urls")
    if not isinstance(urls, list):
        urls = []
    specs["datasheet_urls"] = urls

    return specs


def build_groups(rows: List[Dict[str, str]]) -> Dict[str, Dict[str, List[Dict[str, str]]]]:
    grouped: Dict[str, Dict[str, List[Dict[str, str]]]] = defaultdict(lambda: defaultdict(list))
    for row in rows:
        footprint = (row.get("smd_footprint") or "UNKNOWN").strip() or "UNKNOWN"
        series = infer_series(row.get("manufacturer_code", ""))
        grouped[footprint][series].append(row)
    return grouped


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Group CSV parts by footprint and variation, then generate feeder XML in batch."
    )
    parser.add_argument("csv_file", type=Path, help="Path to exported CSV file")
    parser.add_argument("--output", type=Path, default=Path("feeders_generated.xml"),
                        help="Output XML file (default: feeders_generated.xml)")
    parser.add_argument("--report", type=Path, default=Path("feeders_generated_report.csv"),
                        help="Report CSV with inferred specs and datasheet URLs")
    args = parser.parse_args()

    if not args.csv_file.exists():
        print(f"ERROR: CSV file not found: {args.csv_file}", file=sys.stderr)
        sys.exit(1)

    load_dotenv()
    api_key = os.getenv("GEMINI_API_KEY")
    if not api_key:
        print("ERROR: GEMINI_API_KEY not set. Put it in .env", file=sys.stderr)
        sys.exit(1)

    rows: List[Dict[str, str]] = []
    with args.csv_file.open("r", encoding="utf-8-sig", newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            rows.append(row)

    if not rows:
        print("ERROR: CSV is empty.", file=sys.stderr)
        sys.exit(1)

    grouped = build_groups(rows)
    client = genai.Client(api_key=api_key)

    specs_by_group: Dict[Tuple[str, str], Dict[str, object]] = {}
    for footprint, variations in grouped.items():
        for series, group_rows in variations.items():
            print(
                f"Estimating specs for footprint '{footprint}' variation '{series}' ({len(group_rows)} parts)...",
                file=sys.stderr,
            )
            try:
                specs_by_group[(footprint, series)] = estimate_specs_for_group(
                    client=client,
                    footprint=footprint,
                    series=series,
                    rows=group_rows,
                )
            except Exception as exc:
                print(
                    f"Warning: Gemini failed for footprint '{footprint}' variation '{series}': {exc}",
                    file=sys.stderr,
                )
                specs_by_group[(footprint, series)] = {
                    "tape_width_mm": 8.0,
                    "feed_pitch_mm": 4.0,
                    "sprocket_pitch_mm": 4.0,
                    "pocket_width_mm": None,
                    "pocket_length_mm": None,
                    "pocket_depth_mm": None,
                    "component_width_mm": None,
                    "component_length_mm": None,
                    "component_height_mm": None,
                    "tape_center_offset_mm": None,
                    "datasheet_urls": [],
                    "confidence": 0.0,
                    "reasoning_short": "fallback defaults",
                }

    xml_chunks: List[str] = ["<feeders>"]
    report_rows: List[Dict[str, object]] = []

    for footprint, variations in grouped.items():
        for series, group_rows in variations.items():
            specs = specs_by_group[(footprint, series)]
            confidence = specs.get("confidence")
            reason = str(specs.get("reasoning_short", "")).replace("--", "-")
            c_l = specs.get("component_length_mm")
            c_w = specs.get("component_width_mm")
            c_h = specs.get("component_height_mm")
            urls = specs.get("datasheet_urls") or []
            first_url = urls[0] if urls else ""

            xml_chunks.append(
                f"  <!-- footprint={footprint} series={series} count={len(group_rows)} conf={confidence} "
                f"size_mm={c_l}x{c_w}x{c_h} source={first_url} reason={reason} -->"
            )

            for row in group_rows:
                part_id = str((row.get("id") or "").strip() or "PART_ID")
                manufacturer_code = (row.get("manufacturer_code") or "").strip() or part_id
                feeder_name_raw = f"AUTO_{footprint}_{series}_{manufacturer_code}"[:80]
                feeder_name = re.sub(r"[^A-Za-z0-9_.-]+", "_", feeder_name_raw)

                feeder_xml = build_feeder_xml(specs, part_id=part_id, feeder_name=feeder_name)
                xml_chunks.extend("  " + line for line in feeder_xml.splitlines())

                report_rows.append(
                    {
                        "id": part_id,
                        "manufacturer_code": manufacturer_code,
                        "smd_footprint": footprint,
                        "series": series,
                        "tape_width_mm": specs.get("tape_width_mm"),
                        "feed_pitch_mm": specs.get("feed_pitch_mm"),
                        "pocket_width_mm": specs.get("pocket_width_mm"),
                        "pocket_length_mm": specs.get("pocket_length_mm"),
                        "pocket_depth_mm": specs.get("pocket_depth_mm"),
                        "component_length_mm": c_l,
                        "component_width_mm": c_w,
                        "component_height_mm": c_h,
                        "sprocket_pitch_mm": specs.get("sprocket_pitch_mm"),
                        "tape_center_offset_mm": specs.get("tape_center_offset_mm"),
                        "confidence": confidence,
                        "datasheet_url": first_url,
                        "reasoning_short": specs.get("reasoning_short"),
                    }
                )

    xml_chunks.append("</feeders>")
    args.output.write_text("\n".join(xml_chunks) + "\n", encoding="utf-8")

    report_fields = [
        "id",
        "manufacturer_code",
        "smd_footprint",
        "series",
        "tape_width_mm",
        "feed_pitch_mm",
        "pocket_width_mm",
        "pocket_length_mm",
        "pocket_depth_mm",
        "component_length_mm",
        "component_width_mm",
        "component_height_mm",
        "sprocket_pitch_mm",
        "tape_center_offset_mm",
        "confidence",
        "datasheet_url",
        "reasoning_short",
    ]
    with args.report.open("w", encoding="utf-8", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=report_fields)
        writer.writeheader()
        writer.writerows(report_rows)

    total_variations = sum(len(variations) for variations in grouped.values())
    print("\nDone.", file=sys.stderr)
    print(f"  Parts processed: {len(rows)}", file=sys.stderr)
    print(f"  Footprint groups processed: {len(grouped)}", file=sys.stderr)
    print(f"  Variations processed: {total_variations}", file=sys.stderr)
    print(f"  Output XML: {args.output}", file=sys.stderr)
    print(f"  Output report: {args.report}", file=sys.stderr)


if __name__ == "__main__":
    main()
