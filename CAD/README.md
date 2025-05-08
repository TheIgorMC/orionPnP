# OrionPnP CAD & 3D Models

> This directory contains mechanical design files and 3D models used in the OrionPnP project. These are actively maintained and subject to change as hardware validation continues.

---

## Overview

The OrionPnP mechanical design is built around modularity and printability. It includes mounts, brackets, covers, and pogo-compatible parts used in the feeder system and frame.

The CAD models are designed with:
- **Fusion 360** as the main parametric environment
- STL exports for printing
- A BOM covering all fasteners and support components

---

## Contents

| File/Folder        | Description                                             |
|--------------------|---------------------------------------------------------|
| `/STLs/`           | Exported 3D printable parts (mounts, brackets, etc.)    |
| `/STEP/`           | Neutral-format STEP files for external CAD use          |
| `OrionPnP_Main.f3d`| Main Fusion 360 assembly file (WIP)                     |
| `OrionPnP_BOM.md`  | Human-readable Bill of Materials                        |
| `OrionPnP_BOM.csv` | BOM spreadsheet (for sourcing or BOM tools)             |

---

## Notes

- The current design targets a compact V-slot or Prusa-style frame
- Feeder and programmer jigs are sized for standard feeder PCBs
- STL exports are named by part function for easier identification
- Most parts are designed for **0.4mm nozzle, 0.2mm layer height**

---

## Bill of Materials

The [OrionPnP_BOM.md](./OrionPnP_BOM.md) file contains a full breakdown of:
- Printed parts
- Hardware (screws, nuts, spacers, etc.)
- Optional assembly tools and adhesives

A CSV version is also available for importing into sourcing platforms.

---

## Printing Recommendations

- Material: **PETG** or **PLA+** recommended for most parts
- Infill: 30–50% for structural components
- Supports: Only required for a few overhangs (noted per part)
- Use heat-set inserts for repeated fastening when possible

---

## License

All CAD files are released under the same open-source license as the rest of the project. You are free to modify and redistribute them, but attribution is appreciated.

---

## Related Pages

- [Build Your Own](../../wiki/Build-Your-Own)
- [Feeder Design](../../wiki/Feeder-Design)
- [Hardware Overview](../../wiki/Hardware-Details)

---

Stay tuned as we finalize the mechanical design and publish full step-by-step assembly guides.
