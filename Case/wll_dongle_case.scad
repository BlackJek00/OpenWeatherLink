// =====================================================================
//  WeatherLink Live dongle - desk enclosure
//  Board: ESP32-S3-DevKitC-1 and 1000 mil / 2x USB-C clones
//         PCB 57.15 x 27.94 (2250 x 1100 mil), module overhang to 63.39
//  Form:  leaning desk plaque, 2 printed parts + flush 2nd-colour inlays
//  OpenSCAD 2021.01 or newer. Units: mm.
//
//  Coordinate frames
//    local : X = width (left -> right), Y = up along the face,
//            Z = out of the face towards the viewer (back = 0, face = T)
//    world : local leaned back by `tilt`, desk plane at z = 0
//    board : u = from USB-end PCB edge towards antenna end
//            v = from lower long PCB edge
//            (component side facing the face, USB ports to the right)
//
//  Print
//    front  : face down on the bed (inlays are the first layers)
//    accent : same frame as front, assign 2nd filament
//    logo   : same frame as front, assign 2nd filament (needs logo_file)
//    back   : outer back face down
// =====================================================================

/* [Output] */
part = "assembly"; // [assembly, exploded, front, accent, logo, back, board]
coords = "print";  // [print, local]

/* [Board] */
pcb_len = 57.15;
pcb_w = 27.94;
pcb_t = 1.6;
module_overhang = 6.24;  // set 0 if the PCB itself is 63.4 long
module_len = 25.5;
module_w = 18.0;
module_h = 3.2;          // WROOM-1 can height above PCB
usb_h = 3.3;             // USB-C receptacle height above PCB
usb_proud = 0.7;         // receptacle face beyond PCB edge
headers_soldered = true; // pin headers on the underside
header_below = 8.5;      // spacer 2.5 + pin 6.0

/* [Enclosure] */
W = 82;
H = 52;
tilt = 15;               // lean back from vertical, deg
wall = 2.4;
face_t = 2.0;
back_t = 2.0;
floor_t = 2.4;
corner_r = 8;
front_chamfer = 1.6;     // 45 deg, prints clean on the bed
back_chamfer = 0.8;
comp_clear = 6.0;        // face inner surface -> PCB top
cut_above_front = 4;     // raises the desk cut: lower, more stable
fit = 0.25;
lip_h = 3.0;
lip_t = 1.2;

/* [USB opening] */
usb_mode = "slot";       // [slot, single]
usb_slot_w = 26.5;       // covers both ports incl. plug overmould
usb_slot_h = 7.6;
usb_slot_r = 2.5;
usb_single_w = 12.5;
usb_single_offset = 0;   // port centre vs PCB centreline (board v axis)

/* [Second colour inlays] */
inlay_d = 0.6;           // 3 layers at 0.2
accent_frame = true;
frame_inset = 5.5;
frame_w = 0.8;
emblem = true;           // mast + cup anemometer + wind vane + text
emblem_text = "WEATHERLINK";
emblem_font = "Liberation Sans:style=Bold";   // bundled with OpenSCAD
emblem_text_h = 5.2;     // cap height of the text
emblem_spacing = 1.06;   // letter tracking
emblem_icon_k = 1.0;     // icon scale (line widths stay >= min_line)
emblem_gap = 3.0;        // icon base -> text top
emblem_dy = 0;
min_line = 0.8;          // 2 perimeters at 0.4 nozzle
logo_file = "";          // your SVG/DXF, artwork shapes only, no background
logo_w = 60;
logo_dx = 0;
logo_dy = 0;
subtitle = "";
subtitle_size = 3.6;
subtitle_dy = -13;
subtitle_font = "Liberation Sans:style=Bold";

/* [Extras] */
led_window = false;      // thin skin over the RGB LED
led_pos = [30, 14];      // [u, v] on the board
led_window_d = 5;
led_window_skin = 0.6;
vents = true;
feet_recess = true;      // for 8 mm silicone bumpers
foot_d = 8.4;
foot_depth = 0.6;

/* [Fasteners: 2x M3x12 pan head self-tapping] */
screw_clear_d = 3.4;
screw_head_d = 6.4;
screw_head_h = 2.6;
screw_pilot_d = 2.6;     // 4.0-4.2 for M3 heat-set inserts
screw_depth = 10;
boss_d = 6.4;
pad_d = 8.4;
pad_h = 3;

