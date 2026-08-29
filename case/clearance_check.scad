// Test fixture, not a printable part. Places the lid where it actually sits and
// intersects it against real geometry. Any facets = interference.
//   OpenSCAD -o out.stl -D 'part="none"' -D 'check="realplug"' clearance_check.scad
// NOTE: an envelope that exactly matches a hole yields a ZERO-VOLUME artifact
// from coincident faces -- always check the facet count, not just whether an STL
// got written.
include <case.scad>;
check = "realplug";

module plug_envelope(c, w, h) {
  z = floor_t + under_h + c;
  translate([conn_end * (out_l/2 + 6), 0, z])
    rotate([0, 90, 0]) linear_extrude(height = 24, center = true) rr2(h, w, 0.6);
}
module board_volume() {
  translate([conn_end * (cav_l/2 - pcb_l/2), 0,
             floor_t + under_h + (pcb_t + stack_h + pcb_t + above_wing_h)/2])
    cube([pcb_l, pcb_w, pcb_t + stack_h + pcb_t + above_wing_h], center = true);
}
if (check == "realplug")            // actual plug overmolds through the assembly
  intersection() {
    union() { base(); translate([0,0,base_h]) lid(); }
    union() { plug_envelope(usbc_c, usbc_plug_w, usbc_plug_h);
              plug_envelope(usba_c, usba_plug_w, usba_plug_h); }
  }
if (check == "fit")                 // lid skirt vs tray step
  intersection() { base(); translate([0,0,base_h]) lid(); }
if (check == "board")               // lid vs the installed stack
  intersection() { translate([0,0,base_h]) lid(); board_volume(); }
