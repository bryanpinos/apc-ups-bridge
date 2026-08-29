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
headroom     = 2.5;    // air above the USB-A shell. Not just lid clearance: the
                       // face plate needs material ABOVE the USB-A hole, and at
                       // 1.0 it was left with 0.7 mm and broke out of the edge.

under_h      = 4.5;    // clearance below the Feather for solder tails / LiPo lead

// ---------- CONNECTOR END ----------------------------------------------------
// Both USB ports exit the same end. That end is left fully open in the base and
// lid, and closed afterwards by a separate FACE PLATE carrying one exact hole per
// connector. This is the only way to get two clean openings: a connector can only
// pass through an opening that is open in the direction of assembly, and no
// horizontal parting line runs through both a ~7 mm and a ~22 mm connector.
// The face plate also blocks the board from sliding out, so it replaces the
// corner pillars as the retention feature.
conn_end     = 1;      // +1 or -1: which end the connectors face

// Openings, measured from the CAVITY FLOOR. Verify against the real boards.
// MEASURED plug overmold dimensions -- the real constraint. The receptacle is
// irrelevant: what has to pass through is the plug, and its overmold is much
// larger. Measure the actual cables that will live in this case; a chunky
// charging lead can be half again the size of a slim one.
usbc_plug_w  = 11.0;   // measured
usbc_plug_h  = 6.6;
usba_plug_w  = 15.1;   // measured
usba_plug_h  = 7.7;
plug_clear   = 0.8;    // total added, i.e. 0.4 per side

usbc_w       = usbc_plug_w + plug_clear;
usbc_h       = usbc_plug_h + plug_clear;
usba_w       = usba_plug_w + plug_clear;
usba_h       = usba_plug_h + plug_clear;
usba_z       = 22.0;   // centre height: 18.5 (wing PCB top) + 3.5
usba_y       = 0.0;

face_t       = 1.2;    // per layer (flange + plug) -> 2.4 mm total, was 4.0.
                       // Plate thickness directly eats plug insertion depth.
face_fit     = 0.25;   // per-side interference into the end opening

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

// Opens the connector end completely in the base and lid; the face plate closes
// it. The far end wall stays solid.
module conn_slot() {
  h = cav_h + lip_h + 20;
  translate([conn_end * (out_l/2 - wall/2), 0, floor_t + h/2])
    cube([wall + 0.4, out_w + 4, h], center = true);
}

// ---------- FACE PLATE -------------------------------------------------------
// Presses into the open connector end after the stack is in. Built in its own
// frame: y is height above the CAVITY FLOOR, so usbc_z / usba_z drop straight in.
// The outer flange seats against the case end; the plug is an interference fit
// in the cavity mouth. Print it flat, holes facing up.
// Straight bore plus an outward chamfer, so a plug overmold can nose into the
// opening instead of butting against a flat face.
module hole(y, z, w, h) {
  translate([y, z, -1])
    linear_extrude(height = face_t * 4)
      offset(r = 0.6) offset(delta = -0.6) square([w, h], center = true);
  translate([y, z, -0.01])
    linear_extrude(height = 1.2, scale = 1.0)
      offset(r = 0.6) offset(delta = -0.6) square([w + 1.6, h + 1.6], center = true);
}

module face() {
  difference() {
    union() {
      translate([0, cav_h/2, 0])
        rrect(out_w, cav_h + 2*wall, face_t, corner_r * 0.6);
      translate([0, cav_h/2, face_t])
        rrect(cav_w + 2*face_fit, cav_h + 2*face_fit, face_t, corner_r * 0.4);
    }
    hole(usbc_y, usbc_z, usbc_w, usbc_h);
    hole(usba_y, usba_z, usba_w, usba_h);
  }
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
else if (part == "face")    face();
