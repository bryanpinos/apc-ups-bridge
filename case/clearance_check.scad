// Test fixture, not a printable part. Places the lid where it actually sits and
// intersects it with the swept envelope of each plug, plus the board's own
// volume. Any output at all is an interference. Should render EMPTY.
//   OpenSCAD -o /dev/null -D 'part="none"' -D 'check="plug"' clearance_check.scad
include <case.scad>;
check = "plug";

module plug_envelope(c, w, h) {
  z = floor_t + under_h + c;
  translate([conn_end * (out_l/2 + 6), 0, z])
    rotate([0, 90, 0])
      linear_extrude(height = 24, center = true) rr2(h, w, 0.6);
}

module board_volume() {          // whole stack, as installed
  translate([conn_end * (cav_l/2 - pcb_l/2), 0,
             floor_t + under_h + (pcb_t + stack_h + pcb_t + above_wing_h)/2])
    cube([pcb_l, pcb_w, pcb_t + stack_h + pcb_t + above_wing_h], center = true);
}

intersection() {
  translate([0, 0, base_h]) lid();
  union() {
    if (check == "plug")  { plug_envelope(usbc_c, usbc_w, usbc_h);
                            plug_envelope(usba_c, usba_w, usba_h); }
    if (check == "board") board_volume();
  }
}

// base vs plug envelopes -- confirms the holes are genuinely through and placed
// right; and base vs lid -- confirms the spigot actually fits the cavity.
if (check == "basehole")
  intersection() { base(); union() { plug_envelope(usbc_c, usbc_w, usbc_h);
                                     plug_envelope(usba_c, usba_w, usba_h); } }
if (check == "fit")
  intersection() { base(); translate([0, 0, base_h]) lid(); }

// The meaningful one: the actual PLUG overmold, not the hole outline, swept
// through the wall. Coincident faces make an outline-sized envelope render as a
// zero-volume artifact; a real plug is smaller and gives a clean answer.
if (check == "realplug")
  intersection() {
    union() { base(); translate([0, 0, base_h]) lid(); }
    union() { plug_envelope(usbc_c, usbc_plug_w, usbc_plug_h);
              plug_envelope(usba_c, usba_plug_w, usba_plug_h); }
  }
