# OrionPnP

![Banner for OrionPnP](/assets/banner01.png)

> ⚠️ **DISCLAIMER:** OrionPnP is a prototype under active development. Testing and validation will begin after the first boards are fully assembled. Schematic, PCB, and BOM details may still change. Refer to the [Changelog](../../wiki/Changelog) and [Build Your Own](../../wiki/Build-Your-Own) pages for updates.

---

## What is OrionPnP?

**OrionPnP** is an open-source DIY Pick-and-Place (PnP) machine designed to integrate seamlessly with [OpenPnP](http://openpnp.org).  
The goal is to reliably place at least **0402 components**, while minimizing complexity and user effort — affordably and efficiently.

---

## Why This Project?

Most DIY PnP machines today are either:
- Too expensive
- Too unreliable
- Or both

**OrionPnP** takes a different path: _“minimum effort for maximum reliability.”_  
This means fewer parts, cleaner electronics, and a streamlined build process — optimized for consistent, precise assembly.

As of now, a complete OrionPnP machine (including electronics, mechanics, and **five assembled feeders**) is expected to cost around **800 EUR**, excluding printed parts. This makes it one of the most affordable fully-featured PnP options available.

---

## Why a Custom Mainboard?

Generic motion control boards (like 3D printer boards):
- Often include features irrelevant to PnP (e.g., CAN, multiple extruder support)
- Lack native support for vacuum sensing or multiple solenoids
- Are either too limited, too bloated, or too expensive

**OrionPnP introduces a purpose-built controller**:  
Just the features a PnP machine needs — nothing more.

---

## Key Features

![Render of the mainboard](/assets/RenderBoardV2a.png)

### Motion & Control
- 6x **TMC2226 stepper drivers** with sensorless homing and UART control
- 4x **12V solenoid outputs** (flyback diodes pre-installed)
- 2x **12V LED ring outputs** for bottom and nozzle cameras
- 1x **12V vacuum pump output**, with back EMF protection

### Power & Expandability
- 24V input, 12V and 5V power rails with onboard buck converters
- 2x **2-pin fan headers** (selectable 5V or 12V)
- 1x **4-pin PC fan header** (12V only, non-PWM)
- Automotive-style fuses for 5V and 12V outputs

### Sensing
- 2x **I²C differential pressure sensors** (one per nozzle)
- Each sensor is isolated on its own I²C bus

### USB & Connectivity
- Integrated **USB 3.0 hub**
  - 3x downstream USB 2.0 ports
  - 1x direct stream to the MCU
  - USB 3.0 Type-B upstream port
  - One port is reserved for a **feeder expansion board**

---

## Software Approach

The mainboard runs a **customized Marlin 2.0 build**, chosen for:
- Mature GCODE support
- Existing OpenPnP compatibility
- Flexibility and ease of expansion

Current modifications include:
- Vacuum sensor hooks
- Solenoid and pump control
- LED and GPIO support for pick-and-place automation

More firmware integration details are covered in the [Software Overview](../../wiki/Software-Overview).

---

## Modular Feeder System

OrionPnP includes a distributed architecture of **smart feeders**, connected via RS485.  
Each feeder has its own microcontroller, optical sensors, and a command queue system.  
Feeders are assigned addresses automatically and can be hot-plugged or programmed via a dedicated interface.

See [Feeder Design](../../wiki/Feeder-Design) and [Command Reference](../../wiki/OrionProtocol) for full details.

---

## Host Interface

A **host controller** can manage feeders independently of the mainboard.  
It supports:
- Multi-line RS485 management
- Address assignment via screen + encoder
- OLED feedback and status display
- USB communication to OpenPnP

Typically based on the **STM32F072**, as described in the [Software Overview](../../wiki/Software-Overview).

---

## Project Scope

The core mainboard is in active prototyping.  
Feeder electronics and mechanics are now being finalized.  
CAD files, firmware, schematics, and wiring diagrams will all be published under open-source licenses.

See [Build Your Own](../../wiki/Build-Your-Own) for parts list, tools, and assembly tips.

---

## Testing Status

![Image of the first soldered board](assets/RealBoardV1b.jpg)

### ✅ Power
All rails have been verified. A minor datasheet misread caused an issue on the first version, but has since been corrected in v02.

### ⚠️ USB
Initial USB hub issues were identified:
- Patched for testing
- New hub package selected for v02
- USB 3.0 to 2.0 compatibility still under evaluation

### ✅ MCU & Firmware
- Firmware flashes and executes without issue
- Custom Marlin build compiles cleanly
- GCODE interpreted and processed reliably

### ⏳ Pending Tests
- Solenoid, pump, and LED output loads
- Pressure sensor I²C polling
- Driver thermal testing under motion load
- Full I/O suite with real motors and heatsinks

---

## Contributions Welcome

OrionPnP is a community-first project.  
Feel free to:
- Fork the repo
- Suggest improvements
- Report bugs or build issues

Please check in before large architectural changes — development is moving fast.

---

Stay tuned for updates on feeder programming, mechanical parts, and full assembly instructions.
