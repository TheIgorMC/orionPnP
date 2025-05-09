# OrionPnP CAD & 3D Models

> This directory contains mechanical design files and 3D models used in the OrionPnP project. These are actively maintained and subject to change as hardware validation continues.

---

## Overview

The OrionPnP mechanical design is built around modularity and printability. It includes mounts, brackets, covers, and pogo-compatible parts used in the feeder system and frame.

The CAD models are designed with:
- **Fusion 360** as the main parametric environment
- STL exports for printing (3MF is gradually being added)
- A BOM covering all fasteners and support components, alongside the list of 3D printed components

---

## Contents

| File/Folder        | Description                                             |
|--------------------|---------------------------------------------------------|
| `/Feeders/`        | Exported 3D files for feeders (M3F + STL)               |
| `/OrionPnP/`       | Exported 3D files for the machine (STL)                 |
| `OrionPnP_Main.f3d`| Main Fusion 360 assembly file (WIP)                     |
| `Hardware BOM.ods` | BOM spreadsheet (for components and 3D printed parts)   |
| `TEMP_BOM.md`      | Temporary Markdown file for quick BOM consultation      |

---

## Notes

- Printed parts **should** be printable with smaller printers (e.g. Prusa Mini)
- STL exports are named by part function for easier identification
- Most parts are designed for **0.6mm nozzle, 0.2mm layer height**

---

## Bill of Materials

The [TEMP_BOM.md](./TEMP_BOM.md) file contains a full breakdown of:
- Printed parts
- Hardware (screws, nuts, spacers, etc.)
- Optional assembly tools and adhesives

---

## Printing Recommendations

- Material: **ABS/ASA** or **PETG** recommended for most parts
- Color: not relevant except for camera mounts (preferable black + white diffuser)
- Infill: 30–50% for structural components
- Supports: Only required for a few overhangs (noted per part)
- Use heat-set inserts for repeated fastening when indicated

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
