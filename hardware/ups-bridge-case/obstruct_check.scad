// Test fixture, not a printable part. Asks: does any LID material remain in the
// connector-end wall zone below z = +2.5? That zone is where the old conn_slot()
// (which started at +floor_t) left a 5 mm bar -- skirt plus end wall -- across
// the bottom of the opening, fouling the face plate. Must render empty.
//   OpenSCAD -o /dev/null -D 'part="none"' obstruct_check.scad
include <case.scad>;
intersection() {
  lid();
  translate([cav_l/2 + 0.1, -out_w/2 - 1, -lip_h - 0.2])
    cube([out_l/2 - cav_l/2 + 1.0, out_w + 2, lip_h + 2.7]);
}