/* [Hidden] */
$fn = 48;
eps = 0.01;
under_clear = headers_soldered ? header_below + 1.0 : 3.0;
T = back_t + under_clear + pcb_t + comp_clear + face_t;
Zf_in = T - face_t;
Zt = Zf_in - comp_clear;
Zb = Zt - pcb_t;
st = sin(tilt);
ct = cos(tilt);
z_cut = T * st + cut_above_front;
floor_z = z_cut + floor_t;
bx0 = W - wall;          // USB-end PCB edge sits on the right inner wall
face_y0 = max(front_chamfer, cut_above_front / ct);  // visible flat face
face_y1 = H - front_chamfer;
face_cy = (face_y0 + face_y1) / 2;
fr_y0 = face_y0 + frame_inset - front_chamfer;
fr_y1 = face_y1 - frame_inset + front_chamfer;

pin_tip_z = Zb - header_below;
pin_margin = (pin_tip_z < back_t + lip_h + 0.5) ? fit + lip_t + 0.5 : 1;
yb = max((floor_z + 1 - Zb * st) / ct,
         headers_soldered ? (floor_z + 1 - (Zb - 2.5) * st) / ct : 0,
         headers_soldered ? (floor_z + pin_margin - pin_tip_z * st) / ct - 0.95 : 0,
         wall + 3);

function bx(u) = bx0 - u;
function by(v) = yb + v;

usb_zc = Zt + 1.63;
usb_yc = by(pcb_w / 2);
boss_z0 = back_t + pad_h + 0.1;
guide_z0 = max(Zb - 1, back_t + lip_h + 0.3);   // clear of the back-cover lip
bosses = [[wall + 6.5, by(pcb_w / 2)],
          [W - wall - 7, H - wall - pad_d / 2 - 0.2]];

// ---------------------------------------------------------------- shapes
module puck(r, h, cb, cf) {
    rotate_extrude()
        polygon([[0, 0], [r - cb, 0], [r, cb], [r, h - cf], [r - cf, h], [0, h]]);
}

module rslab(x0, y0, x1, y1, z0, z1, r, cb, cf) {
    hull()
        for (x = [x0 + r, x1 - r], y = [y0 + r, y1 - r])
            translate([x, y, z0]) puck(r, z1 - z0, max(cb, eps), max(cf, eps));
}

// local half-space { Y*cos(tilt) + Z*sin(tilt) >= c }  (above a desk-parallel plane)
module above_desk(c) {
    rotate([tilt, 0, 0]) translate([-500, c, -500]) cube(1000);
}

module zslab(z0, z1) { translate([-500, -500, z0]) cube([1000, 1000, z1 - z0]); }

module outer() {
    intersection() {
        rslab(0, 0, W, H, 0, T, corner_r, back_chamfer, front_chamfer);
        above_desk(z_cut);
    }
}

module cavity(d = 0) {
    intersection() {
        rslab(wall + d, wall + d, W - wall - d, H - wall - d,
              back_t - 1, Zf_in - d, max(corner_r - wall - d, 1), eps, 0.8);
        above_desk(floor_z + d);
    }
}

module rrect2d(x0, y0, x1, y1, r) {
    hull() for (x = [x0 + r, x1 - r], y = [y0 + r, y1 - r]) translate([x, y]) circle(r);
}

module box(x0, x1, y0, y1, z0, z1) {
    translate([min(x0, x1), min(y0, y1), z0]) cube([abs(x1 - x0), abs(y1 - y0), z1 - z0]);
}

// --------------------------------------------------------------- inlays
module frame_2d() {
    difference() {
        rrect2d(frame_inset, fr_y0, W - frame_inset, fr_y1,
                max(corner_r - frame_inset, 0.6));
        rrect2d(frame_inset + frame_w, fr_y0 + frame_w,
                W - frame_inset - frame_w, fr_y1 - frame_w,
                max(corner_r - frame_inset - frame_w, 0.3));
    }
}

// original weather-station icon: mast, crossarm, cup anemometer (top view
// rotor) on the left end, wind vane on the right end. Origin: mast base.
module emblem_icon(k) {
    lw = min_line;
    A = [-10 * k, 14.5 * k];           // anemometer hub
    P = [ 10 * k, 15.5 * k];           // vane pivot
    ya = 9 * k;                        // crossarm centre line
    translate([-2.5 * k, 0]) square([5 * k, 0.9]);              // foot
    translate([-0.6, 0]) square([1.2, ya]);                     // mast
    translate([-12 * k, ya - 0.5]) square([24 * k, 1.0]);      // crossarm
    for (p = [A, P]) translate([p[0] - 0.45, ya]) square([0.9, p[1] - ya]);
    translate(A) {               // cup rotor, horizontal, seen from slightly above
        e = 0.42;                // foreshortening of the rotor plane
        circle(r = 1.1);
        for (phi = [90, 210, 330]) {
            c = [5.5 * k * cos(phi), 5.5 * k * sin(phi) * e];
            t = [-sin(phi), cos(phi) * e];      // CCW tangent = cup opening
            hull() { circle(d = lw); translate(c) circle(d = lw); }
            translate(c) rotate(atan2(t[1], t[0])) intersection() {
                circle(r = 2.3 * k);
                translate([-10, -10]) square([10, 20]);
            }
        }
    }
    translate(P) {                                              // wind vane
        circle(r = 0.9);
        translate([-6.5 * k, -0.45]) square([10.1 * k, 0.9]);  // shaft
        polygon([[-9.5 * k, 0], [-6.2 * k, 2.0 * k], [-6.2 * k, -2.0 * k]]);
        polygon([[3.2 * k, 0.45], [5.8 * k, 3.0 * k], [8.2 * k, 3.0 * k], [7.0 * k, 0],
                 [8.2 * k, -3.0 * k], [5.8 * k, -3.0 * k], [3.2 * k, -0.45]]);
    }
}

