// =============================================================================
//  UPS Bridge enclosure
//  Adafruit ESP32-S3 Feather + Adafruit USB Host FeatherWing (MAX3421E)
//  Companion to github.com/bryanpinos/apc-ups-bridge
//
//  Two parts, both print flat with no supports.
//
//  DESIGN NOTE: both short ends are OPEN. The Feather's USB-C and the wing's
//  USB-A sit at opposite ends and at very different heights, so no single
//  horizontal parting line lets both connectors pass through a wall. Open ends
//  remove the problem entirely, let the stack slide in, and help airflow.
//    OpenSCAD -o out.stl -D 'part="base"'    case.scad
//    OpenSCAD -o out.stl -D 'part="lid"'     case.scad
//    OpenSCAD -o out.stl -D 'part="fittest"' case.scad   // 6 mm test slice
// =============================================================================

// ---------- BOARD DIMENSIONS -------------------------------------------------
// Feather spec is 50.8 x 22.86 mm; the Host wing is 52.0 x 22.8 x 8.8 mm.
// Cavity is sized to the larger footprint so both fit.
pcb_l        = 52.0;
pcb_w        = 23.0;
pcb_t        = 1.6;

// >>> MEASURE THESE TWO ON THE ACTUAL STACK <<<
stack_h      = 12.0;   // Feather PCB top surface -> wing PCB bottom surface
above_wing_h = 10.0;   // wing PCB top -> highest point (the USB-A shell)

under_h      = 4.5;    // clearance below the Feather for solder tails / LiPo lead

// ---------- ENDS -------------------------------------------------------------
// Both short ends open by default; the stack slides in and the connectors are
// simply exposed. Set closed_ends=true only if you have measured your own
// connector positions and want to add cutouts.
closed_ends  = false;
end_wall     = 2.0;    // only used when closed_ends = true

// ---------- SHELL ------------------------------------------------------------
wall         = 2.0;
floor_t      = 2.0;
gap          = 0.6;    // per-side clearance around the PCB
corner_r     = 2.5;
lip_h        = 3.0;
lip_t        = 1.2;
vent_slots   = true;

part = "base";
$fn = 48;

// ---------- DERIVED ----------------------------------------------------------
cav_l     = pcb_l + 2*gap;
cav_w     = pcb_w + 2*gap;
cav_h     = under_h + pcb_t + stack_h + pcb_t + above_wing_h;
out_l     = cav_l + 2*wall;
out_w     = cav_w + 2*wall;
feather_z = under_h;                       // PCB underside above inner floor
wing_z    = under_h + pcb_t + stack_h;
base_h    = floor_t + under_h + pcb_t + stack_h * 0.55;
lid_h     = floor_t + (cav_h - (base_h - floor_t));

// Rounded rectangular prism, centred in X/Y, sitting on z=0.
module rrect(l, w, h, r) {
  linear_extrude(height = h)
    offset(r = r) offset(delta = -r) square([l, w], center = true);
}

// Corner posts the Feather rests on.
module posts() {
  p = 3.2;
  for (x = [-1, 1], y = [-1, 1])
    translate([x * (cav_l/2 - p/2), y * (cav_w/2 - p/2), floor_t])
      translate([-p/2, -p/2, 0])
        cube([p, p, under_h]);
}

// Removes both short end walls so the stack can slide in and the connectors
// stand proud. Cut solids are one piece per end, kept disjoint.
module open_ends() {
  // The end wall occupies x = cav_l/2 .. out_l/2, so centre the cutter on
  // out_l/2 - wall/2 and make it exactly one wall thick. Full width (out_w+4)
  // so the rounded corners go too. Starts above the floor: the floor runs the
  // whole length and the boards rest on it.
  h = cav_h + lip_h + 20;
  for (sx = [-1, 1])
    translate([sx * (out_l/2 - wall/2), 0, floor_t + h/2])
      cube([wall + 0.4, out_w + 4, h], center = true);
}

module floor_vents() {
  if (vent_slots)
    for (i = [-3 : 3])
      translate([i * 6, 0, -1])
        cube([2.2, cav_w * 0.5, floor_t + 2], center = false);
}

module floor_vents_c() {
  if (vent_slots)
    for (i = [-3 : 3])
      translate([i * 6, 0, floor_t/2])
        cube([2.2, cav_w * 0.5, floor_t * 3], center = true);
}

// ---------- BASE -------------------------------------------------------------
module base() {
  difference() {
    rrect(out_l, out_w, base_h, corner_r);
    translate([0, 0, floor_t]) rrect(cav_l, cav_w, base_h, corner_r * 0.6);
    if (!closed_ends) open_ends();
    floor_vents_c();
    // rebate at the rim for the lid lip
    translate([0, 0, base_h - lip_h])
      difference() {
        rrect(out_l + 2, out_w + 2, lip_h + 2, corner_r);
        rrect(cav_l + 2*lip_t, cav_w + 2*lip_t, lip_h + 4, corner_r * 0.6);
      }
  }
  posts();
}

// ---------- LID --------------------------------------------------------------
// A box open at the bottom, plus a thinner skirt that drops into the rebate
// cut around the base rim, giving a stepped joint with no visible gap.
module lid() {
  difference() {
    union() {
      rrect(out_l, out_w, lid_h, corner_r);
      translate([0, 0, -lip_h]) rrect(out_l, out_w, lip_h, corner_r);
    }
    // hollow the body, leaving floor_t as the top plate
    translate([0, 0, -0.01])
      rrect(cav_l, cav_w, lid_h - floor_t + 0.01, corner_r * 0.6);
    // skirt bore, sized to clear the step left on the base rim
    translate([0, 0, -lip_h - 0.01])
      rrect(cav_l + 2*lip_t + 0.3, cav_w + 2*lip_t + 0.3, lip_h + 0.02, corner_r * 0.6);
    if (!closed_ends) open_ends();
    top_vents();
  }
}

module top_vents() {
  if (vent_slots)
    for (i = [-3 : 3])
      translate([i * 6, 0, lid_h - floor_t/2])
        cube([2.2, cav_w * 0.5, floor_t * 3], center = true);
}


// ---------- FIT TEST ---------------------------------------------------------
// A 6 mm slice of the base: proves cavity size, corner posts and the USB-C
// cutout height in a few minutes of printing.
module fittest() {
  intersection() {
    base();
    translate([-(out_l+10)/2, -(out_w+10)/2, 0]) cube([out_l+10, out_w+10, 6]);
  }
}

if      (part == "base")    base();
else if (part == "lid")     lid();
else if (part == "fittest") fittest();
