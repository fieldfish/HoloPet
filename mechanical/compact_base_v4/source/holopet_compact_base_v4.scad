// HoloPet 紧凑底座 V4 — 实物装配纠错版
// 2026-09-22, unit: mm
// `part` selects one printable part or a reference assembly.
// OpenSCAD example:
//   openscad.com -o 01_main_shell.stl -D "part=\"main_shell\"" holopet_compact_base_v4.scad

part = is_undef(part) ? "assembly_components" : part;
$fn = 128;
eps = 0.12;

// ---------------- Measured component envelopes ----------------
screen_od = 115.0;
screen_h = 17.0;
screen_display_d = 87.6;
screen_pocket_d = 116.2;       // 0.6/side candidate
screen_aperture_d = 86.0;      // minimum only through the 1.5mm annular seat

pi_w = 85.0;
pi_h = 56.0;
pi_t = 1.6;
pi_mount_dx = 58.0;
pi_mount_dy = 49.0;
spacer_h = 5.0;
spacer_od = 6.5;
spacer_hole_d = 2.8;
screen_rear_to_cooler_bottom = 25.0; // measured assembly clearance

speaker_w = 70.0;
speaker_h = 30.0;
speaker_depth = 20.0;          // conservative component envelope
speaker_hole_dx = 58.0;
speaker_hole_dy = 22.0;
speaker_hole_d = 3.2;
speaker_clear_l = 72.0;        // 1mm clearance at each 70mm end
speaker_clear_h = 32.0;        // 1mm clearance above/below 30mm body
speaker_clear_d = 22.0;        // 2mm reserve behind 20mm body
speaker_cradle_wall = 2.5;
speaker_cable_slot_w = 14.0;
speaker_cable_slot_h = 10.0;

audio_body_l = 70.0;           // measured microphone / audio assembly length
audio_bend_each = 10.0;        // cable-bend reserve at both connector ends
audio_keepout_l = audio_body_l + 2*audio_bend_each; // 90mm total keepout
audio_w = 28.0;
audio_t = 12.0;

dome_od = 90.0;
dome_id = 86.0;
dome_h = 180.0;
dome_clearance = 0.60;
dome_groove_od = dome_od + 2*dome_clearance;
dome_insert = 10.0;             // lower locating depth; no tall inner wall
dome_shield_additional_h = 45.0;// extra opaque cover around lower glass

// ---------------- Compact body ----------------
body_od = 142.0;
body_id = 136.0;
wall = 3.0;
body_h = 65.0;
bottom_plate_t = 4.0;
bottom_lip_h = 3.0;

collar_insert_h = 5.0;
collar_visible_h = 64.0;        // V3 + 10mm external opaque shielding
collar_h = collar_insert_h + collar_visible_h; // 69
collar_origin_z = body_h - collar_insert_h;    // assembled Z=60
assembled_h = collar_origin_z + collar_h;      // 129, electronics body remains compact
collar_insert_d = body_id - 0.8;               // 0.4/side
collar_mid_od = 108.0;
collar_neck_od = 97.2;          // 3mm radial wall around the unchanged Ø91.2 sleeve ID
screen_front_local_z = 8.0;
screen_front_z = collar_origin_z + screen_front_local_z; // 68
screen_rear_z = screen_front_z - screen_h;                // 51
hardware_low_z = screen_rear_z - screen_rear_to_cooler_bottom; // 26
glass_seat_local_z = 14.0;     // same lower glass datum as compact collar before extension
glass_seat_t = 1.5;            // short support shoulder only; no 10mm inner wall
glass_sleeve_id = dome_groove_od; // 91.2 clear bore around OD90 glass

// Two wall speakers share one 30mm vertical band; USB audio lies flat below Pi.
carrier_z = 7.0;
speaker_z0 = carrier_z + 3.0;
speaker_face_y = 52.0;          // cradle maxR≈64.7, leaving >3mm to ID136
audio_floor_z = carrier_z + 3.0;

