### Machine architecture

The **OrionPnP** machine is comprised of:
- The machine itself with:
  - One **mainboard** (ATmega2560 based board) that controls the motors and ligths/pumps/solenoids. Driven via GCODE commands over COM.
  - Two **USB cameras**, one on the picking head, one fixed on the build plate. Each has its own independent USB cable.
  - One **Feeder host**, with at least two RS485 buses. One is for the feeders connected to the machine, one is reserved for a programming slot to program new feeders. Communicates via GCODES over COM.
  - One **feeder rail**, a simple PCB strip to connect feeders to the machine. It carries 12V, GND, A and B from the RS485 bus.
- A computer or SBC with [OpenPnP](https://openpnp.org/) installed and decent processing power.
- Feeders of various types for different tape widths (8, 12, 16...). Each feeder has 2 motors connected to a driver to advance the tape and peel the protective film.

The main concept to understand is that the heavy processing is made on the PC/SBC and the other boards need decresing levels of elaboration done. The hierarchy is like this:

- PC/SBC running OpenPnP
  - Feeder host board
    - Feeders
  - Mainboard of the OrionPnP

The method for adding the feeders to the machine is the one in this procedure:
1. Snap a feeder on the programmer area of the host.
2. Set with the simple menu or via custom PC commands the informations on the carried component (tape width, pitch of parts, part ID). These are used by the feeder to know which parts it is feeding and how to properly drive the motors.
3. Unlatch the feeder from the programmer area of the host.
4. Put the feeder on any empty section of the rail.
5. Some sort of *polling command* runs periodically and detects the new feeder.
6. A time is waited to see if more feeders are connected to either the rail OR the programmer (10s?).
7. After the delay passed, the head moves on the feeder section and a custom script runs on the PC/SBC.  

