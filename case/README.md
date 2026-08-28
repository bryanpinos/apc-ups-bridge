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

## Measure these two before printing the real thing

Everything else is derived from the Feather form factor, but these depend on
which headers you used:

| Parameter | Meaning |
|---|---|
| `stack_h` | Feather PCB top surface to wing PCB bottom surface |
| `above_wing_h` | Wing PCB top to the highest point (the USB-A shell) |

Defaults are 12.0 mm and 10.0 mm. Adjust and re-render.

## Suggested print settings

0.2 mm layers, 3 perimeters, 20% infill, PETG or PLA. No supports. The base
prints floor-down; the lid prints top-down (open side up).