// Fasteners / controls
m3_clearance_d = 3.4;
m3_pilot_d = 2.5;
collar_fastener_angles = [35, 145, 265];
retainer_fastener_angles = [45, 135, 270];
bottom_fastener_angles = [45, 135, 225, 315];
bottom_fastener_r = 64.0;
ec11_angle = 90;                // +Y, above the front speaker rather than behind Pi
ec11_z = 52.5;                  // PCB bottom≈40.5, clears speaker top Z=40
ec11_hole_d = 7.3;
ec11_face_boss_d = 18.0;        // compact flat land for panel nut / washer
ec11_face_boss_depth = 2.5;
ec11_inner_pad_w = 24.0;
ec11_inner_pad_h = 24.0;        // stays inside 65mm shell; no local height increase

retainer_od = 133.0;
retainer_id = 94.0;
retainer_fastener_r = 62.0;

knob_od = 18.0;
knob_h = 12.0;
knob_socket_d = 6.25;

// External service openings.  The Pi USB/Ethernet edge remains directly
// accessible; Pi power/HDMI and display power are intentionally separate.
pi_usb_eth_angle = 0;
pi_usb_eth_w = 64.0;
pi_usb_eth_z0 = 43.0;
pi_usb_eth_h = 19.0;
pi_power_hdmi_angle = 315;
pi_power_hdmi_w = 44.0;
pi_power_hdmi_z0 = 43.0;
pi_power_hdmi_h = 19.0;
screen_power_angle = 225;
screen_power_w = 20.0;
screen_power_z0 = 47.0;
screen_power_h = 15.0;

module annulus(od, id, h) {
    difference() {
        cylinder(d=od, h=h);
        translate([0,0,-eps]) cylinder(d=id, h=h+2*eps);
    }
}

module rounded_rect_2d(w, h, r=2) {
    hull() for (x=[-w/2+r,w/2-r], y=[-h/2+r,h/2-r]) translate([x,y]) circle(r=r,$fn=24);
}

module rounded_box(w,d,h,r=2) {
    linear_extrude(height=h) rounded_rect_2d(w,d,r);
}

module radial_hole(angle,z,d,len=9) {
    rotate([0,0,angle]) translate([body_id/2-2,0,z]) rotate([0,90,0]) cylinder(d=d,h=len,$fn=32);
}

module radial_window(angle,z0,tangent_w,height,len=9) {
    rotate([0,0,angle]) translate([body_id/2-2,-tangent_w/2,z0]) cube([len,tangent_w,height]);
}

module wall_boss(angle,z0,h,center_r=56,od=8) {
    rotate([0,0,angle]) {
        translate([center_r,0,z0]) cylinder(d=od,h=h,$fn=36);
        translate([center_r,-od/2,z0]) cube([body_id/2+wall-center_r,od,h]);
    }
}

module radial_face_boss(angle,z,d,depth=2.5) {
    rotate([0,0,angle]) translate([body_od/2-1,0,z]) rotate([0,90,0])
        cylinder(d=d,h=depth+1,$fn=56);
}

module radial_inner_pad(angle,z0,tangent_w,height,depth=4) {
    rotate([0,0,angle])
        translate([body_id/2-depth,-tangent_w/2,z0])
            cube([depth+wall,tangent_w,height]);
}

