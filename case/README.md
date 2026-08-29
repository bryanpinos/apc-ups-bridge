# Enclosure

Parametric OpenSCAD case for an Adafruit ESP32-S3 Feather with a USB Host
FeatherWing stacked on top. Two parts, both print flat, no supports.

```bash
OPENSCAD=/Applications/OpenSCAD.app/Contents/MacOS/OpenSCAD   # macOS
$OPENSCAD -o stl/ups-bridge-base.stl    -D 'part="base"'    case.scad
$OPENSCAD -o stl/ups-bridge-lid.stl     -D 'part="lid"'     case.scad
$OPENSCAD -o stl/ups-bridge-fittest.stl -D 'part="fittest"' case.scad
```

Outer footprint is about **57 x 28 mm**; assembled height depends on your stack.

## Three parts: base, lid, face plate

Both USB ports exit the same end. That end is left **fully open** in the base and
lid, and closed afterwards by a **face plate** carrying one exact hole per
connector. The far end is a solid wall.

This is the only arrangement that gives two clean openings. A connector can only
pass through an opening that is open in the direction of assembly, so a
base/lid parting line would have to run through *both* connectors — impossible
when one sits at ~7 mm and the other at ~22 mm. Whichever you do not split
through becomes an ugly full-height slot.

The face plate also **retains the board**: the stack drops into the base, the lid
goes on, then the plate presses into the open end and blocks the only escape
route. No screws or clips.

Assembly order: stack into base → lid on → face plate pressed in.

Set `conn_end` to `+1` or `-1` to choose which end the connectors face, and
`face_fit` for the press-fit interference (0.25 mm per side by default).

### Print the face plate first

It is a two-minute print and it is the only part whose fit depends on connector
positions rather than board outline. Offer it up to the assembled stack before
committing filament to the base and lid. Adjust `usbc_*` / `usba_*` if the holes
do not line up — `usbc_z` and `usba_z` are heights above the **cavity floor**,
and `usbc_y` / `usba_y` are lateral offsets from the centreline.

## Print the fit test first

`ups-bridge-fittest.stl` is a ~10 mm slice of the base — a few minutes of
filament. It proves the cavity width and the corner posts before you commit to
the full part.

It looks like two side rails with a small 90-degree tab at each end: those tabs
are the four corner posts the Feather rests on, and the board drops *between*
the rails and sits *on* the posts.

**The slice must be taller than the posts.** `fit_h` is derived as
`floor_t + under_h + pcb_t + 2`, which leaves 2 mm of wall standing above the
seated board. An earlier 6 mm version sliced the posts off level with the wall
tops, so a board laid in rested on top of everything and the width clearance
could not be tested at all.

## Dimensions

Measured on a real stack using plain male/female headers:

| Measurement | Value |
|---|---|
| Feather PCB bottom to wing PCB top | **14.0 mm** |
| Feather PCB bottom to top of USB-A shell | **21.0 mm** |

which give `stack_h = 10.8` (14.0 less two 1.6 mm PCBs) and `above_wing_h = 7.0`
(21.0 less 14.0). Plus 4.5 mm of clearance under the Feather for solder tails and
the LiPo lead, and 1.0 mm of headroom so the lid never presses on the USB-A shell:

```
cavity 26.5 mm tall   base 14.0 + lid 16.5   footprint 57.2 x 28.2 mm
```

**Re-measure if you use stacking headers** — they are taller than plain ones and
`stack_h` will grow.

## Suggested print settings

0.2 mm layers, 3 perimeters, 20% infill, PETG or PLA. No supports. The base
prints floor-down; the lid prints top-down (open side up).
