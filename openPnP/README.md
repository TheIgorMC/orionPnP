# openPnP Configuration

This folder contains the openPnP configuration and supporting files for the Orion PnP machine.

## Files

| File | Description |
|------|-------------|
| `machine.xml` | Main openPnP machine configuration (axes, drivers, cameras, nozzles, feeders) |
| `packages.xml` | Component package definitions (footprints) |
| `parts.xml` | Parts library with component-to-package mappings |
| `vision-settings.xml` | Vision pipeline settings for part and fiducial detection |

## Scripts

Example scripts are provided in `scripts/Examples/` in both JavaScript and Python.

### JavaScript (`scripts/Examples/JavaScript/`)
- `Hello_World.js` — Basic scripting example
- `Move_Machine.js` — Machine movement control
- `Pipeline.js` — Vision pipeline scripting
- `Reset_Strip_Feeders.js` — Reset all strip feeders
- `QrCodeXout.js` — QR code utility
- `Call_Java.js` — Calling Java methods from scripts
- `Print_Scripting_Info.js` — Print available scripting objects
- `Utility.js` — General utility functions

### Python (`scripts/Examples/Python/`)
- `move_machine.py` — Machine movement control
- `print_nozzle_info.py` — Print nozzle status/info
- `print_hallo_openpnp.py` — Basic scripting example
- `call_java.py` — Calling Java methods from scripts
- `print_methods_vars.py` — Inspect available methods and variables
- `print_scripting_info.py` — Print available scripting objects
- `use_module.py` — Example of importing Python modules
- `utility.py` — General utility functions

The `scripts/Events/` folder is reserved for event-triggered scripts (e.g., job start/stop hooks).

### Utilities (`scripts/util/`)

| File | Description |
|------|-------------|
| `datasheet_to_feeder.py` | Reads a component datasheet PDF via the Gemini API and outputs an openPnP `BlindsFeeder` XML snippet with the extracted tape specs |
| `batch_parts_to_feeders.py` | Reads a CSV list of parts, groups by footprint then variation series, uses Gemini with web search to estimate tape specs and component size, and outputs bulk feeder XML + review report |
| `requirements.txt` | Python dependencies for the util scripts |
| `.env.example` | Template for the required `GEMINI_API_KEY` environment variable |

#### `datasheet_to_feeder.py` usage

```bash
# Install dependencies
pip install -r scripts/util/requirements.txt

# Copy and fill in your Gemini API key
cp scripts/util/.env.example .env

# Run against a datasheet PDF
python scripts/util/datasheet_to_feeder.py path/to/component.pdf \
    --part-id "C_0402_100NF" \
    --name "Feeder_0402_100nF" \
    --output feeder_0402.xml
```

The script uploads the PDF to the Gemini Files API, extracts the following fields, and writes a ready-to-import `<feeder>` XML element:

| Field extracted | openPnP attribute |
|---|---|
| Tape width | `tape-center-offset-mm` (via IEC 60286-3 lookup) |
| Feed pitch (A dimension) | `pocket-pitch` |
| Pocket size (A0/B0) | `pocket-size` |
| Sprocket pitch | `sprocket-pitch` |

The output XML can be pasted directly inside the `<feeders>` section of `machine.xml`.

#### `batch_parts_to_feeders.py` usage (CSV automation)

Use this when you already have a CSV with many part numbers (e.g. `scripts/util/exported.csv`).

```bash
# Install dependencies
pip install -r scripts/util/requirements.txt

# Run batch generation from a CSV export
python scripts/util/batch_parts_to_feeders.py scripts/util/exported.csv \
    --output scripts/util/feeders_generated.xml \
    --report scripts/util/feeders_generated_report.csv
```

What it does:

1. Reads `id`, `manufacturer_code`, and `smd_footprint` from the CSV.
2. Groups parts by `smd_footprint` first, then by inferred series/variation.
3. Calls search-enabled Gemini (`gemini-2.5-flash` + Google Search tool) per variation group to infer tape specs and component size.
4. Generates one `<feeder>` per CSV row with the inferred variation specs.
5. Writes a review CSV with footprint, variation, confidence, component dimensions, and datasheet URL.

The XML output contains a `<feeders>` root with comments showing footprint, variation, confidence, dimensions, and a datasheet source link for each block.