// ---------------- 01 Main shell ----------------
module main_shell() {
    difference() {
        union() {
            difference() {
                cylinder(d=body_od,h=body_h);
                translate([0,0,-eps]) cylinder(d=body_id,h=body_h+2*eps);
            }
            // collar seat
            translate([0,0,body_h-6]) annulus(body_id+1.0,116.8,1.4);
            // bottom screw bosses
            for(a=bottom_fastener_angles) wall_boss(a,0,10,bottom_fastener_r,8);
            // EC11 panel reinforcement: inner pad resists rotation and the
            // outer planar boss gives the panel nut a flat clamping surface.
            radial_inner_pad(ec11_angle,ec11_z-ec11_inner_pad_h/2,
                             ec11_inner_pad_w,ec11_inner_pad_h,4);
            radial_face_boss(ec11_angle,ec11_z,ec11_face_boss_d,
                             ec11_face_boss_depth);
        }

        // Direct Pi USB / Ethernet access.
        radial_window(pi_usb_eth_angle,pi_usb_eth_z0,
                      pi_usb_eth_w,pi_usb_eth_h,12);
        // Pi USB-C power plus dual micro-HDMI service opening.  Located
        // between the two speaker zones rather than through a speaker frame.
        radial_window(pi_power_hdmi_angle,pi_power_hdmi_z0,
                      pi_power_hdmi_w,pi_power_hdmi_h,12);
        // Independent display-power opening.  This must not be shared with
        // Pi power so either board can be disconnected and serviced alone.
        radial_window(screen_power_angle,screen_power_z0,
                      screen_power_w,screen_power_h,12);

        // Two curved acoustic grille fields for the two 70x30 wall speakers.
        for(a0=[90,270])
            for(da=[-26:6:26])
                for(zz=[14:6:34]) radial_window(a0+da,zz,4.2,3.0,9);

        // Microphone / audio-card grille at -X low band.
        for(yy=[-10:5:10], zz=[11:5:21])
            rotate([0,0,180]) translate([body_id/2-2,yy,zz]) rotate([0,90,0])
                cylinder(d=2.4,h=9,$fn=24);

        // EC11 shaft and anti-rotation hole.
        radial_hole(ec11_angle,ec11_z,ec11_hole_d,9);
        rotate([0,0,ec11_angle]) translate([body_id/2-2,8,ec11_z]) rotate([0,90,0])
            cylinder(d=2.2,h=9,$fn=24);

        // thermal vents, deliberately separated from microphone holes
        for(a=[150:10:200]) for(zz=[38,44]) radial_window(a,zz,6,2.8,9);

        // collar radial screws
        for(a=collar_fastener_angles) radial_hole(a,body_h-2.5,m3_clearance_d,9);
        // screen rear-retainer radial screws at the screen rear plane
        for(a=retainer_fastener_angles) radial_hole(a,screen_rear_z-2.0,m3_clearance_d,9);
        // bottom cover pilot holes
        for(a=bottom_fastener_angles)
            rotate([0,0,a]) translate([bottom_fastener_r,0,-eps]) cylinder(d=m3_pilot_d,h=9,$fn=24);
    }
}

// ---------------- 02 Glass collar + screen front shoulder ----------------
module top_collar_raw() {
    difference() {
        union() {
            cylinder(d=collar_insert_d,h=collar_insert_h);
            // Keep full OD through the Ø116.2 screen pocket. Tapering before
            // the pocket ended created a disconnected upper sleeve.
            translate([0,0,collar_insert_h-0.1]) cylinder(d=body_od,h=4.2);
            translate([0,0,collar_insert_h+4]) cylinder(d1=body_od,d2=collar_neck_od,h=5.0);
            translate([0,0,collar_insert_h+9]) cylinder(d=collar_neck_od,h=collar_visible_h-9);
        }
        // LCD module enters from below.  Cuts overlap by eps so the exported
        // mesh cannot retain a zero-thickness closing face.
        translate([0,0,-10])
            cylinder(d=screen_pocket_d,h=screen_front_local_z+10+eps);
        // Short expanding optical cone: Ø87.6 at the display grows to Ø91.2
        // at the glass seat instead of using a long Ø86 tube.
        translate([0,0,screen_front_local_z-eps])
            cylinder(d1=screen_display_d,d2=glass_sleeve_id,
                     h=glass_seat_local_z-screen_front_local_z+2*eps);
        // The minimum Ø86 opening exists only through the 1.5mm annular seat.
        translate([0,0,glass_seat_local_z-eps])
            cylinder(d=screen_aperture_d,h=glass_seat_t+2*eps);
        // Above the seat the full Ø91.2 sleeve bore continues to the top.
        translate([0,0,glass_seat_local_z+glass_seat_t-eps])
            cylinder(d=glass_sleeve_id,
                     h=collar_h-glass_seat_local_z-glass_seat_t+2*eps);
        // No full-depth radial cable notch here: the collar remains a closed
        // 360-degree ring.  Display cabling uses the bridged retainer tunnel.
        // radial pilot holes matching the shell
        for(a=collar_fastener_angles)
            rotate([0,0,a]) translate([collar_insert_d/2-8,0,2.5]) rotate([0,90,0])
                cylinder(d=m3_pilot_d,h=9,$fn=24);
    }
}

