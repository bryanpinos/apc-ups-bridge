// =============================================================================
//  UPS Bridge enclosure
//  Adafruit ESP32-S3 Feather + Adafruit USB Host FeatherWing (MAX3421E)
//  Companion to github.com/bryanpinos/apc-ups-bridge
//
//  Two parts, both print flat with no supports.
//
//  DESIGN NOTE: both USB ports exit the SAME end. That end gets a central
//  full-height slot for the connectors but KEEPS its corner pillars; the far end
//  is a solid wall. The stack drops in from above and is then captured
//  lengthwise -- pillars one way, solid wall the other -- with no screw needed.
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

// Measured on a real Feather + USB Host FeatherWing stack with plain male/
// female headers. Re-measure if you use stacking headers, which are taller.
stack_h      = 10.8;   // MEASURED: 14.0 overall (Feather PCB bottom -> wing PCB top)
                       //           minus two 1.6 mm PCBs = 10.8
above_wing_h = 7.0;    // MEASURED: 21.0 (Feather PCB bottom -> USB-A top)
                       //           minus 14.0 (to wing PCB top) = 7.0
headroom     = 1.0;    // air above the USB-A shell so the lid never presses on it

under_h      = 4.5;    // clearance below the Feather for solder tails / LiPo lead

// ---------- CONNECTOR END ----------------------------------------------------
// Both USB ports (Feather USB-C, low; wing USB-A, high) are on the same end.
// conn_slot_w is the central full-height opening they pass through. What is left
// either side becomes a corner pillar, and those pillars are what stop the board
// sliding out. Widen the slot only as far as the connectors actually need.
conn_end     = 1;      // +1 or -1: which end the connectors face
conn_slot_w  = 17.0;   // central opening width (USB-A is ~13.2 mm)

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
cav_h     = under_h + pcb_t + stack_h + pcb_t + above_wing_h + headroom;
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

// Cuts the central connector slot in the connector end only. The far end wall
// stays solid, and the material either side of the slot stays as corner pillars.
module conn_slot() {
  h = cav_h + lip_h + 20;
  translate([conn_end * (out_l/2 - wall/2), 0, floor_t + h/2])
    cube([wall + 0.4, conn_slot_w, h], center = true);
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
    conn_slot();
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
    conn_slot();
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
// Must be TALLER than the corner posts (floor_t + under_h = 6.5 mm), otherwise
// the posts get sliced level with the wall tops and a board laid in rests on top
// of everything -- which tests nothing. fit_h leaves real wall above the board so
// the width clearance is actually checkable.
fit_h = floor_t + under_h + pcb_t + 2.0;

module fittest() {
  intersection() {
    base();
    translate([-(out_l+10)/2, -(out_w+10)/2, 0]) cube([out_l+10, out_w+10, fit_h]);
  }
}

if      (part == "base")    base();
else if (part == "lid")     lid();
else if (part == "fittest") fittest();