module emblem_2d() {
    k = emblem_icon_k;
    icon_h = 19.1 * k;
    icon_dx = -0.70 * k;               // centre the icon bounding box
    total = icon_h + emblem_gap + emblem_text_h;
    translate([W / 2, face_cy + emblem_dy - total / 2]) {
        resize([0, emblem_text_h], auto = true)
            text(emblem_text, size = 10, font = emblem_font, spacing = emblem_spacing,
                 halign = "center", valign = "baseline");
        translate([icon_dx, emblem_text_h + emblem_gap]) emblem_icon(k);
    }
}

module accent_2d() {
    if (accent_frame) frame_2d();
    if (emblem) emblem_2d();
    if (subtitle != "")
        translate([W / 2, face_cy + subtitle_dy])
            text(subtitle, size = subtitle_size, font = subtitle_font,
                 halign = "center", valign = "center");
}

module logo_2d() {
    if (logo_file != "")
        translate([W / 2 + logo_dx, face_cy + logo_dy])
            resize([logo_w, 0], auto = true) import(logo_file, center = true);
}

module inlay(extra = 0) {
    translate([0, 0, T - inlay_d]) linear_extrude(inlay_d + extra) children();
}

// --------------------------------------------------------- front shell
module usb_cut() {
    w = (usb_mode == "slot") ? usb_slot_w : usb_single_w;
    yc = (usb_mode == "slot") ? usb_yc : usb_yc + usb_single_offset;
    module rr(ww, hh, rr_, x0, len) {           // rounded rectangle along X
        hull() for (sy = [-1, 1], sz = [-1, 1])
            translate([x0, yc + sy * (ww / 2 - rr_), usb_zc + sz * (usb_slot_h / 2 + (hh - usb_slot_h) / 2 - rr_)])
                rotate([0, 90, 0]) cylinder(r = rr_, h = len);
    }
    rr(w, usb_slot_h, usb_slot_r, W - wall - 0.6, wall + 2);
    hull() {                                   // 0.8 outer chamfer
        rr(w, usb_slot_h, usb_slot_r, W - 0.8, eps);
        rr(w + 1.6, usb_slot_h + 1.6, usb_slot_r + 0.8, W, eps);
    }
    rr(w + 1.6, usb_slot_h + 1.6, usb_slot_r + 0.8, W, 2);
    // inner recess for the proud USB-C shells (both ports, any mode); also lets
    // them pass the wall when the board is dropped in straight
    box(W - wall - eps, W - wall + usb_proud + 0.2, usb_yc - (usb_slot_w - 2) / 2,
        usb_yc + (usb_slot_w - 2) / 2, back_t - 1, Zt + usb_h + 0.3);
}

module front_features() {
    // module-can presser
    mu = pcb_len + module_overhang - module_len + 8.5;
    translate([bx(mu) - 4, usb_yc - 4, Zt + module_h + 0.15])
        cube([8, 8, Zf_in - (Zt + module_h + 0.15) + eps]);
    // presser bar over the USB-C shells
    box(bx(1.0), bx(4.5), by(3), by(pcb_w - 3), Zt + usb_h + 0.15, Zf_in + eps);
    // lower PCB-edge guides (merge into floor and face)
    for (u = [12, 46])
        box(bx(u) - 1.5, bx(u) + 1.5, yb - fit - 6, yb - fit, guide_z0, Zf_in + eps);
    // upper PCB-edge guides
    for (u = [20, 46])
        box(bx(u) - 1.5, bx(u) + 1.5, by(pcb_w) + fit, by(pcb_w) + fit + 2, guide_z0, Zf_in + eps);
    // screw bosses
    for (b = bosses) translate([b[0], b[1], boss_z0]) cylinder(d = boss_d, h = Zf_in - boss_z0 + eps);
}