module top_collar() {
    // printable orientation: narrow neck on bed; local support only under shoulder
    translate([0,0,collar_h]) rotate([180,0,0]) top_collar_raw();
}

// ---------------- 03 Screen rear retainer ----------------
module screen_rear_retainer() {
    // Fully closed annulus.  Display cabling passes through the Ø94 centre
    // opening and then through the shell service port.
    difference() {
        annulus(retainer_od,retainer_id,4.0);
        for(a=retainer_fastener_angles)
            rotate([0,0,a]) translate([retainer_fastener_r,0,2.0]) rotate([0,90,0])
                cylinder(d=m3_pilot_d,h=12,$fn=24);
    }
}

// ---------------- 04 Pi spacers, four loose parts on one plate ----------------
module one_spacer() {
    difference() {
        cylinder(d=spacer_od,h=spacer_h,$fn=48);
        translate([0,0,-eps]) cylinder(d=spacer_hole_d,h=spacer_h+2*eps,$fn=28);
    }
}

module pi_spacers_x4() {
    for(x=[-6,6],y=[-6,6]) translate([x,y,0]) one_spacer();
}

// ---------------- 05 Audio component carrier ----------------
module speaker_cradle_local() {
    outer_l = speaker_clear_l + 2*speaker_cradle_wall;
    outer_h = speaker_clear_h + speaker_cradle_wall;
    // Local sound face is Y=0; the speaker body extends toward -Y.
    difference() {
        union() {
            // Full-size floor: 72x22mm clear footprint for a 70x20mm body.
            translate([-outer_l/2,-speaker_clear_d,0])
                cube([outer_l,speaker_clear_d,speaker_cradle_wall]);
            // End rails leave 72mm clear length; the V3 rails left only 66mm.
            for(x=[-outer_l/2,outer_l/2-speaker_cradle_wall])
                translate([x,-speaker_clear_d,0])
                    cube([speaker_cradle_wall,speaker_clear_d,outer_h]);
            // Low inner rail prevents radial movement but does not cover the
            // speaker cone or trap the cable.
            translate([-outer_l/2,-speaker_clear_d,0])
                cube([outer_l,speaker_cradle_wall,12]);
            // Low front lip prevents the speaker sliding toward the shell;
            // only the bottom 5mm is retained, leaving the cone unobstructed.
            translate([-outer_l/2,-speaker_cradle_wall,0])
                cube([outer_l,speaker_cradle_wall,5]);
        }
        // Cable exits on BOTH short ends; either speaker lead orientation fits.
        for(side=[-1,1])
            translate([side<0 ? -outer_l/2-eps : outer_l/2-speaker_cradle_wall-eps,
                       -speaker_clear_d+4,4])
                cube([speaker_cradle_wall+2*eps,
                      speaker_clear_d-8,
                      speaker_cable_slot_h]);
        // Additional rear-centre cable path toward the audio tray.
        translate([-speaker_cable_slot_w/2,-speaker_clear_d-eps,4])
            cube([speaker_cable_slot_w,speaker_cradle_wall+2*eps,
                  speaker_cable_slot_h]);
    }
}

module speaker_frame(sign=1) {
    if(sign>0) translate([0,speaker_face_y,0]) speaker_cradle_local();
    else mirror([0,1,0]) translate([0,speaker_face_y,0]) speaker_cradle_local();
}

module audio_component_carrier() {
    carrier_l = audio_keepout_l + 4;
    difference() {
        union() {
            // 70mm device plus 10mm cable-bend reserve at each end.  The
            // connector ends remain fully open so leads are not folded hard.
            rounded_box(carrier_l,audio_w+4,3,3);
            // low side lips; connector and bend zones remain open
            translate([-carrier_l/2,-audio_w/2-2,3]) cube([carrier_l,2,8]);
            translate([-carrier_l/2,audio_w/2,3]) cube([carrier_l,2,8]);
            speaker_frame(1);
            speaker_frame(-1);
            // four ribs bind both cradles to the central tray
            for(x=[-38.5,36],s=[-1,1])
                hull() {
                    translate([x,s*15,0]) cube([3,3,3]);
                    translate([x,s*(speaker_face_y-3),0]) cube([3,3,12]);
                }
        }
        // Sound-card straps plus three full-through cable paths.
        for(x=[-32,32]) translate([x,-18,-eps]) cube([4,36,3+2*eps]);
        translate([-8,-18,-eps]) cube([16,36,3+2*eps]);
    }
}

