# Setup cycle (verify rails, pick window, pick height)

Starts after the machine is homed (a message with **Start setup cycle** / **Later**), after the automatic scan added
feeders (same message), from the Bus Manager (**Setup cycle** on a rail) and from the Orion menu.

Per rail, feeder after feeder in X order:

1. **Units**: a feeder is created for every unit that has none.
2. **Fiducial**: the camera goes to the saved X of the feeder's large fiducial. A small shift is saved.
3. **Fiber**: the feeder's fiber is lit at the fiber spot (offset from the fiducial, set per rail). If the spot does not
   light, the unit there is not the feeder (replaced, or gone): the feeder counts as missing at that place, the fiber /
   tape-movement search runs for it, and the fiducial that was rejected is not given back to it.
4. **Pick window**: set per rail in *Rail vision scan*: the machine coordinate along the tape where the cover tape is
   peeled (**= camera X / Y** takes it from the camera), a margin, and the side the pockets are on. The pocket centre is
   brought to `peel edge + side * (margin + pocket length / 2)`. The pocket length is the feeder's pocket setting, else
   the part's package body (the longer side).
5. **Vision** for parts up to the limits in *Connection and monitor* (default 3 x 4 mm): the camera looks at the pocket
   nearest the target, the tape is jogged until its centre is within 0.10 mm (adjustable), the unit's tape zero is set
   there and the pick point and pocket reference are saved. If the pocket was already within the tolerance, nothing is
   changed. The jog direction is learned. No pocket visible: the tape is searched along its length.
6. **Semi blind** for larger parts (and parts of unknown size): the pick point is set by the offset only; the camera
   checks that a pocket is roughly there (reported when further than 1.5 mm off) and nothing else is adjusted.

**Pick height**: every new feeder starts with the rail's default Z (*Start pick Z at*). At the first feed of each
session, for every feeder (also old ones), the nozzle is lowered in steps over the pick spot with the vacuum on until
the vacuum sensor (chosen from a list of actuators; threshold and direction set in *Connection and monitor*, with a
**Read now** button) shows a seal; the pick height is that Z minus the extra depth (default 0.10 mm). Z is assumed to
decrease going down. A replaced feeder (new serial) forgets its Z.

Tests (simulated, no hardware): window arithmetic, vision / semi blind choice, jog loop incl. wrong first direction,
nothing-to-do case and search, Z probe incl. no seal / already sealed / reading direction, rail scan with a replaced
feeder. The camera, jog and vacuum steps on the real machine are not covered by tests.
