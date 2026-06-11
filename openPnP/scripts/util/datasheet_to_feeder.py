#!/usr/bin/env python3
"""
datasheet_to_feeder.py

Reads a component datasheet PDF via the Gemini API and outputs an
openPnP-compatible BlindsFeeder XML snippet populated with the
tape/package specs found in the datasheet.

Usage:
    python datasheet_to_feeder.py <datasheet.pdf> [options]

Options:
    --part-id   PART_ID     openPnP part-id to embed in the feeder (default: PART_ID)
    --name      NAME        Feeder name (default: PDF filename stem)
    --output    FILE.xml    Write XML to file instead of stdout

Requires a .env file in the project root (or CWD) with:
    GEMINI_API_KEY=<your key>
"""

import argparse
import json
import os
import sys
import time
import uuid
from pathlib import Path
from xml.dom import minidom
from xml.etree import ElementTree as ET

from dotenv import load_dotenv
import google.generativeai as genai


# ---------------------------------------------------------------------------
# Setup
# ---------------------------------------------------------------------------

load_dotenv()


def configure_gemini() -> None:
    """Configure Gemini from GEMINI_API_KEY in environment/.env."""
    api_key = os.getenv("GEMINI_API_KEY")
    if not api_key:
        print("ERROR: GEMINI_API_KEY not set. Add it to a .env file.", file=sys.stderr)
        sys.exit(1)
    genai.configure(api_key=api_key)

# ---------------------------------------------------------------------------
# Gemini prompt
# ---------------------------------------------------------------------------

EXTRACTION_PROMPT = """\
You are an expert in electronic component packaging and SMT tape-and-reel specifications.

Analyze the provided component datasheet PDF and extract the tape/packaging information.
Return ONLY a raw JSON object (no markdown, no explanation) with these exact keys.
Use null for any value not found in the document.

{
  "tape_width_mm":         <number — tape width, e.g. 8, 12, 16, 24>,
  "feed_pitch_mm":         <number — pocket-to-pocket feed pitch, e.g. 2, 4, 8>,
  "pocket_width_mm":       <number — pocket opening width (A0 dimension per IEC 60286-3)>,
  "pocket_length_mm":      <number — pocket opening length (B0 dimension)>,
  "pocket_depth_mm":       <number — pocket depth (K0 dimension)>,
  "component_width_mm":    <number — component body width>,
  "component_length_mm":   <number — component body length>,
  "component_height_mm":   <number — component maximum height>,
  "sprocket_pitch_mm":     <number — sprocket hole pitch, almost always 4>,
  "tape_center_offset_mm": <number — distance W1 from tape reference edge to pocket centre (IEC 60286-3), typically 3.5 for 8 mm tape>
}
"""


# ---------------------------------------------------------------------------
# Gemini helpers
# ---------------------------------------------------------------------------

def upload_pdf(pdf_path: Path):
    """Upload a PDF to the Gemini Files API and wait for processing."""
    print(f"Uploading '{pdf_path.name}' to Gemini Files API...", file=sys.stderr)
    uploaded = genai.upload_file(str(pdf_path), mime_type="application/pdf")
    while uploaded.state.name == "PROCESSING":
        time.sleep(2)
        uploaded = genai.get_file(uploaded.name)
    if uploaded.state.name == "FAILED":
        raise RuntimeError(f"Gemini file processing failed: {uploaded.state}")
    print("Upload complete.", file=sys.stderr)
    return uploaded


def extract_specs(pdf_path: Path) -> dict:
    """Send the PDF to Gemini and parse the returned tape specs."""
    configure_gemini()
    model = genai.GenerativeModel("gemini-1.5-flash")
    uploaded = upload_pdf(pdf_path)
    try:
        print("Querying Gemini for tape specs...", file=sys.stderr)
        response = model.generate_content([uploaded, EXTRACTION_PROMPT])
        raw = response.text.strip()
        # Strip markdown code fences if the model adds them
        if raw.startswith("```"):
            parts = raw.split("```")
            raw = parts[1]
            if raw.startswith("json"):
                raw = raw[4:]
        specs = json.loads(raw.strip())
    finally:
        genai.delete_file(uploaded.name)
    return specs


# ---------------------------------------------------------------------------
# XML builder
# ---------------------------------------------------------------------------

# Standard W1 offsets (reference-edge to pocket centre) per IEC 60286-3.
# openPnP stores this as a negative value (offset from nominal centre line).
_TAPE_CENTER_OFFSET_DEFAULTS = {
    8:  -1.0625,
    12: -1.0625,
    16: -1.0625,
    24:  0.0,
    32:  0.0,
    44:  0.0,
}


def _sub(parent, tag, attribs):
    """Convenience wrapper for ET.SubElement."""
    return ET.SubElement(parent, tag, attribs)