// Cheap one-speaker fit test; print this before the complete carrier.
module speaker_fit_coupon() {
    speaker_cradle_local();
}

// ---------------- 06 Bottom cover ----------------
module bottom_cover() {
    difference() {
        union() {
            cylinder(d=body_od,h=bottom_plate_t);
            translate([0,0,bottom_plate_t]) annulus(body_id-0.8,112.0,bottom_lip_h);
        }
        // intake field under audio card and Pi
        for(x=[-42:7:42],y=[-42:7:42])
            if(x*x+y*y<=48*48) translate([x,y,-eps]) cylinder(d=4.0,h=bottom_plate_t+eps*2,$fn=22);
        for(a=bottom_fastener_angles)
            rotate([0,0,a]) translate([bottom_fastener_r,0,-eps]) cylinder(d=m3_clearance_d,h=8,$fn=24);
        // lip relief around screw bosses
        for(a=bottom_fastener_angles)
            rotate([0,0,a]) translate([52,-5,bottom_plate_t-eps]) cube([12,10,bottom_lip_h+2*eps]);
        for(a=[0,90,180,270]) rotate([0,0,a]) translate([50,0,-eps]) cylinder(d=3.2,h=5,$fn=24);
    }
}

// ---------------- 07 EC11 knob ----------------
module ec11_knob() {
    difference() {
        cylinder(d1=knob_od,d2=knob_od-0.8,h=knob_h);
        // Visible through D-bore.  V3's blind socket was easy to misread and
        // too shallow for some shafts; this bore is open at both ends.
        translate([0,0,-eps]) intersection() {
            cylinder(d=knob_socket_d,h=knob_h+2*eps,$fn=48);
            translate([-4,-knob_socket_d/2,0])
                cube([8,knob_socket_d-0.65,knob_h+2*eps]);
        }
    }
}

// ---------------- Component proxy models: REFERENCE ONLY ----------------
module screen_model() {
    color([0.04,0.05,0.07]) cylinder(d=screen_od,h=screen_h);
    color([0.05,0.35,0.70]) translate([0,0,screen_h]) cylinder(d=screen_display_d,h=0.5);
}

module pi_model() {
    color([0.0,0.45,0.20]) translate([-pi_w/2,-pi_h/2,0]) cube([pi_w,pi_h,pi_t]);
    // measured conservative cooler envelope ending 25mm below screen rear
    color([0.35,0.37,0.40]) translate([-31.75,-21.25,-(screen_rear_to_cooler_bottom-spacer_h-pi_t)])
        cube([63.5,42.5,screen_rear_to_cooler_bottom-spacer_h-pi_t]);
    // coarse connector blocks on +X and -Y edges
    color([0.65,0.65,0.68]) translate([pi_w/2-2,-22,1.6]) cube([16,18,15]);
    color([0.65,0.65,0.68]) translate([pi_w/2-2,5,1.6]) cube([16,34,15]);
    color([0.65,0.65,0.68]) translate([-34,-pi_h/2-7,1.6]) cube([38,9,8]);
}

module speaker_model(sign=1) {
    y0 = sign>0 ? speaker_face_y-speaker_depth : -speaker_face_y;
    color([0.08,0.08,0.09]) translate([-speaker_w/2,y0,speaker_z0]) cube([speaker_w,speaker_depth,speaker_h]);
    color([0.18,0.18,0.19]) translate([0,sign*(speaker_face_y+0.2),speaker_z0+speaker_h/2])
        rotate([90,0,0]) scale([1.7,1,1]) cylinder(d=18,h=1,$fn=48);
}