module front_shell() {
    difference() {
        union() {
            difference() {
                intersection() { outer(); zslab(back_t, T + 1); }
                cavity(0);
            }
            intersection() { cavity(0); front_features(); }
        }
        usb_cut();
        for (b = bosses)
            translate([b[0], b[1], boss_z0 - eps]) cylinder(d = screw_pilot_d, h = screw_depth);
        inlay(eps) accent_2d();
        inlay(eps) logo_2d();
        if (led_window)
            translate([bx(led_pos[0]), by(led_pos[1]), Zf_in - 1])
                cylinder(d = led_window_d, h = 1 + face_t - led_window_skin);
        if (feet_recess) {
            z0 = (back_t + T) / 2;
            y0 = (z_cut - z0 * st) / ct;
            for (x = [14, W - 14])
                translate([x, y0, z0]) rotate([tilt - 90, 0, 0])
                    translate([0, 0, -1]) cylinder(d = foot_d, h = foot_depth + 1);
        }
    }
}

// ----------------------------------------------------------- back cover
module back_features() {
    // lip
    intersection() {
        difference() { cavity(fit); cavity(fit + lip_t); }
        zslab(back_t - eps, back_t + lip_h);
    }
    // screw pads
    for (b = bosses) translate([b[0], b[1], back_t - eps]) cylinder(d = pad_d, h = pad_h + eps);
    // PCB support pads (inboard of the header rows)
    for (u = [[10, 15], [44, 49]], v = [[4, 7], [pcb_w - 7, pcb_w - 4]])
        box(bx(u[0]), bx(u[1]), by(v[0]), by(v[1]), back_t - eps, Zb);
    // antenna-end stop, below the module overhang
    box(bx(pcb_len) - fit - 2, bx(pcb_len) - fit, usb_yc - 5, usb_yc + 5, back_t - eps, Zt - 0.5);
}

module back_cover() {
    difference() {
        union() {
            intersection() { outer(); zslab(-1, back_t); }
            intersection() { cavity(fit); back_features(); }
        }
        for (b = bosses) {
            translate([b[0], b[1], -1]) cylinder(d = screw_clear_d, h = back_t + pad_h + 2);
            translate([b[0], b[1], -1]) cylinder(d = screw_head_d, h = screw_head_h + 1);
        }
        if (vents)
            for (i = [-3 : 3])
                translate([bx(30) + i * 3.4, by(pcb_w / 2), 0])
                    hull() for (s = [-1, 1])
                        translate([0, s * 5.2, -1]) cylinder(d = 1.6, h = back_t + 2, $fn = 16);
    }
}

// --------------------------------------------------------- dummy board
module board() {
    color("darkgreen") translate([bx(pcb_len), yb, Zb]) cube([pcb_len, pcb_w, pcb_t]);
    color("silver")
        translate([bx(pcb_len + module_overhang), by(pcb_w / 2 - module_w / 2), Zt])
            cube([module_len, module_w, module_h - 0.1]);
    for (o = [-7, 7]) color("silver")
        translate([bx(7.35 - usb_proud), by(pcb_w / 2 + o) - 4.47, Zt]) cube([7.35, 8.94, 3.26]);
    if (headers_soldered)
        for (v = [1.27, pcb_w - 1.27]) {
            color("black") translate([bx(56.515), by(v) - 1.27, Zb - 2.5]) cube([55.88, 2.54, 2.5]);
            color("gold") translate([bx(55.565), by(v) - 0.32, pin_tip_z])
                cube([53.98, 0.64, header_below + pcb_t + 2.0]);
        }
}

// ------------------------------------------------------------- output
module to_world() { translate([-W / 2, 0, -z_cut]) rotate([90 - tilt, 0, 0]) children(); }
module to_print_front() { translate([0, H, T]) rotate([180, 0, 0]) children(); }

c_body = [0.93, 0.93, 0.91];
c_ink = [0.12, 0.12, 0.13];

if (part == "assembly" || part == "exploded") {
    ex = (part == "exploded") ? 28 : 0;
    to_world() {
        translate([0, 0, ex]) {
            color(c_body) front_shell();
            color(c_ink) inlay() accent_2d();
            color(c_ink) inlay() logo_2d();
            if (logo_file == "" && !emblem)   // preview-only marker for the logo area
                %translate([0, 0, 0.3]) inlay()
                    translate([W / 2 + logo_dx, face_cy + logo_dy]) square([logo_w, logo_w / 4], center = true);
        }
        translate([0, 0, ex / 2]) board();
        color(c_body) back_cover();
    }
} else if (part == "front") {
    if (coords == "print") to_print_front() front_shell(); else front_shell();
} else if (part == "accent") {
    if (coords == "print") to_print_front() inlay() accent_2d(); else inlay() accent_2d();
} else if (part == "logo") {
    if (coords == "print") to_print_front() inlay() logo_2d(); else inlay() logo_2d();
} else if (part == "back") {
    back_cover();
} else if (part == "board") {
    board();
}

echo(str("T=", T, " yb=", yb, " z_cut=", z_cut, " board Y ", yb, "..", yb + pcb_w));
