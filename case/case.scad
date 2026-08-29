// =============================================================================
//  UPS Bridge enclosure
//  Adafruit ESP32-S3 Feather + Adafruit USB Host FeatherWing (MAX3421E)
//  Companion to github.com/bryanpinos/apc-ups-bridge
//
//  Two parts, both print flat with no supports.
//
//  DESIGN NOTE: both USB ports exit the SAME end, and that end wall is SOLID,
//  carrying one hole per connector. The base is therefore full height and the
//  lid is only a cap.
//
//  This replaces an earlier three-part scheme (short base + tall lid + a separate
//  face plate pressed into an open end). That could not work: the slot that
//  opened the end removed the full width of it, so the face plate had nothing
//  left to register against, and the corner posts sat directly in its path.
//
//  The Feather goes in from above and is SCREWED to four bosses, which fixes its
//  position relative to the connector openings instead of leaving it to gravity.
//  Fit the Feather and its screws first, then plug the wing on top -- the wing
//  covers the Feather's mounting holes, and the 10.8 mm gap between the boards
//  clears the screw heads easily.
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
above_wing_h = 7.4;    // MEASURED: 21.4 (Feather PCB bottom -> USB-A top)
                       //           minus 14.0 (to wing PCB top) = 7.4
headroom     = 3.0;    // air above the USB-A shell. Sets how much wall is left
                       // above the USB-A hole, whose top edge lands at 28.65 from
                       // the outer floor. Below ~2.3 the hole breaks out of the
                       // top of the base entirely.

under_h      = 4.5;    // clearance below the Feather for solder tails / LiPo lead

// ---------- CONNECTOR END ----------------------------------------------------
// Both USB ports exit the same end, through a solid 2 mm wall. The plug overmold
// -- not the receptacle -- is what has to pass through: the connectors sit ~2 mm
// behind the outer face, and a USB-A plug only has ~12 mm of shell against ~12 mm
// of insertion depth, so if the overmold could not enter the hole the plug would
// stop 2 mm short of seating. Size these to the actual cables that will live in
// this case; a chunky charging lead can be half again the size of a slim one.
conn_end     = 1;      // +1 or -1: which end the connectors face

usbc_plug_w  = 11.0;   // measured
usbc_plug_h  = 6.6;
usba_plug_w  = 15.1;   // measured
usba_plug_h  = 7.7;
plug_clear   = 1.2;    // total added, i.e. 0.6 per side. Deliberately loose: the
                       // centre heights below close to about +/-0.5 mm, and a
                       // plug that rattles slightly still mates -- one that is
                       // 0.4 mm proud of the hole does not.

// Connector CENTRE heights, from the FEATHER PCB BOTTOM -- the same datum as the
// 14.0 and 21.4 stack measurements. Closed out from three caliper readings:
//   wing PCB top          = 14.0   -> USB-A shell bottom (it sits flush)
//   USB-A top             = 21.4   -> USB-A spans 14.0..21.4, centre 17.7
//   clear gap between the = 10.2   -> USB-C top = 14.0 - 10.2 = 3.8, and it sits
//   two connector shells              on the Feather PCB top (1.6), so 1.6..3.8,
//                                     centre 2.7
// Note the USB-C body works out only 2.2 mm proud of the PCB, which means it is
// a mid-mount part sitting in a board cutout, not a 3.2 mm top-mount one. The
// three measurements close on each other, so trust them over the datasheet.
usbc_c       = 2.7;
usba_c       = 17.7;

usbc_w       = usbc_plug_w + plug_clear;
usbc_h       = usbc_plug_h + plug_clear;
usba_w       = usba_plug_w + plug_clear;
usba_h       = usba_plug_h + plug_clear;

// An undefined variable here does NOT fail: OpenSCAD warns, discards the whole
// transform, and draws the feature at the origin. That shipped a face plate with
// its USB-C opening 3.2 mm low. Fail loudly instead.
assert(is_num(usbc_c) && is_num(usba_c) && is_num(usbc_w) && is_num(usba_h),
       "connector opening dimensions must all be numbers");

// ---------- SHELL ------------------------------------------------------------
wall         = 2.0;
floor_t      = 2.0;
gap          = 0.6;    // per-side clearance around the PCB
corner_r     = 2.5;
// The lid locates with a SPIGOT that drops into the top of the cavity, not a
// skirt around the outside. An outer skirt has to live in a rebate cut into the
// base's outer wall -- the same wall the connector holes are in -- and at this
// stack height the rebate collided with the top of the USB-A opening, putting
// the lid seam across the port. An inner spigot shares nothing with that wall.
// It hangs above the board rather than reaching down beside it: the cavity is
// only 0.6 mm wider than the PCB, so there is no room alongside, but there is
// clear air above the USB-A shell.
spigot_h     = 2.0;    // engagement depth; USB-A top clears it by 1.0 mm
spigot_t     = 1.2;    // ring wall
spigot_clear = 0.3;    // per side, spigot to cavity wall
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
// The base is now the whole box: it has to be tall enough to contain the USB-A
// hole in its own end wall. The lid is just a cap over the top.
base_h    = floor_t + cav_h;
lid_h     = floor_t;

