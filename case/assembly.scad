// Assembly / exploded view. Not a printable part.
//   OpenSCAD -o out.png -D explode=1 assembly.scad
include <case.scad>;          // renders base() at the origin
explode = 0;
translate([0, 0, base_h + explode * 20]) lid();