def build_feeder_xml(specs: dict, part_id: str, feeder_name: str) -> str:
    """Build an openPnP BlindsFeeder XML snippet from extracted tape specs."""
    feeder_id = "FDR" + uuid.uuid4().hex[:16].upper()

    tape_width      = float(specs.get("tape_width_mm")      or 8.0)
    feed_pitch      = float(specs.get("feed_pitch_mm")      or 4.0)
    pocket_width    = float(specs.get("pocket_width_mm")    or 0.0)
    pocket_length   = float(specs.get("pocket_length_mm")   or 0.0)
    sprocket_pitch  = float(specs.get("sprocket_pitch_mm")  or 4.0)

    # pocket-size in openPnP is the larger of the two pocket dimensions
    pocket_size = max(pocket_width, pocket_length)

    # tape-center-offset: use extracted value or fall back to IEC 60286-3 defaults
    raw_offset = specs.get("tape_center_offset_mm")
    if raw_offset is not None:
        # openPnP convention: negative means offset toward reference edge
        tape_center_offset = -abs(float(raw_offset))
    else:
        tape_center_offset = _TAPE_CENTER_OFFSET_DEFAULTS.get(int(tape_width), -1.0625)

    attribs = {
        "class":                   "org.openpnp.machine.reference.feeder.BlindsFeeder",
        "version":                 "1.1",
        "id":                      feeder_id,
        "name":                    feeder_name,
        "enabled":                 "false",
        "part-id":                 part_id,
        "retry-count":             "3",
        "feed-retry-count":        "3",
        "pick-retry-count":        "0",
        "normalize":               "true",
        "vision-enabled":          "true",
        "feed-count":              "0",
        "feeder-no":               "0",
        "feeders-total":           "0",
        "feeder-group-name":       "Default",
        "pocket-count":            "0",
        "first-pocket":            "1",
        "last-pocket":             "0",
        "push-speed":              "0.1",
        "ocr-action":              "None",
        "ocr-font-name":           "Liberation Mono",
        "ocr-font-size-pt":        "7.0",
        "ocr-text-orientation":    "AwayFromTape",
        "fid-loc-max-passes":      "3",
        "fid-loc-tolerance-mm":    "0.5",
        "pocket-pos-tolerance-mm": "0.1",
        "max-label-size-mm":       "10.0",
        "tape-center-offset-mm":   str(tape_center_offset),
        "cover-type":              "BlindsCover",
        "cover-actuation":         "OpenOnJobStart",
    }

    feeder = ET.Element("feeder", attribs)

    _sub(feeder, "location",           {"units": "Millimeters", "x": "0.0", "y": "0.0", "z": "0.0", "rotation": "-90.0"})
    _sub(feeder, "fiducial-1-location", {"units": "Millimeters", "x": "0.0", "y": "0.0", "z": "0.0", "rotation": "0.0"})
    _sub(feeder, "fiducial-2-location", {"units": "Millimeters", "x": "0.0", "y": "0.0", "z": "0.0", "rotation": "0.0"})
    _sub(feeder, "fiducial-3-location", {"units": "Millimeters", "x": "0.0", "y": "0.0", "z": "0.0", "rotation": "0.0"})
    _sub(feeder, "tape-length",         {"value": "0.0",                    "units": "Millimeters"})
    _sub(feeder, "feeder-extent",       {"value": "0.0",                    "units": "Millimeters"})
    _sub(feeder, "pocket-centerline",   {"value": "0.0",                    "units": "Millimeters"})
    _sub(feeder, "pocket-pitch",        {"value": str(feed_pitch),          "units": "Millimeters"})
    _sub(feeder, "pocket-size",         {"value": str(pocket_size),         "units": "Millimeters"})
    _sub(feeder, "sprocket-pitch",      {"value": str(sprocket_pitch),      "units": "Millimeters"})
    _sub(feeder, "edge-open-distance",  {"value": "2.0",                    "units": "Millimeters"})
    _sub(feeder, "edge-closed-distance",{"value": "2.0",                    "units": "Millimeters"})
    _sub(feeder, "push-Z-offset",       {"value": "0.25",                   "units": "Millimeters"})
    _sub(feeder, "ocr-margin",          {"value": "20.0",                   "units": "Millimeters"})

    # Pretty-print
    raw_xml = ET.tostring(feeder, encoding="unicode")
    pretty = minidom.parseString(raw_xml).toprettyxml(indent="   ")
    # Drop the <?xml ...?> declaration line
    lines = pretty.splitlines()
    return "\n".join(line for line in lines[1:] if line.strip())


# ---------------------------------------------------------------------------
# CLI entry point
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="Extract tape specs from a component datasheet PDF and output "
                    "an openPnP BlindsFeeder XML snippet."
    )
    parser.add_argument("pdf", type=Path, help="Path to the datasheet PDF")
    parser.add_argument("--part-id", default="PART_ID",
                        help="openPnP part-id to embed in the feeder element (default: PART_ID)")
    parser.add_argument("--name", default=None,
                        help="Feeder name (default: PDF filename stem)")
    parser.add_argument("--output", type=Path, default=None,
                        help="Write XML to this file instead of stdout")
    args = parser.parse_args()

    if not args.pdf.exists():
        print(f"ERROR: File not found: {args.pdf}", file=sys.stderr)
        sys.exit(1)

    feeder_name = args.name or args.pdf.stem

    specs = extract_specs(args.pdf)

    print("\nExtracted specs:", file=sys.stderr)
    for key, val in specs.items():
        print(f"  {key}: {val}", file=sys.stderr)
    print(file=sys.stderr)

    xml_output = build_feeder_xml(specs, args.part_id, feeder_name)

    if args.output:
        args.output.write_text(xml_output, encoding="utf-8")
        print(f"Written to {args.output}", file=sys.stderr)
    else:
        print(xml_output)


if __name__ == "__main__":
    main()