// Rounded rectangular prism, centred in X/Y, sitting on z=0.
module rrect(l, w, h, r) {
  linear_extrude(height = h)
    offset(r = r) offset(delta = -r) square([l, w], center = true);
}

// ---------- SCREW BOSSES ------------------------------------------------------
// Four bosses aligned to the FEATHER's mounting holes (the wing stacks above and
// is not fastened). Bosses replace the plain corner posts the board used to rest
// on: screwing it down also pins it against the connector openings, so the plug
// heights stay true.
//
// Both measured: 3.5 mm from the board edge to the FAR side of the mounting
// hole (the same to the long and the short edge), and 2.5 mm hole diameter. The
// hole CENTRE is therefore 3.5 - 2.5/2 = 2.25 mm in from each edge.
feather_l    = 50.8;   // Feather spec, NOT the 52.0 wing footprint the cavity uses
feather_w    = 22.86;
hole_edge    = 3.5;    // MEASURED: board edge -> far side of the mounting hole
board_hole_d = 2.5;    // MEASURED
hole_inset   = hole_edge - board_hole_d/2;

boss_od      = 5.0;
screw_pilot  = 1.7;    // M2 self-tapping into PLA. Drill out to 2.05 for a
                       // machine screw and a nut, or 1.5 for a tighter bite.

// The Feather sits hard against the connector end, so its openings line up: its
// end edge is at the cavity end and the bosses are referenced back from there.
module posts() {
  for (px = [conn_end * (cav_l/2 - hole_inset),
             conn_end * (cav_l/2 - feather_l + hole_inset)],
       py = [-1, 1])
    translate([px, py * (feather_w/2 - hole_inset), floor_t])
      difference() {
        cylinder(h = under_h, d = boss_od);
        translate([0, 0, -0.5]) cylinder(h = under_h + 1, d = screw_pilot);
      }
}

// ---------- CONNECTOR OPENINGS ------------------------------------------------
// Cut through the SOLID end wall of the base. Heights come from usbc_c / usba_c,
// which are measured from the Feather PCB bottom -- so the datum chain here is
// outer floor -> floor_t -> under_h (post height) -> the measured centre.
//
// Each opening is a straight bore plus a shallow relief on the OUTSIDE face, so
// a plug overmold noses in rather than butting against a flat wall.
module rr2(a, b, r) { offset(r = r) offset(delta = -r) square([a, b], center = true); }

module conn_hole(c, w, h) {
  z = floor_t + under_h + c;
  // straight bore, right through the wall
  translate([conn_end * (out_l/2 - wall/2), 0, z])
    rotate([0, 90, 0])
      linear_extrude(height = wall + 2, center = true) rr2(h, w, 0.6);
  // outward relief, 1.2 mm deep, leaving 0.8 mm of full-thickness wall
  translate([conn_end * (out_l/2 - 0.4), 0, z])
    rotate([0, 90, 0])
      linear_extrude(height = 1.6, center = true) rr2(h + 1.6, w + 1.6, 0.6);
}

module conn_holes() {
  conn_hole(usbc_c, usbc_w, usbc_h);
  conn_hole(usba_c, usba_w, usba_h);
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
    conn_holes();
    floor_vents_c();
  }
  posts();
}

// ---------- LID --------------------------------------------------------------
// A flat plate the size of the case, plus a spigot ring underneath that drops
// into the top of the cavity and locates it. Outside, the joint is a clean butt
// line on the rim. No opening of any kind -- the connectors are entirely the
// base's problem now. Print it PLATE DOWN: the other way up, the ring prints
// first and the plate becomes a 24 mm bridge.
module lid() {
  difference() {
    union() {
      rrect(out_l, out_w, lid_h, corner_r);
      translate([0, 0, -spigot_h])
        difference() {
          rrect(cav_l - 2*spigot_clear, cav_w - 2*spigot_clear, spigot_h, corner_r * 0.6);
          translate([0, 0, -0.5])
            rrect(cav_l - 2*spigot_clear - 2*spigot_t,
                  cav_w - 2*spigot_clear - 2*spigot_t, spigot_h + 1, corner_r * 0.4);
        }
    }
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
