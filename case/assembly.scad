// Assembly / exploded view. Not a printable part -- this exists to show how the
// face plate relates to the base and lid, which is not obvious from the parts.
//   OpenSCAD -o out.png -D explode=1 assembly.scad
include <case.scad>;          // renders base() at the origin
explode = 0;

translate([0, 0, base_h + explode * 22]) lid();

translate([out_l/2 + face_t + explode * 34, 0, floor_t])
  rotate([90, 0, -90]) face();