module audio_model() {
    color([0.05,0.05,0.06]) translate([-audio_body_l/2,-audio_w/2,audio_floor_z])
        rounded_box(audio_body_l,audio_w,audio_t,2);
    // Orange proxy volumes are keep-clear bend zones, not printed material.
    for(s=[-1,1]) color([0.95,0.45,0.08,0.55])
        translate([s>0 ? audio_body_l/2 : -audio_body_l/2-audio_bend_each,
                   -audio_w/2,audio_floor_z])
            cube([audio_bend_each,audio_w,audio_t]);
}

module ec11_model() {
    rotate([0,0,ec11_angle]) {
        color([0.1,0.45,0.25]) translate([body_id/2-22,-12,ec11_z-12]) cube([22,24,24]);
        color([0.65,0.65,0.65]) translate([body_id/2-1,0,ec11_z]) rotate([0,90,0]) cylinder(d=7,h=12,$fn=32);
    }
}

module glass_model() {
    color([0.70,0.88,1.0,0.20]) difference() {
        cylinder(d=dome_od,h=dome_h);
        translate([0,0,-eps]) cylinder(d=dome_id,h=dome_h+2*eps);
    }
}

module component_models_only() {
    translate([0,0,screen_rear_z]) screen_model();
    for(x=[-pi_mount_dx/2,pi_mount_dx/2],y=[-pi_mount_dy/2,pi_mount_dy/2])
        translate([x,y,screen_rear_z-spacer_h]) one_spacer();
    translate([0,0,screen_rear_z-spacer_h-pi_t]) pi_model();
    speaker_model(1); speaker_model(-1);
    audio_model(); ec11_model();
    translate([0,0,collar_origin_z+glass_seat_local_z+glass_seat_t]) glass_model();
}

module component_models_no_glass() {
    translate([0,0,screen_rear_z]) screen_model();
    for(x=[-pi_mount_dx/2,pi_mount_dx/2],y=[-pi_mount_dy/2,pi_mount_dy/2])
        translate([x,y,screen_rear_z-spacer_h]) one_spacer();
    translate([0,0,screen_rear_z-spacer_h-pi_t]) pi_model();
    speaker_model(1); speaker_model(-1);
    audio_model(); ec11_model();
}

module assembly_printed(exploded=0) {
    main_shell();
    translate([0,0,collar_origin_z+exploded*10]) top_collar_raw();
    translate([0,0,screen_rear_z-4-exploded*8]) screen_rear_retainer();
    translate([0,0,carrier_z-exploded*12]) audio_component_carrier();
    translate([0,0,-bottom_plate_t-exploded*16]) bottom_cover();
}

module assembly_components() {
    color([0.12,0.14,0.18]) assembly_printed(0);
    component_models_only();
}

module cutaway_components() {
    intersection() {
        union() {
            color([0.12,0.14,0.18]) assembly_printed(0);
            component_models_no_glass();
        }
        translate([-150,-0.2,-20]) cube([300,160,170]);
    }
}

// Low-cost fit coupon: body ID / screen OD / collar skirt relationship.
module screen_body_fit_coupon() {
    difference() {
        cylinder(d=body_od,h=10);
        translate([0,0,-eps]) cylinder(d=body_id,h=10+2*eps);
        translate([-70,-70,-1]) cube([70,140,12]);
    }
    translate([0,0,10]) difference() {
        cylinder(d=collar_insert_d,h=5);
        translate([0,0,-eps]) cylinder(d=screen_pocket_d,h=5+2*eps);
        translate([-70,-70,-1]) cube([70,140,7]);
    }
}

if(part=="main_shell") main_shell();
else if(part=="top_collar") top_collar();
else if(part=="screen_rear_retainer") screen_rear_retainer();
else if(part=="pi_spacers_x4") pi_spacers_x4();
else if(part=="audio_component_carrier") audio_component_carrier();
else if(part=="bottom_cover") bottom_cover();
else if(part=="ec11_knob") ec11_knob();
else if(part=="screen_body_fit_coupon") screen_body_fit_coupon();
else if(part=="speaker_fit_coupon") speaker_fit_coupon();
else if(part=="components_only") component_models_only();
else if(part=="assembly_printed") assembly_printed(0);
else if(part=="exploded") assembly_printed(1);
else if(part=="cutaway_components") cutaway_components();
else assembly_components();
