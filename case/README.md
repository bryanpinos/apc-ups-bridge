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

## Both short ends are open — deliberately

The Feather's USB-C and the wing's USB-A sit at **opposite ends and very
different heights**, so no single horizontal parting line lets both connectors
pass through a wall. Rather than guess at connector positions, both ends are
left open: the board stack slides in, the connectors stand proud, and airflow
improves. Set `closed_ends = true` if you have measured your own boards and
want to add cutouts.

## Print the fit test first

`ups-bridge-fittest.stl` is a 6 mm slice of the base — a few minutes of
filament. It proves the cavity width and length and the corner posts before you
commit to the full part.

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
