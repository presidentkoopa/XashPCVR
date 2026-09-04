/*
vr_openxr.c - OpenXR VR support for Xash3D FWGS (PCVR fork)
Copyright (C) 2026

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

See vr_openxr.h for the design rationale.
*/

#include "common.h"
#include "client.h"
#include "ref_common.h"
#include "mod_local.h"		// Mod_ForName - engine-internal model loading, for bare-hand models
#include "entity_types.h"	// ET_NORMAL
#include "keydefs.h"		// K_MOUSE1 - VR menu pointer clicks
#include "vr_openxr.h"

// XASH_OPENXR is defined by the build ONLY when an openxr_loader for this
// architecture was actually found (engine/wscript). Without it in the guard,
// this file still compiled - the source glob is unconditional - and died on
// #include <openxr/openxr.h> with no include path, so ANY Windows client build
// configured without --openxr failed outright rather than falling back to
// flatscreen.
#if XASH_WIN32 && !XASH_DEDICATED && XASH_OPENXR

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_OPENGL

#include <windows.h>
// openxr_platform.h declares MSFT perception-anchor interop in terms of IUnknown
// when XR_USE_PLATFORM_WIN32 is set, so the COM base interface must be visible.
#include <unknwn.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#if XASH_SDL
#include <SDL.h>
#endif

CVAR_DEFINE_AUTO( vr_enable, "1", FCVAR_ARCHIVE, "enable OpenXR VR rendering" );
static CVAR_DEFINE_AUTO( vr_debug, "0", 0, "verbose OpenXR logging" );

// Axis-sign escape hatches. Converting an OpenXR quaternion into GoldSrc euler
// angles has several sign conventions that can only really be confirmed by
// wearing the headset. These let that be corrected live, in-game, instead of
// requiring a rebuild per guess. Once the correct combination is known it should
// be baked into VR_ConvertOrientation and these can go.
static CVAR_DEFINE_AUTO( vr_pitch_sign, "1", FCVAR_ARCHIVE, "flip HMD pitch (1 or -1)" );
static CVAR_DEFINE_AUTO( vr_yaw_sign, "1", FCVAR_ARCHIVE, "flip HMD yaw (1 or -1)" );
static CVAR_DEFINE_AUTO( vr_roll_sign, "1", FCVAR_ARCHIVE, "flip HMD roll (1 or -1)" );
static CVAR_DEFINE_AUTO( vr_compose_yaw, "1", FCVAR_ARCHIVE, "add the game's yaw to the HMD yaw" );

static CVAR_DEFINE_AUTO( vr_diag, "1", FCVAR_ARCHIVE, "write vr_diag.log (0 off, 1 normal, 2 every frame)" );
static CVAR_DEFINE_AUTO( vr_diag_interval, "0.5", FCVAR_ARCHIVE, "seconds between diagnostic pose samples" );

// locomotion
static CVAR_DEFINE_AUTO( vr_movespeed, "400", FCVAR_ARCHIVE, "thumbstick movement speed" );
static CVAR_DEFINE_AUTO( vr_turnspeed, "120", FCVAR_ARCHIVE, "smooth turn degrees/sec" );
static CVAR_DEFINE_AUTO( vr_snap_turn, "1", FCVAR_ARCHIVE, "1 = snap turning, 0 = smooth" );
static CVAR_DEFINE_AUTO( vr_snap_angle, "30", FCVAR_ARCHIVE, "snap turn step in degrees" );
static CVAR_DEFINE_AUTO( vr_deadzone, "0.2", FCVAR_ARCHIVE, "thumbstick deadzone (0-1)" );
// Teleport locomotion. Off by default: smooth movement stays the default
// experience, and this is the comfort alternative for players who need it.
static CVAR_DEFINE_AUTO( vr_teleport, "0", FCVAR_ARCHIVE, "movement stick aims a teleport arc instead of walking" );
static CVAR_DEFINE_AUTO( vr_teleport_speed, "600", FCVAR_ARCHIVE, "teleport arc launch speed (reach)" );
static CVAR_DEFINE_AUTO( vr_teleport_gravity, "800", FCVAR_ARCHIVE, "teleport arc gravity" );
static CVAR_DEFINE_AUTO( vr_teleport_steps, "48", FCVAR_ARCHIVE, "teleport arc integration steps" );
static CVAR_DEFINE_AUTO( vr_teleport_step_time, "0.05", FCVAR_ARCHIVE, "teleport arc seconds per step" );
static CVAR_DEFINE_AUTO( vr_teleport_width, "1.0", FCVAR_ARCHIVE, "teleport arc ribbon half-width" );
static CVAR_DEFINE_AUTO( vr_teleport_alpha, "0.7", FCVAR_ARCHIVE, "teleport arc opacity" );
static CVAR_DEFINE_AUTO( vr_hud_scale, "0.55", FCVAR_ARCHIVE, "HUD size within the eye (0.2-1.0)" );
static CVAR_DEFINE_AUTO( vr_hud_parallax, "0.02", FCVAR_ARCHIVE, "HUD stereo disparity as a fraction of eye width; 0 puts the HUD at infinity" );
static CVAR_DEFINE_AUTO( vr_deathload, "1", FCVAR_ARCHIVE, "pull the trigger while dead to reload the last quicksave" );
static CVAR_DEFINE_AUTO( vr_autosave, "120", FCVAR_ARCHIVE, "seconds between automatic quicksaves; 0 = off" );
static CVAR_DEFINE_AUTO( vr_shoulder_light, "1", FCVAR_ARCHIVE, "reach beside your head with the off hand to toggle the flashlight" );
static CVAR_DEFINE_AUTO( vr_shoulder_melee, "1", FCVAR_ARCHIVE, "reach beside your head to swap to the melee weapon and back" );
static CVAR_DEFINE_AUTO( vr_reload, "1", FCVAR_ARCHIVE, "load the gun by hand instead of pressing reload" );
static CVAR_DEFINE_AUTO( vr_reload_side, "11", FCVAR_ARCHIVE, "ammo pouch offset out to the off-hand side, units" );
static CVAR_DEFINE_AUTO( vr_reload_down, "26", FCVAR_ARCHIVE, "how far below the head the pouch sits, units" );
static CVAR_DEFINE_AUTO( vr_reload_back, "2", FCVAR_ARCHIVE, "pouch offset behind the head, units" );
static CVAR_DEFINE_AUTO( vr_reload_radius, "11", FCVAR_ARCHIVE, "size of the ammo pouch hotspot, units" );
static CVAR_DEFINE_AUTO( vr_reload_port, "13", FCVAR_ARCHIVE, "how near the gun counts as the loading port, units" );
static CVAR_DEFINE_AUTO( vr_reload_port_fwd, "4", FCVAR_ARCHIVE, "port offset forward of the grip, units" );
// PUBLISHED TO THE SERVER, PER PLAYER.
//
// FCVAR_USERINFO puts this in the client's userinfo, where a game DLL can
// read it with pfnInfoKeyValue for THAT player specifically - the same
// channel "name" and "model" already travel on. Per player is the whole
// point: a global cvar would change reloading for everyone on the server,
// so a flatscreen player joining a VR host would inherit a reload they have
// no way to perform.
//
// A DLL that has never heard of it simply never asks, and behaves exactly as
// it does today. That is what keeps the mod catalogue untouched: the engine
// states a fact about the player and nothing more.
static CVAR_DEFINE_AUTO( vr_handload, "0", FCVAR_USERINFO, "this player loads weapons by hand, one round at a time" );
static CVAR_DEFINE_AUTO( vr_pump, "1", FCVAR_ARCHIVE, "pump-action weapons must have the action worked between shots" );
static CVAR_DEFINE_AUTO( vr_reload_model, "models/shotgunshell.mdl", FCVAR_ARCHIVE, "what a carried round looks like; empty to draw nothing" );
static CVAR_DEFINE_AUTO( vr_pump_scrub, "0.45", FCVAR_ARCHIVE, "seconds of animation a full pull of the action covers" );
static CVAR_DEFINE_AUTO( vr_pump_ejects, "0", FCVAR_ARCHIVE, "working a loaded action throws the chambered round away, as it would" );
static CVAR_DEFINE_AUTO( vr_pump_recoil, "0.35", FCVAR_ARCHIVE, "seconds of firing animation to play before it holds for the pump" );
static CVAR_DEFINE_AUTO( vr_pump_reach, "44", FCVAR_ARCHIVE, "how near the weapon a hand must be to work its action, units" );
static CVAR_DEFINE_AUTO( vr_action_sound, "weapons/scock1.wav", FCVAR_ARCHIVE, "sound played when the action is worked; empty for none" );
static CVAR_DEFINE_AUTO( vr_pump_travel, "0.8", FCVAR_ARCHIVE, "how far the action must be pulled back, units" );
static CVAR_DEFINE_AUTO( vr_reload_hold, "1.0", FCVAR_ARCHIVE, "seconds on the reload button to force an ordinary reload" );
static CVAR_DEFINE_AUTO( vr_shoulder_radius, "9", FCVAR_ARCHIVE, "size of the over-the-shoulder hotspot, units" );
static CVAR_DEFINE_AUTO( vr_shoulder_side, "7", FCVAR_ARCHIVE, "hotspot offset out to the dominant side, units" );
static CVAR_DEFINE_AUTO( vr_shoulder_back, "5", FCVAR_ARCHIVE, "hotspot offset behind the head, units" );
static CVAR_DEFINE_AUTO( vr_shoulder_up, "2", FCVAR_ARCHIVE, "hotspot offset above the head centre, units" );
static CVAR_DEFINE_AUTO( vr_menu_lock, "1", FCVAR_ARCHIVE, "anchor the menu in the world and drive it with a controller pointer" );
static CVAR_DEFINE_AUTO( vr_menu_leash, "35", FCVAR_ARCHIVE, "degrees the head may turn before the anchored menu is dragged along" );
static CVAR_DEFINE_AUTO( vr_menu_pitch_sign, "-1", FCVAR_ARCHIVE, "flip if the menu anchor and pointer move the wrong way vertically" );
CVAR_DEFINE_AUTO( vr_hands, "1", FCVAR_ARCHIVE, "pin the viewmodel to the right controller" );
CVAR_DEFINE_AUTO( vr_hud, "1", FCVAR_ARCHIVE, "draw the 2D HUD/menu inside the headset" );

// Calibration offset for the hand/weapon mesh's local "forward" axis, applied
// on top of the tracked pose. Live-reported: wrist-to-fingertip pointed at
// the floor while pitch read ~-80 (which, by this engine's own convention,
// means "pointing steeply up") - the mesh's own rest-pose forward axis does
// not line up with the standard local-+X-forward assumption the angle math
// is built on. +90 pitch is the single most likely correction for exactly
// this symptom (a mesh whose forward axis is a quarter-turn off from
// standard), so it is the default rather than 0 - not a guess shipped blind,
// a specific candidate chosen from the reported numbers.
static CVAR_DEFINE_AUTO( vr_supersample, "1.0", FCVAR_ARCHIVE, "render scale per eye; 1 = the runtime's own recommendation" );
static CVAR_DEFINE_AUTO( vr_msaa, "4", FCVAR_ARCHIVE, "multisample samples per eye: 0, 2, 4 or 8" );
static CVAR_DEFINE_AUTO( vr_depth_submit, "1", FCVAR_ARCHIVE, "give the runtime our depth buffer so it can reproject accurately" );
CVAR_DEFINE_AUTO( vr_mirror, "1", FCVAR_ARCHIVE, "mirror the left eye to the desktop window" );

// DEFAULT 0, deliberately. A 90-degree pitch "calibration" lived here for a
// long time and was wrong: it rotated the mesh's whole reference frame, so
// physical ROLL (turning the palm over) came out as rotation about a
// perpendicular axis - reported live as flipping your palm swinging the
// whole arm through a wide arc, when in reality a palm flip pivots in place.
// It was compensating for a pitch inversion whose real cause was a missing
// negation (see VR_DrawHands). Fixing the cause removed the need for it.
// +90, set from live testing. -45 was tried first and went the WRONG WAY,
// so the sign here is empirical, not derived: positive rotates the mesh the
// direction that actually corrects it on this hardware. (Bone-chain
// measurement says the wrist->fingertip axis sits 68.7 deg off model +X,
// which brackets this magnitude, but the sign convention that reaches the
// renderer passes through a pitch pre-negation here plus another inside
// R_StudioSetUpTransform - so trust the headset over the derivation.)
// Applied as a local pre-rotation, never euler addition (see VR_DrawHands).
// NOTE: this default is largely cosmetic - the cvar is FCVAR_ARCHIVE, so
// config.cfg restores the last live value at startup and overrides it. The
// authoritative value is set in valve/vrbinds.cfg, which is exec'd after
// config.cfg. Change it there; no rebuild needed.
static CVAR_DEFINE_AUTO( vr_hand_pitch_offset, "-90", FCVAR_ARCHIVE, "hand mesh rest-pose pitch correction, degrees" );
static CVAR_DEFINE_AUTO( vr_hand_yaw_offset,   "0",  FCVAR_ARCHIVE, "hand/weapon mesh calibration: yaw offset in degrees" );
static CVAR_DEFINE_AUTO( vr_hand_roll_offset,  "0",  FCVAR_ARCHIVE, "hand/weapon mesh calibration: roll offset in degrees" );

// Pivot correction, expressed as a point in the MESH's own local space.
//
// Purpose: make the mesh rotate about the player's real pivot (the palm)
// instead of about whatever point the model happens to be built around. If
// these are wrong in either direction the mesh sweeps an arc when rotated -
// reported live as "swings too wide, like it's rolling around a softball".
//
// Default 0,0,0 deliberately. A previous 2.5-unit forward default ASSUMED
// the mesh origin sits at the wrist; the live "softball" report is evidence
// it actually sits at or near the palm already, in which case any nonzero
// value CREATES the lever arm rather than removing it. Zero = rotate about
// the model's own origin, which is the correct behaviour if that origin is
// already the palm.
//
// The offset FORMULA is Lambda1VR's (see the call site). The MAGNITUDE is
// not - theirs is 5, tuned for their own models/v_hand.mdl at their scale.
//
// MEASURED, not guessed: v_hand_hevsuit.mdl's hands bodypart spans only
// X = -2.36 .. +4.37, i.e. 6.73 units end to end, with the model origin
// already sitting 35% along that span (the wrist/palm junction). A 5-unit
// pullback is therefore ~74% of the entire hand length, throwing the
// rotation centre way out past the mesh - reported live as "the pivot point
// is in the finger tips". Blindly copying another project's constant across
// a different model at a different scale was the error.
//
// Pivot correction, expressed as a POINT IN THE MESH'S OWN LOCAL SPACE
// (GoldSrc model space: +X forward, +Y left, +Z up).
//
// MEASURED, not guessed. v_hand_hevsuit.mdl's sequence 0 frame 0 pose was
// evaluated through the full bone chain - GoldSrc vertices are stored
// BONE-LOCAL, so a raw vertex bbox is meaningless and an earlier "measured"
// constant here was taken from exactly that mistake. Validation: the
// computed posed bbox (-8.705,-3.315,-3.918)..(-1.130,1.006,5.819) matches
// the bbox the file itself stores for sequence 0 exactly.
//
// The result overturns the earlier assumptions in this file: the origin is
// neither at the wrist nor the palm. The whole mesh lies BEHIND the origin
// along +X, with the origin 1.255 units PAST THE THUMB TIP - so drawing at
// the raw tracked point pivots the hand about its own fingertips, the live
// report verbatim. Lambda1VR's "-5*forward" made it worse, pushing the
// pivot ~10.1 units in front of the palm.
//
// This CANNOT be a scalar along forward: the palm is 5.14 fwd / 1.06 right /
// 1.06 down from the origin, and the hand's long axis points 68.7 degrees
// BELOW model +X (independently confirmed by the flashlight bones
// muzzle_pos -> muzzle_pos2, a beam 64.4 degrees below +X).
//
// Defaults are the PALM CENTRE. For a WRIST pivot use -5.828/-0.987/2.827.
// Pose-invariant: the wrist joint measures identically across all 9
// sequences, so these survive finger-curl blending. v_hand_labcoat.mdl is
// the same rig within 0.03 units - no special case needed.
// WEAPON mesh rest-pose correction. Separate from the hand's, because the
// bare-hand mesh and the weapon viewmodels are different models with
// different rest orientations - live proof: with the hands finally correct
// at vr_hand_pitch_offset -45, the equipped gun still hung ~45 degrees
// below horizontal. Same mechanism (local pre-rotation), own number.
// Authoritative value lives in valve/vrbinds.cfg - see the note there.
static CVAR_DEFINE_AUTO( vr_weapon_pitch_offset, "-45", FCVAR_ARCHIVE, "weapon viewmodel rest-pose pitch correction, degrees" );
static CVAR_DEFINE_AUTO( vr_weapon_yaw_offset,   "0",   FCVAR_ARCHIVE, "weapon viewmodel rest-pose yaw correction, degrees" );
static CVAR_DEFINE_AUTO( vr_weapon_roll_offset,  "0",   FCVAR_ARCHIVE, "weapon viewmodel rest-pose roll correction, degrees" );

// TWO-HANDED WEAPON STABILISATION.
//
// Ported from Lambda1VR (VrInputAlt2.c:183-196): when the off hand is
// brought up to the weapon, aim is taken from the VECTOR BETWEEN THE TWO
// HANDS instead of from the dominant controller's own orientation. That is
// what makes a rifle feel shouldered - small wrist jitter on the firing
// hand stops throwing the muzzle around, because the barrel now follows the
// long baseline between both hands.
//
// The min distance is Lambda1VR's guard, and it matters: with the hands
// close together (a pistol in a two-handed grip) the baseline is too short
// to derive a stable direction from, and the aim would snap around wildly.
// Below the minimum we fall back to normal one-handed aim.
//
// Distances are HL units (1 unit ~= 1 inch). Lambda1VR uses metres:
// its 0.15 m guard ~= 6 units, and its 0.50 m engage range ~= 20 units.
// A straight port of that is NOT enough on its own, and live testing showed
// why: taking aim from the hand-to-hand vector unconditionally makes the gun
// CHASE the off hand - wherever the off hand goes, the muzzle snaps to point
// at it. Reported as "gun snaps to offhand = bad, offhand grabs gun = good".
//
// So engagement is gated on the off hand actually being ON THE BARREL LINE:
// the hand-to-hand direction must lie within a cone around the weapon's
// CURRENT forward. That is the difference between the off hand grabbing a
// foregrip that is already there, and the gun swinging over to find the
// hand. Outside the cone we simply stay one-handed.
//
// Engagement is also blended over vr_twohand_smooth seconds rather than
// switching hard, so grabbing and releasing eases in instead of popping.
//
// Distances are HL units (1 unit ~= 1 inch). Lambda1VR uses metres:
// its 0.15 m guard ~= 6 units, and its 0.50 m engage range ~= 20 units.
// Engagement is a GEOMETRY test, not a proximity cone: the off hand has to
// actually be on the weapon. The gun is modelled as a line segment running
// from the grip out along the barrel, and the hand must come within
// vr_twohand_radius of that segment - i.e. the hand and the weapon have to
// physically meet, which is what a real support grip is.
//
// A cone was tried first and is wrong at close range: a cone is infinitely
// wide far away and infinitely tight up close, so the hand could be a foot
// off the barrel and still count as long as the angle was small.
static CVAR_DEFINE_AUTO( vr_twohand,        "1",  FCVAR_ARCHIVE, "two-handed weapon stabilisation (aim along the hand-to-hand vector)" );
static CVAR_DEFINE_AUTO( vr_twohand_min,    "5",  FCVAR_ARCHIVE, "two-hand: min hand separation to engage, HL units" );
static CVAR_DEFINE_AUTO( vr_twohand_max,    "30", FCVAR_ARCHIVE, "two-hand: max hand separation to stay engaged, HL units" );
static CVAR_DEFINE_AUTO( vr_twohand_radius, "5",  FCVAR_ARCHIVE, "two-hand: how close the off hand must come to the barrel line, HL units" );
static CVAR_DEFINE_AUTO( vr_twohand_barrel, "22", FCVAR_ARCHIVE, "two-hand: FALLBACK barrel length, used only when the model has no attachment - otherwise the real grip-to-muzzle distance wins" );
static CVAR_DEFINE_AUTO( vr_twohand_smooth, "0.12", FCVAR_ARCHIVE, "two-hand: engage/release blend time, seconds (0 = instant)" );

// ---------------------------------------------------------------------
// LASER SIGHT + GRENADE ARC
//
// Both are drawn through the TriAPI from inside pfnDrawNormalTriangles
// (see VR_DrawOverlays), which runs during the 3D pass with the world
// transform already set, once per eye, and is engine-side - so this works
// regardless of which mod is loaded.
//
// The arc is a real ballistic simulation stepped against the world with
// CL_TraceLine, not a decorative curve: it uses the same gravity the server
// applies, so where the line ends is where the grenade lands.
// ---------------------------------------------------------------------
static CVAR_DEFINE_AUTO( vr_laser,        "1",   FCVAR_ARCHIVE, "laser sight: 0 off, 1 dot only, 2 dot + beam" );
static CVAR_DEFINE_AUTO( vr_laser_r,      "1.0", FCVAR_ARCHIVE, "laser sight colour, red 0-1" );
static CVAR_DEFINE_AUTO( vr_laser_g,      "0.1", FCVAR_ARCHIVE, "laser sight colour, green 0-1" );
static CVAR_DEFINE_AUTO( vr_laser_b,      "0.1", FCVAR_ARCHIVE, "laser sight colour, blue 0-1" );
static CVAR_DEFINE_AUTO( vr_laser_alpha,  "0.5", FCVAR_ARCHIVE, "laser beam opacity 0-1 (the dot is always solid)" );
static CVAR_DEFINE_AUTO( vr_laser_width,  "0.35", FCVAR_ARCHIVE, "laser beam half-width, HL units" );
static CVAR_DEFINE_AUTO( vr_laser_dot,    "1.6", FCVAR_ARCHIVE, "laser impact dot radius, HL units" );
static CVAR_DEFINE_AUTO( vr_laser_range,  "4096", FCVAR_ARCHIVE, "laser sight max range, HL units" );

static CVAR_DEFINE_AUTO( vr_arc,          "1",   FCVAR_ARCHIVE, "grenade trajectory arc: 0 off, 1 on when holding a throwable" );
static CVAR_DEFINE_AUTO( vr_arc_speed,    "500", FCVAR_ARCHIVE, "assumed throw speed for the arc, HL units/sec (HL grenade is ~500 + view velocity)" );
static CVAR_DEFINE_AUTO( vr_arc_gravity,  "800", FCVAR_ARCHIVE, "gravity used for the arc, HL units/sec^2 (sv_gravity default 800)" );
static CVAR_DEFINE_AUTO( vr_arc_steps,    "48",  FCVAR_ARCHIVE, "arc simulation steps (higher = smoother)" );
static CVAR_DEFINE_AUTO( vr_arc_step_time,"0.06", FCVAR_ARCHIVE, "arc simulation timestep, seconds" );
static CVAR_DEFINE_AUTO( vr_arc_width,    "0.6", FCVAR_ARCHIVE, "arc ribbon half-width, HL units" );
static CVAR_DEFINE_AUTO( vr_arc_r,        "0.2", FCVAR_ARCHIVE, "arc colour, red 0-1" );
static CVAR_DEFINE_AUTO( vr_arc_g,        "0.9", FCVAR_ARCHIVE, "arc colour, green 0-1" );
static CVAR_DEFINE_AUTO( vr_arc_b,        "1.0", FCVAR_ARCHIVE, "arc colour, blue 0-1" );
static CVAR_DEFINE_AUTO( vr_arc_alpha,    "0.65", FCVAR_ARCHIVE, "arc opacity 0-1" );

// Per-weapon rest-pose pitch. A single global value cannot be right for
// every viewmodel: Lambda1VR carries separate vr_weapon_pitchadjust (-20)
// and vr_crowbar_pitchadjust (-25) for exactly this reason. Melee weapons
// are held like a tool, not aimed like a gun, so they need their own angle.
static CVAR_DEFINE_AUTO( vr_melee_pitch_offset, "-70", FCVAR_ARCHIVE, "melee weapon rest-pose pitch correction, degrees" );

// Swing-to-hit. Threshold is in HL units/sec of real hand movement; a
// deliberate swing runs well past this while ordinary hand drift does not.
static CVAR_DEFINE_AUTO( vr_melee_swing, "1",   FCVAR_ARCHIVE, "melee: swing the controller to attack" );
static CVAR_DEFINE_AUTO( vr_melee_speed, "220", FCVAR_ARCHIVE, "melee: hand speed needed to register a swing, HL units/sec" );

// Fire along the WEAPON, not along the head.
//
// The mod's weapon code traces the shot from the player's view origin along
// cmd->viewangles - so with viewangles driven by the HMD, bullets come out
// of your face no matter where the gun is pointed. Reported live as exactly
// that. Sending the weapon's aim as viewangles instead makes shots follow
// the gun, and costs nothing visually because the rendered view comes from
// the HMD pose in VR_BeginEye, not from viewangles.
//
// The catch is that viewangles ALSO define the movement frame, so the stick
// would suddenly walk you relative to the muzzle. VR_GetMovement's output is
// counter-rotated by the head/weapon yaw difference to cancel that out.
static CVAR_DEFINE_AUTO( vr_aim_from_weapon, "1", FCVAR_ARCHIVE, "fire along the weapon instead of the head" );

// Master switch for the view_ofs substitution that moves the shot ORIGIN to
// the muzzle (sv_pmove.c and pfnLocalPlayerViewheight). Separate from
// vr_aim_from_weapon, which only changes DIRECTION, so the two can be
// bisected independently when something breaks.
static CVAR_DEFINE_AUTO( vr_weapon_origin, "1", FCVAR_ARCHIVE, "shot ORIGIN at the muzzle (0 = stock, from the eye)" );
// ---------------------------------------------------------------------
// Room-scale, comfort and handedness.
//
// Everything here is engine-side and reads only tracking data, so it works on
// any mod. That is the line this fork holds: HLVR and Lambda1VR get physical
// ladders and grabbing by forking the game SDK, which then only fixes the one
// mod it was built for.
// ---------------------------------------------------------------------
static CVAR_DEFINE_AUTO( vr_grip_offset_up, "-2", FCVAR_ARCHIVE, "draw the grip hand this far above/below the barrel line, units" );
static CVAR_DEFINE_AUTO( vr_grip_offset_fwd, "0", FCVAR_ARCHIVE, "draw the grip hand this far along the barrel, units" );
static CVAR_DEFINE_AUTO( vr_grip_offset_side, "0", FCVAR_ARCHIVE, "draw the grip hand this far to the side of the barrel, units" );
static CVAR_DEFINE_AUTO( vr_grip_pose, "1", FCVAR_ARCHIVE, "close the supporting hand and wrap it around the barrel" );
static CVAR_DEFINE_AUTO( vr_grip_pose_pitch, "0", FCVAR_ARCHIVE, "grip pose pitch offset" );
static CVAR_DEFINE_AUTO( vr_grip_pose_yaw, "0", FCVAR_ARCHIVE, "grip pose yaw offset" );
static CVAR_DEFINE_AUTO( vr_grip_pose_roll, "0", FCVAR_ARCHIVE, "grip pose roll offset" );
static CVAR_DEFINE_AUTO( vr_grip_snap, "1", FCVAR_ARCHIVE, "post the supporting hand onto the barrel while two-handing (0-1)" );
static CVAR_DEFINE_AUTO( vr_touch_hold, "0.3", FCVAR_ARCHIVE, "seconds contact is held after the hand stops touching" );
static CVAR_DEFINE_AUTO( vr_touch_backoff, "24", FCVAR_ARCHIVE, "units behind the hand the use ray starts, so large objects still register" );
static CVAR_DEFINE_AUTO( vr_touch_reach, "3", FCVAR_ARCHIVE, "how far past your hand counts as touching, units" );
static CVAR_DEFINE_AUTO( vr_touch_los, "1", FCVAR_ARCHIVE, "require a clear line from eye to hand before the hand can use anything" );
static CVAR_DEFINE_AUTO( vr_touch_use, "1", FCVAR_ARCHIVE, "press buttons and levers with your hand instead of the crosshair" );
static CVAR_DEFINE_AUTO( vr_step_smooth, "12", FCVAR_ARCHIVE, "ease the view up steps instead of snapping (0 = off)" );
static CVAR_DEFINE_AUTO( vr_roomscale, "1", FCVAR_ARCHIVE, "walking in your room walks in the game" );
static CVAR_DEFINE_AUTO( vr_roomscale_gain, "1", FCVAR_ARCHIVE, "how hard the body chases your real position" );
static CVAR_DEFINE_AUTO( vr_neck_model, "1", FCVAR_ARCHIVE, "drive room-scale from the base of the neck, so tilting your head does not walk you" );
static CVAR_DEFINE_AUTO( vr_neck_up, "8", FCVAR_ARCHIVE, "units from the headset down to the neck pivot" );
static CVAR_DEFINE_AUTO( vr_neck_fwd, "4", FCVAR_ARCHIVE, "units from the headset back to the neck pivot" );
static CVAR_DEFINE_AUTO( vr_roomscale_noise, "0.03", FCVAR_ARCHIVE, "per-frame head motion below this is tracker noise, units" );
static CVAR_DEFINE_AUTO( vr_roomscale_max, "600", FCVAR_ARCHIVE, "cap on room-scale move speed" );
static CVAR_DEFINE_AUTO( vr_lefthand, "0", FCVAR_ARCHIVE, "left-handed: weapon in the left hand" );
static CVAR_DEFINE_AUTO( vr_height, "68", FCVAR_ARCHIVE, "your standing eye height in units, ~1 unit per inch" );
static CVAR_DEFINE_AUTO( vr_height_offset, "0", FCVAR_ARCHIVE, "shift the view up or down from the tracked height" );
static CVAR_DEFINE_AUTO( vr_seated, "0", FCVAR_ARCHIVE, "seated play: no physical crouch, and the view is raised to standing height" );
static CVAR_DEFINE_AUTO( vr_seated_lift, "0", FCVAR_ARCHIVE, "extra height for seated play; normally 0 - the view is already anchored to the mod's eye position, so lifting it only makes the player tall" );
static CVAR_DEFINE_AUTO( vr_crouch, "1", FCVAR_ARCHIVE, "duck by physically ducking" );
static CVAR_DEFINE_AUTO( vr_crouch_ratio, "0.75", FCVAR_ARCHIVE, "fraction of standing height that counts as crouched" );
static CVAR_DEFINE_AUTO( vr_walkdirection, "0", FCVAR_ARCHIVE, "0 = walk where you look, 1 = walk where your off hand points" );
static CVAR_DEFINE_AUTO( vr_ladder, "1", FCVAR_ARCHIVE, "climb ladders by pulling with your hands" );
static CVAR_DEFINE_AUTO( vr_ladder_hands, "1", FCVAR_ARCHIVE, "both hands on the ladder: stows the weapon and disables stick climbing" );
static CVAR_DEFINE_AUTO( vr_ladder_speed, "1", FCVAR_ARCHIVE, "hand pull to climb ratio; 1 = your hand moves you 1:1" );
static CVAR_DEFINE_AUTO( vr_ladder_hands_only, "1", FCVAR_ARCHIVE, "ladders can ONLY be climbed by hand; the stick will not do it" );
static CVAR_DEFINE_AUTO( vr_ladder_max, "180", FCVAR_ARCHIVE, "fastest a pull can climb, units/sec" );
static CVAR_DEFINE_AUTO( vr_vignette, "1", FCVAR_ARCHIVE, "comfort vignette that closes in while moving" );
static CVAR_DEFINE_AUTO( vr_vignette_size, "0.62", FCVAR_ARCHIVE, "how much of the view stays clear at full speed" );
static CVAR_DEFINE_AUTO( vr_vignette_fade, "6", FCVAR_ARCHIVE, "how fast the vignette opens and closes" );

static CVAR_DEFINE_AUTO( vr_dual_wield, "0", FCVAR_ARCHIVE, "VR akimbo: off hand fires the same weapon (needs our hl.dll)" );
static CVAR_DEFINE_AUTO( vr_offhand_muzzle, "8", FCVAR_ARCHIVE, "units forward of the off hand the second shot leaves from" );

// One-shot dump of the actual numbers going into aim, to vr_diag.log.
static CVAR_DEFINE_AUTO( vr_diag_aim, "0", 0, "log muzzle/aim/overlay state for N frames" );

// Aim pitch, SEPARATE from the mesh pitch (vr_weapon_pitch_offset).
//
// Two attempts to derive aim from the mesh correction both failed live:
// using the raw pose put the laser 45 deg above the barrel, and applying the
// mesh correction on top made it 90 deg. Each application of the mesh's -45
// rotated aim UP by 45, so aim needs the opposite sign, not the same value.
//
// The mesh correction and the aim correction are simply not the same number:
// the mesh one rotates the MODEL so it looks seated in the hand, and the
// barrel sits at its own angle WITHIN that model. Tying them together was
// the mistake. This is measured off the laser and tuned on its own.
// Derive the firing line from the model's muzzle attachment (exact,
// per-weapon, no tuning) rather than a hand-tuned angle. 0 falls back to
// the vr_aim_*_offset values below.
// Rotate the drawn weapon so its barrel lies on the actual firing line.
static CVAR_DEFINE_AUTO( vr_model_align, "0.15", FCVAR_ARCHIVE, "align weapon model to the firing line; value is convergence gain per frame, 0 = off" );
static CVAR_DEFINE_AUTO( vr_aim_attachment, "1", FCVAR_ARCHIVE, "aim from the weapon model's muzzle attachment" );
static CVAR_DEFINE_AUTO( vr_aim_pitch_offset, "45", FCVAR_ARCHIVE, "aim pitch correction, degrees (tune until the laser lies on the barrel)" );
static CVAR_DEFINE_AUTO( vr_aim_yaw_offset,   "0",  FCVAR_ARCHIVE, "aim yaw correction, degrees" );

// Haptics. Value doubles as a master gain, so 0 disables and 0.5 halves.
static CVAR_DEFINE_AUTO( vr_haptics, "1", FCVAR_ARCHIVE, "haptic feedback strength, 0 = off" );

// Off-hand flashlight: 0 = stock head-mounted, 1 = off hand, 2 = weapon hand.
static CVAR_DEFINE_AUTO( vr_flashlight_hand, "0", FCVAR_ARCHIVE, "flashlight mount: 0 head, 1 off hand, 2 weapon hand" );

static CVAR_DEFINE_AUTO( vr_hand_pivot_fwd,  "-5.139", FCVAR_ARCHIVE, "hand mesh pivot point, model-space X (forward), HL units" );
static CVAR_DEFINE_AUTO( vr_hand_pivot_left, "-1.059", FCVAR_ARCHIVE, "hand mesh pivot point, model-space Y (left), HL units" );
static CVAR_DEFINE_AUTO( vr_hand_pivot_up,   "1.063",  FCVAR_ARCHIVE, "hand mesh pivot point, model-space Z (up), HL units" );



/*
=================================================================
	minimal GL entry points

The engine does not normally link OpenGL - ref_gl owns all of that.
We need only enough to wrap an OpenXR swapchain texture in an FBO.
Resolved through SDL so we don't drag in a GL loader.
=================================================================
*/
typedef unsigned int GLenum_t;
typedef unsigned int GLuint_t;
typedef int          GLsizei_t;
typedef int          GLint_t;
typedef unsigned int GLbitfield_t;

#define GL_FRAMEBUFFER_EXT          0x8D40
#define GL_COLOR_ATTACHMENT0_EXT    0x8CE0
#define GL_DEPTH_ATTACHMENT_EXT     0x8D00
#define GL_RENDERBUFFER_EXT         0x8D41
#define GL_DEPTH_COMPONENT24_EXT    0x81A6
#define GL_FRAMEBUFFER_COMPLETE_EXT 0x8CD5
#define GL_TEXTURE_2D_T             0x0DE1
#define GL_READ_FRAMEBUFFER_T       0x8CA8
#define GL_DRAW_FRAMEBUFFER_T       0x8CA9
#define GL_COLOR_BUFFER_BIT_T       0x00004000
#define GL_LINEAR_T                 0x2601
#define GL_NEAREST_T                0x2600
#define GL_DEPTH_BUFFER_BIT_T       0x00000100
#define GL_RGBA8_T                  0x8058
#define GL_DEPTH_COMPONENT16_T      0x81A5
#define GL_DEPTH_COMPONENT32F_T     0x8CAC
#define GL_DEPTH24_STENCIL8_T       0x88F0

static struct
{
	void (APIENTRY *GenFramebuffers)( GLsizei_t, GLuint_t * );
	void (APIENTRY *DeleteFramebuffers)( GLsizei_t, const GLuint_t * );
	void (APIENTRY *BindFramebuffer)( GLenum_t, GLuint_t );
	void (APIENTRY *FramebufferTexture2D)( GLenum_t, GLenum_t, GLenum_t, GLuint_t, GLint_t );
	void (APIENTRY *GenRenderbuffers)( GLsizei_t, GLuint_t * );
	void (APIENTRY *DeleteRenderbuffers)( GLsizei_t, const GLuint_t * );
	void (APIENTRY *BindRenderbuffer)( GLenum_t, GLuint_t );
	void (APIENTRY *RenderbufferStorage)( GLenum_t, GLenum_t, GLsizei_t, GLsizei_t );
	void (APIENTRY *FramebufferRenderbuffer)( GLenum_t, GLenum_t, GLenum_t, GLuint_t );
	GLenum_t (APIENTRY *CheckFramebufferStatus)( GLenum_t );
	void (APIENTRY *Viewport)( GLint_t, GLint_t, GLsizei_t, GLsizei_t );
	void (APIENTRY *BlitFramebuffer)( GLint_t, GLint_t, GLint_t, GLint_t,
		GLint_t, GLint_t, GLint_t, GLint_t, GLbitfield_t, GLenum_t );
	// Optional - MSAA needs it, everything else works without it.
	void (APIENTRY *RenderbufferStorageMultisample)( GLenum_t, GLsizei_t,
		GLenum_t, GLsizei_t, GLsizei_t );
	qboolean loaded;
} vrgl;

static qboolean VR_LoadGLFuncs( void )
{
	if( vrgl.loaded )
		return true;

#define GETPROC( x, n ) \
	vrgl.x = (void *)SDL_GL_GetProcAddress( n ); \
	if( !vrgl.x ) { Con_Printf( S_ERROR "VR: missing GL entry point %s\n", n ); return false; }

	GETPROC( GenFramebuffers,        "glGenFramebuffers" )
	GETPROC( DeleteFramebuffers,     "glDeleteFramebuffers" )
	GETPROC( BindFramebuffer,        "glBindFramebuffer" )
	GETPROC( FramebufferTexture2D,   "glFramebufferTexture2D" )
	GETPROC( GenRenderbuffers,       "glGenRenderbuffers" )
	GETPROC( DeleteRenderbuffers,    "glDeleteRenderbuffers" )
	GETPROC( BindRenderbuffer,       "glBindRenderbuffer" )
	GETPROC( RenderbufferStorage,    "glRenderbufferStorage" )
	GETPROC( FramebufferRenderbuffer,"glFramebufferRenderbuffer" )
	GETPROC( CheckFramebufferStatus, "glCheckFramebufferStatus" )
	GETPROC( Viewport,               "glViewport" )
	GETPROC( BlitFramebuffer,        "glBlitFramebuffer" )
#undef GETPROC

	// Not fatal if absent: without it MSAA is unavailable, which is a quality
	// setting rather than a requirement for putting a frame on the headset.
	vrgl.RenderbufferStorageMultisample =
		(void *)SDL_GL_GetProcAddress( "glRenderbufferStorageMultisample" );

	vrgl.loaded = true;
	return true;
}

/*
=================================================================
	state
=================================================================
*/
// Semantic actions. OpenXR binds these per controller type, so one set of code
// drives Touch, Index, Vive wands and WMR without per-device branching.
typedef enum
{
	VRA_MOVE = 0,	// vec2 - left stick
	VRA_TURN,	// vec2 - right stick
	VRA_JUMP,
	VRA_CROUCH,
	VRA_ATTACK,
	VRA_ATTACK2,
	VRA_USE,
	VRA_RELOAD,
	VRA_FLASHLIGHT,
	VRA_NEXTWEAP,
	VRA_PREVWEAP,
	VRA_MENU,
	VRA_OFFGRIP,	// off-hand grip: grab / two-hand a weapon
	VRA_COUNT
} vr_action_id_t;

static const struct
{
	const char *name;
	const char *label;
	int         type;	// XrActionType
} vr_action_defs[VRA_COUNT] =
{
	{ "move",       "Move",           XR_ACTION_TYPE_VECTOR2F_INPUT },
	{ "turn",       "Turn",           XR_ACTION_TYPE_VECTOR2F_INPUT },
	{ "jump",       "Jump",           XR_ACTION_TYPE_BOOLEAN_INPUT },
	{ "crouch",     "Crouch",         XR_ACTION_TYPE_BOOLEAN_INPUT },
	{ "attack",     "Attack",         XR_ACTION_TYPE_BOOLEAN_INPUT },
	{ "attack2",    "Secondary Fire", XR_ACTION_TYPE_BOOLEAN_INPUT },
	{ "use",        "Use",            XR_ACTION_TYPE_BOOLEAN_INPUT },
	{ "reload",     "Reload",         XR_ACTION_TYPE_BOOLEAN_INPUT },
	{ "flashlight", "Flashlight",     XR_ACTION_TYPE_BOOLEAN_INPUT },
	{ "nextweap",   "Next Weapon",    XR_ACTION_TYPE_BOOLEAN_INPUT },
	{ "prevweap",   "Prev Weapon",    XR_ACTION_TYPE_BOOLEAN_INPUT },
	{ "menu",       "Menu",           XR_ACTION_TYPE_BOOLEAN_INPUT },
	{ "offgrip",    "Off-hand Grip",  XR_ACTION_TYPE_BOOLEAN_INPUT },
};

typedef struct
{
	XrSwapchain            handle;
	uint32_t               width, height;
	uint32_t               image_count;
	XrSwapchainImageOpenGLKHR *images;   // GL texture names
	GLuint_t              *fbos;         // one FBO per swapchain image
	GLuint_t               depth_rb;     // private depth, when not submitting it
	uint32_t               acquired_index;

	// Multisampled render target. The eye is drawn here and resolved into the
	// swapchain image at the end, because runtimes do not hand out
	// multisampled swapchains and the resolve has to happen somewhere.
	GLuint_t               msaa_fbo;
	GLuint_t               msaa_color;
	GLuint_t               msaa_depth;
	int                    samples;      // 0 = no MSAA

	// Depth handed to the compositor (XR_KHR_composition_layer_depth).
	XrSwapchain            depth_handle;
	XrSwapchainImageOpenGLKHR *depth_images;
	uint32_t               depth_image_count;
	uint32_t               depth_acquired_index;
	qboolean               depth_ready;  // acquired and usable THIS frame
} vr_swapchain_t;

static struct
{
	qboolean      available;      // instance exists
	qboolean      have_system;    // xrGetSystem succeeded (an HMD is present)
	qboolean      session_ready;  // session created
	qboolean      running;        // session state is running; frames may be submitted
	qboolean      frame_started;  // between xrBeginFrame and xrEndFrame
	qboolean      eyes_submitted; // at least one eye was rendered this frame
	int           eyes_this_frame; // how many eyes actually filled proj_views[]
	int           cur_eye;         // eye currently being rendered, for the 2D
	                               // pass - VR_Begin2D composites per eye and
	                               // needs to know which one to bias the HUD's
	                               // stereo disparity toward
	qboolean      menu_frame;     // this frame is the menu-only path (no world)
	double        next_system_retry;
	int           frames_submitted;

	// where the play space sits in the game world, refreshed each frame
	vec3_t        world_origin;
	float         world_yaw;

	// Eased vertical position of the play space. GoldSrc resolves a step by
	// snapping the player up the full step height in a single frame, which on
	// a monitor reads as a small jolt and in a headset reads as your whole
	// body teleporting upward - one of the more reliable ways to make someone
	// ill. Only the EYE is eased; the entity still steps instantly, so nothing
	// about collision or movement changes.
	float         smooth_z;
	qboolean      smooth_z_valid;
	vec3_t        hmd_origin_at_sync;  // real HMD position when world_origin last
	                                    // moved (the player actually walked) - see
	                                    // VR_SetWorldReference. Hand/eye offsets are
	                                    // measured from THIS, not the live HMD pose,
	                                    // so real head translation between walk-steps
	                                    // (leaning, natural movement) is preserved
	                                    // instead of being silently cancelled out.
	qboolean      hmd_origin_at_sync_valid;
	vec3_t        roomscale_cmd;  // world-space move room-scale asked the body for
	                              // this frame, published by VR_GetRoomScaleMove and
	                              // CONSUMED (zeroed) by VR_SetWorldReference. The
	                              // body moves for many reasons - stick, trains,
	                              // conveyors, basevelocity, NPC pushes - and only
	                              // room-scale's own share may close the loop; see
	                              // VR_SetWorldReference.
	qboolean      roomscale_cmd_valid;
	vec3_t        neck_prev;        // neck position last frame, play space
	qboolean      neck_prev_valid;
	float         body_yaw;       // play-space rotation in world (mouse/stick turn)
	float         injected_yaw;   // head yaw written into cl.viewangles last frame
	float         turn_delta;     // stick turn accumulated this frame, consumed by
	                              // VR_OverrideViewAngles

	XrInstance    instance;
	XrSystemId    system;
	XrSession     session;
	XrSpace       stage_space;    // room-scale reference space
	XrSpace       view_space;     // HMD-relative

	XrSessionState session_state;

	int           eye_count;
	XrViewConfigurationView view_configs[VR_MAX_EYES];
	XrView        views[VR_MAX_EYES];
	XrCompositionLayerProjectionView proj_views[VR_MAX_EYES];
	XrCompositionLayerDepthInfoKHR   depth_info[VR_MAX_EYES];
	qboolean      have_depth_ext;  // runtime accepts a depth layer
	float         applied_ss;      // render scale the swapchains were built at
	int           applied_msaa;    // and the sample count, to notice a change
	vr_swapchain_t swapchains[VR_MAX_EYES];

	XrFrameState  frame_state;

	vr_pose_t     hmd_pose;
	vr_pose_t     hand_pose[2];      // AIM pose - for weapon pointing direction
	vr_pose_t     hand_grip_pose[2]; // GRIP pose - for rendering held meshes (hands, weapon model)

	int           gl_major, gl_minor;

	// ---- input ----
	qboolean      input_ready;
	XrActionSet   action_set;
	XrAction      actions[VRA_COUNT];
	XrAction      act_hand_pose;    // aim/pose
	XrAction      act_hand_grip;    // grip/pose
	XrAction      act_haptic[2];    // vibration output, 0 = left, 1 = right
	XrPath        hand_path[2];  // /user/hand/left, /user/hand/right
	XrSpace       hand_space[2];
	XrSpace       grip_space[2];

	float         move_x, move_y;   // -1..1 locomotion stick
	float         turn_x, turn_y;   // -1..1 turn stick
	qboolean      select_open;       // weapon select HUD up (grip + stick click)
	qboolean      sh_inside;        // dominant hand is in the shoulder hotspot
	qboolean      sh_light_inside;  // off hand is in the flashlight hotspot
	qboolean      sh_swapped;       // we swapped to melee from the hotspot

	// Physical reloading. One round in the hand at a time, which covers a
	// magazine (one insert finishes it) and a shell tube (repeat) with the
	// same two moves, so neither needs to know which it is.
	qboolean      rl_holding;       // a round is in the off hand
	qboolean      rl_insert;        // an insert completed THIS frame
	int           rl_clip;          // last clip count seen from CurWeapon

	// Working the action. Separate from reloading because it gates FIRING,
	// and a pump gun needs it after every shot rather than only when empty.
	qboolean      act_needs;        // the action is spent and must be worked
	qboolean      act_armed;        // a hand has taken hold of it
	float         act_ref;          // where along the weapon it took hold
	int           act_clip;         // clip last frame, to notice a shot
	int           act_id;           // and which weapon it belonged to
	qboolean      act_worked;       // the action was worked THIS frame
	float         act_pull;         // 0..1, how far back the action is being held
	qboolean      act_back;         // it has been drawn fully back, awaiting the return
	int           sh_restore_id;    // viewmodel index to walk back to
	int           sh_restore_tries; // attempts left before giving up
	int           sh_seen_id;       // viewmodel index when the last invnext went out
	double        sh_step_time;     // when that step was issued
	int           sh_phase;         // 0 highlight, 1 press, 2 release
	qboolean      ladder_gripping;  // a hand is actually holding a rung
	vec3_t        ladder_center;    // middle of the rungs we are on
	vec3_t        ladder_size;      // its extents, to find which way it faces
	qboolean      ladder_have_dir;  // ladder_center is good this frame
	float         ladder_climb;     // last computed climb, units/sec
	char          sh_restore_name[32]; // weapon to jump straight back to
	float         select_fastswitch; // player's hud_fastswitch, restored on close
	qboolean      btn[VRA_COUNT];
	qboolean      btn_prev[VRA_COUNT];
	qboolean      snap_pending;
	qboolean      turn_active;
	qboolean      profiles_logged;
	int           session_retries;
	HANDLE        instance_mutex;	// turn action reported bound+active by the runtime

	// PFN cache
	PFN_xrGetOpenGLGraphicsRequirementsKHR pfnGetOpenGLGraphicsRequirementsKHR;
} vr;

/*
=================================================================
	diagnostics

Nobody can read the console while wearing a headset, so VR problems are
otherwise invisible. Everything interesting is mirrored into vr_diag.log next
to the executable: lifecycle events, periodic pose samples, submission
statistics, anomaly detection, and a summary block on shutdown.
=================================================================
*/
static struct
{
	FILE    *fp;
	double   next_sample;
	double   session_start;
	int      samples;

	// running extremes, to spot clamping / drift / frozen tracking
	float    ang_min[3], ang_max[3];
	vec3_t   pos_min, pos_max;
	vec3_t   last_pos;
	int      frozen_count;

	int      err_acquire, err_wait, err_locate, err_endframe;
	int      frames_no_render;
	double   last_frame_time;
	float    fps_accum;
	int      fps_frames;
} vrdiag;

// Annotated so the compiler validates format/argument agreement. A mismatch
// here previously shipped as a hard crash inside strnlen when %s consumed a
// garbage pointer - a format specifier had been added without its argument.
#ifdef _MSC_VER
static void VR_DiagPrintf( _Printf_format_string_ const char *fmt, ... );
#else
static void VR_DiagPrintf( const char *fmt, ... ) __attribute__(( format( printf, 1, 2 )));
#endif

static void VR_DiagPrintf( const char *fmt, ... )
{
	va_list args;

	if( !vrdiag.fp )
		return;

	va_start( args, fmt );
	vfprintf( vrdiag.fp, fmt, args );
	va_end( args );
	fflush( vrdiag.fp );	// crash-safe: we want the tail even after a hard fault
}

static void VR_DiagOpen( void )
{
	int i;

	if( !vr_diag.value )
		return;

	vrdiag.fp = fopen( "vr_diag.log", "w" );
	if( !vrdiag.fp )
		return;

	for( i = 0; i < 3; i++ )
	{
		vrdiag.ang_min[i] =  99999.0f;
		vrdiag.ang_max[i] = -99999.0f;
		vrdiag.pos_min[i] =  99999.0f;
		vrdiag.pos_max[i] = -99999.0f;
	}

	VR_DiagPrintf( "================================================================\n" );
	VR_DiagPrintf( " XashVR diagnostic log\n" );
	VR_DiagPrintf( " engine  : %s\n", XASH_VERSION );
	VR_DiagPrintf( " build   : " __DATE__ " " __TIME__ "\n" );
	VR_DiagPrintf( "================================================================\n\n" );
}

static void VR_DiagSummary( void )
{
	if( !vrdiag.fp )
		return;

	VR_DiagPrintf( "\n---------------- SUMMARY ----------------\n" );
	VR_DiagPrintf( "frames submitted : %d\n", vr.frames_submitted );
	VR_DiagPrintf( "pose samples     : %d\n", vrdiag.samples );
	VR_DiagPrintf( "shouldRender==0  : %d frames\n", vrdiag.frames_no_render );

	if( vrdiag.samples > 0 )
	{
		VR_DiagPrintf( "angle range      : pitch %.1f..%.1f  yaw %.1f..%.1f  roll %.1f..%.1f\n",
			vrdiag.ang_min[PITCH], vrdiag.ang_max[PITCH],
			vrdiag.ang_min[YAW],   vrdiag.ang_max[YAW],
			vrdiag.ang_min[ROLL],  vrdiag.ang_max[ROLL] );
		VR_DiagPrintf( "hmd pos range    : x %.1f..%.1f  y %.1f..%.1f  z %.1f..%.1f (units)\n",
			vrdiag.pos_min[0], vrdiag.pos_max[0],
			vrdiag.pos_min[1], vrdiag.pos_max[1],
			vrdiag.pos_min[2], vrdiag.pos_max[2] );
		VR_DiagPrintf( "frozen samples   : %d %s\n", vrdiag.frozen_count,
			vrdiag.frozen_count > 3 ? "  <-- TRACKING MAY HAVE STALLED" : "" );
	}

	VR_DiagPrintf( "errors           : acquire=%d wait=%d locate=%d endframe=%d\n",
		vrdiag.err_acquire, vrdiag.err_wait, vrdiag.err_locate, vrdiag.err_endframe );

	// automatic sanity notes, so the log interprets itself
	if( vrdiag.samples > 4 )
	{
		if( fabs( vrdiag.ang_max[ROLL] - vrdiag.ang_min[ROLL] ) < 0.01f )
			VR_DiagPrintf( "NOTE: roll never changed - head tilt may not be tracking.\n" );
		if( fabs( vrdiag.ang_max[YAW] - vrdiag.ang_min[YAW] ) < 0.01f )
			VR_DiagPrintf( "NOTE: yaw never changed - head turn may not be tracking.\n" );
		if( fabs( vrdiag.pos_max[2] - vrdiag.pos_min[2] ) < 0.01f )
			VR_DiagPrintf( "NOTE: hmd height never changed - positional tracking may be off.\n" );
		if( vr.frames_submitted == 0 )
			VR_DiagPrintf( "NOTE: zero frames submitted - nothing was ever sent to the headset.\n" );
	}

	VR_DiagPrintf( "-----------------------------------------\n" );
}

static void VR_DiagClose( void )
{
	if( !vrdiag.fp )
		return;

	VR_DiagSummary();
	fclose( vrdiag.fp );
	vrdiag.fp = NULL;
}

/*
================
VR_DiagSample

Periodic snapshot of everything that matters, plus anomaly tracking.
================
*/
// Forward declarations: VR_DiagSample (below) reports live hand-mesh state,
// which needs these before their real definitions later in this file.
static qboolean VR_GetHandGripWorld( int hand, vec3_t out_org, vec3_t out_ang );
static model_t   *vr_hand_model_suit;
static model_t   *vr_hand_model_labcoat;
static cl_entity_t vr_hand_ent[2];		// 0 = left, 1 = right (when unarmed)

// The second gun while dual wielding. File scope rather than local to
// VR_DrawOffhandWeapon because the aim path reads it back: the renderer fills
// attachment[] as it draws, which is the ONLY exact source for where this
// weapon's barrel actually points.
static cl_entity_t vr_offhand_ent;
static cl_entity_t vr_round_ent;		// the round being carried to the gun

// VR_AlignModelToFireRay's integrator, at file scope so the periodic diagnostic
// can report it. It winding to its clamp is what inverts the weapon, and that
// happens while DEAD - when the viewmodel is not drawn and any diagnostic
// inside the drawing path therefore never runs.
static float corr_p = 0.0f, corr_y = 0.0f;

// Measured bore offset: the fixed angle between the mesh's barrel and the
// angles the entity is drawn with. See VR_AlignModelToFireRay.
static vec3_t   vr_bore_local;	// bore direction in the MODEL's own frame - a true constant
static vec3_t   vr_bore_last_ang;
static qboolean vr_bore_valid;
static qboolean vr_bore_have_last;

// Where the off hand meets the barrel while two-handing, so the drawn hand can
// be posted onto the weapon instead of passing through it.
static vec3_t   vr_grip_org;
static vec3_t   vr_grip_axis;	// barrel direction at the grip point

static int      VR_ModelAttachments( const model_t *mod );
static qboolean vr_grip_valid;
static qboolean    vr_hands_init_tried;

// Last two-hand grip evaluation, for the periodic diagnostic line. Purely
// observational - lets the failing condition be identified from the log
// instead of guessed at.
static struct
{
	qboolean grip;      // off-hand grip button down
	qboolean offhand;   // off-hand pose tracked
	float    dist;      // hand separation
	float    gap;       // off hand's distance from the barrel axis
	float    barrel;    // barrel length actually used
	qboolean attach;    // barrel axis came from the muzzle attachment
	float    blend;     // engage blend, 1 = fully two-handed
	float    va_pitch, va_yaw;   // hand-to-hand direction as VectorAngles
	float    out_pitch, out_yaw;  // what we hand back to the caller
	qboolean braced;             // returned true (corrections skipped)
} vr_th;


static void VR_DiagSample( void )
{
	float dt;
	int i;
	qboolean frozen;

	if( !vrdiag.fp )
		return;

	// fps accounting runs every frame regardless of sample rate
	dt = (float)( host.realtime - vrdiag.last_frame_time );
	vrdiag.last_frame_time = host.realtime;
	if( dt > 0.0f && dt < 1.0f )
	{
		vrdiag.fps_accum += 1.0f / dt;
		vrdiag.fps_frames++;
	}

	if( vr_diag.value < 2.0f && host.realtime < vrdiag.next_sample )
		return;
	vrdiag.next_sample = host.realtime + Q_max( 0.05f, vr_diag_interval.value );

	frozen = ( VectorLength2( vrdiag.last_pos ) > 0.0f ) &&
		VectorCompareEpsilon( vrdiag.last_pos, vr.hmd_pose.origin, 0.001f );
	if( frozen ) vrdiag.frozen_count++;
	VectorCopy( vr.hmd_pose.origin, vrdiag.last_pos );

	for( i = 0; i < 3; i++ )
	{
		if( vr.hmd_pose.angles[i] < vrdiag.ang_min[i] ) vrdiag.ang_min[i] = vr.hmd_pose.angles[i];
		if( vr.hmd_pose.angles[i] > vrdiag.ang_max[i] ) vrdiag.ang_max[i] = vr.hmd_pose.angles[i];
		if( vr.hmd_pose.origin[i] < vrdiag.pos_min[i] ) vrdiag.pos_min[i] = vr.hmd_pose.origin[i];
		if( vr.hmd_pose.origin[i] > vrdiag.pos_max[i] ) vrdiag.pos_max[i] = vr.hmd_pose.origin[i];
	}

	// headset refresh, derived from the runtime's predicted display period (ns)
	{
		float hmd_hz = 0.0f;

		if( vr.frame_state.predictedDisplayPeriod > 0 )
			hmd_hz = 1000000000.0f / (float)vr.frame_state.predictedDisplayPeriod;

		{
			float lean = 0.0f;
			if( vr.hmd_origin_at_sync_valid )
			{
				vec3_t d;
				VectorSubtract( vr.hmd_pose.origin, vr.hmd_origin_at_sync, d );
				lean = VectorLength( d );
			}

			VR_DiagPrintf( "t=%7.2f  hmd pos=(%7.1f %7.1f %7.1f)  ang=(p%7.2f y%7.2f r%7.2f)"
				"  world=(%7.1f %7.1f %7.1f) yaw=%6.1f  sync=(%7.1f %7.1f %7.1f)%s lean=%6.1f"
				"  fps=%5.1f  hz=%5.1f"
				"  stick=(%5.2f %5.2f) turn=%5.2f%s  hands=L%s/R%s  weapon=%s  sub=%d%s"
				"  2H[grip=%d off=%d dist=%.1f gap=%.1f barrel=%.1f attach=%d blend=%.2f va=(p%.1f y%.1f) out=(p%.1f y%.1f) braced=%d]"
				"  ALIGN[corr_p=%7.2f corr_y=%7.2f health=%d]\n",
				host.realtime - vrdiag.session_start,
				vr.hmd_pose.origin[0], vr.hmd_pose.origin[1], vr.hmd_pose.origin[2],
				vr.hmd_pose.angles[PITCH], vr.hmd_pose.angles[YAW], vr.hmd_pose.angles[ROLL],
				vr.world_origin[0], vr.world_origin[1], vr.world_origin[2], vr.world_yaw,
				vr.hmd_origin_at_sync[0], vr.hmd_origin_at_sync[1], vr.hmd_origin_at_sync[2],
				vr.hmd_origin_at_sync_valid ? "" : "[INVALID]", lean,
				vrdiag.fps_frames ? vrdiag.fps_accum / vrdiag.fps_frames : 0.0f,
				hmd_hz,
				vr.move_x, vr.move_y, vr.turn_x, vr.turn_active ? "" : "[UNBOUND]",
				vr.hand_pose[0].valid ? "ok" : "--",
				vr.hand_pose[1].valid ? "ok" : "--",
				cl.local.viewmodel ? "equipped" : "NONE",
				vr.frames_submitted,
				frozen ? "  [FROZEN]" : "",
				vr_th.grip ? 1 : 0, vr_th.offhand ? 1 : 0,
				vr_th.dist, vr_th.gap, vr_th.barrel,
				vr_th.attach ? 1 : 0, vr_th.blend,
				vr_th.va_pitch, vr_th.va_yaw,
				vr_th.out_pitch, vr_th.out_yaw,
				vr_th.braced ? 1 : 0 ,
				corr_p, corr_y, cl.local.health );
		}
	}

	vrdiag.fps_accum = 0.0f;
	vrdiag.fps_frames = 0;
	vrdiag.samples++;

	// Hand mesh diagnostics - what VR_DrawHands is ACTUALLY feeding the
	// renderer, not just whether the raw pose is tracked. Logs BOTH pose
	// types explicitly labeled, since VR_DrawHands has switched which one it
	// consumes more than once - a label mismatch here previously meant this
	// log did not reflect what was actually on screen. Always know which is
	// LIVE (currently rendered) vs REF (the other one, for comparison).
	{
		int h;
		const qboolean using_aim_for_hands = true; // keep in sync with VR_DrawHands

		for( h = 0; h < 2; h++ )
		{
			vec3_t aim_org, aim_ang, grip_org, grip_ang;
			qboolean got_aim  = VR_GetHandWorld( h, aim_org, aim_ang );
			qboolean got_grip = VR_GetHandGripWorld( h, grip_org, grip_ang );

			VR_DiagPrintf( "  hand[%s] LIVE=%s  aim=%s org=(%7.1f %7.1f %7.1f) ang=(p%7.2f y%7.2f r%7.2f)"
				"  |  grip=%s org=(%7.1f %7.1f %7.1f) ang=(p%7.2f y%7.2f r%7.2f)"
				"  ent.scale=%5.2f (mirror=%s)\n",
				h == 0 ? "L" : "R",
				using_aim_for_hands ? "aim" : "grip",
				got_aim ? "ok" : "INVALID",
				got_aim ? aim_org[0] : 0.0f, got_aim ? aim_org[1] : 0.0f, got_aim ? aim_org[2] : 0.0f,
				got_aim ? aim_ang[PITCH] : 0.0f, got_aim ? aim_ang[YAW] : 0.0f, got_aim ? aim_ang[ROLL] : 0.0f,
				got_grip ? "ok" : "INVALID",
				got_grip ? grip_org[0] : 0.0f, got_grip ? grip_org[1] : 0.0f, got_grip ? grip_org[2] : 0.0f,
				got_grip ? grip_ang[PITCH] : 0.0f, got_grip ? grip_ang[YAW] : 0.0f, got_grip ? grip_ang[ROLL] : 0.0f,
				vr_hand_ent[h].curstate.scale,
				vr_hand_ent[h].curstate.scale < 0.0f ? "YES" : "no" );
		}
	}
}

/*
=================================================================
	helpers
=================================================================
*/
static qboolean VR_InitInput( void );	// defined below, called from VR_InitSession

static const char *VR_ResultString( XrResult r )
{
	static char buf[XR_MAX_RESULT_STRING_SIZE];

	if( vr.instance != XR_NULL_HANDLE && XR_SUCCEEDED( xrResultToString( vr.instance, r, buf )))
		return buf;
	Q_snprintf( buf, sizeof( buf ), "XrResult(%d)", (int)r );
	return buf;
}

#define XR_CHECK( call, what ) \
	do { \
		XrResult _r = ( call ); \
		if( XR_FAILED( _r )) { \
			Con_Printf( S_ERROR "VR: %s failed: %s\n", what, VR_ResultString( _r )); \
			return false; \
		} \
	} while( 0 )

/*
================
VR_ConvertPosition

OpenXR : meters, +X right, +Y up, -Z forward
GoldSrc: units,  +X forward, +Y left, +Z up   (1 unit ~= 1 inch)
================
*/
static void VR_ConvertPosition( const XrVector3f *xr, vec3_t out )
{
	out[0] = -xr->z * VR_UNITS_PER_METER;   // HL forward
	out[1] = -xr->x * VR_UNITS_PER_METER;   // HL left
	out[2] =  xr->y * VR_UNITS_PER_METER;   // HL up
}

/*
================
VR_ConvertOrientation

Build the basis from the quaternion in OpenXR space, convert the basis vectors
into HL space, then extract HL euler angles (pitch, yaw, roll) in degrees.

NOTE: the roll sign convention has not yet been verified against a live headset.
If the world appears to bank the wrong way, negate `roll` here.
================
*/
/*
================
VR_AnglesFromBasis

Extract HL euler angles from an orthonormal forward/right/up basis.

Ported from HLVR's GetAnglesFromVectors (src/cl_dll/util.cpp:149), which is
live-proven on real hardware with this exact hand mesh.

Why not the obvious -asin(forward[2]): asin is mathematically clamped to
+-90 degrees, so any pose past vertical folds back on itself instead of
continuing. That single limitation caused BOTH live-reported failures:
  - pointing the controller at the ceiling rendered the hand REVERSED
    (wrist up, fingers down), and
  - the earlier motion-sickness incident, because a 90-degree mesh
    calibration parked the extraction permanently ON the fold-over
    boundary, where tiny real rotations produce huge angle jumps.
Recovering cos(pitch) from whichever yaw/roll component is numerically
stable gives atan2 a real 2-argument input, restoring the full +-180 range.

Shared by both callers deliberately - VR_ApplyMeshCalibration previously had
its own private copy of the broken asin form, which is why calibration was
unsafe while raw tracking looked fine.
================
*/
static void VR_AnglesFromBasis( const vec3_t fwd, const vec3_t right, const vec3_t up, vec3_t angles )
{
	float sp = -fwd[2];
	float yaw_r  = atan2f( fwd[1], fwd[0] );
	float roll_r = atan2f( -right[2], up[2] );
	float cy = cosf( yaw_r ),  sy = sinf( yaw_r );
	float cr = cosf( roll_r ), sr = sinf( roll_r );
	float cp;

	if( fabs( cy ) > 0.001f )       cp = fwd[0] / cy;
	else if( fabs( sy ) > 0.001f )  cp = fwd[1] / sy;
	else if( fabs( sr ) > 0.001f )  cp = -right[2] / sr;
	else if( fabs( cr ) > 0.001f )  cp = up[2] / cr;
	else                            cp = cosf( asinf( bound( -1.0f, sp, 1.0f )));

	angles[PITCH] = RAD2DEG( atan2f( sp, cp ));
	angles[YAW]   = RAD2DEG( yaw_r );

	// ROLL IS NOT NEGATED. Xash's AngleVectors defines
	//   right[2] = -sin(roll)*cos(pitch),  up[2] = cos(roll)*cos(pitch)
	// so roll_r = atan2( -right[2], up[2] ) already IS roll. A stale comment
	// here claimed a negation was needed - true only for atan2( right[2], .. ),
	// which is not what is computed above, so the negation was applied on top
	// of an already-correct value and inverted it. Both references return it
	// un-negated (HLVR util.cpp:192, Lambda1VR TBXR_Common.c). Negated roll
	// breaks round-tripping through AngleVectors/Matrix3x4_CreateFromEntity
	// and inverts exactly the axis used to turn a palm over.
	angles[ROLL]  = RAD2DEG( roll_r );
}

static void VR_ConvertOrientation( const XrQuaternionf *q, vec3_t angles )
{
	float x = q->x, y = q->y, z = q->z, w = q->w;
	vec3_t fwd_xr, up_xr, fwd, up, right;

	// forward = q * (0,0,-1)
	fwd_xr[0] = -( 2.0f * ( x * z + w * y ));
	fwd_xr[1] = -( 2.0f * ( y * z - w * x ));
	fwd_xr[2] = -( 1.0f - 2.0f * ( x * x + y * y ));

	// up = q * (0,1,0)
	up_xr[0] = 2.0f * ( x * y - w * z );
	up_xr[1] = 1.0f - 2.0f * ( x * x + z * z );
	up_xr[2] = 2.0f * ( y * z + w * x );

	// XR -> HL axes (direction vectors, no scaling)
	fwd[0] = -fwd_xr[2]; fwd[1] = -fwd_xr[0]; fwd[2] = fwd_xr[1];
	up[0]  = -up_xr[2];  up[1]  = -up_xr[0];  up[2]  = up_xr[1];

	VectorNormalize( fwd );
	VectorNormalize( up );

	// right = fwd x up  (HL is left-handed in the sense AngleVectors uses)
	CrossProduct( fwd, up, right );
	VectorNormalize( right );

	// Derived against AngleVectors() in public/xash3d_mathlib.h:431, which defines
	//   forward[2] = -sin(pitch)
	//   right[2]   = -sin(roll) * cos(pitch)
	//   up[2]      =  cos(roll) * cos(pitch)
	VR_AnglesFromBasis( fwd, right, up, angles );

	// runtime sign overrides, see the cvar declarations
	if( vr_pitch_sign.value < 0.0f ) angles[PITCH] = -angles[PITCH];
	if( vr_yaw_sign.value   < 0.0f ) angles[YAW]   = -angles[YAW];
	if( vr_roll_sign.value  < 0.0f ) angles[ROLL]  = -angles[ROLL];
}

static void VR_ConvertPose( const XrPosef *pose, vr_pose_t *out )
{
	VR_ConvertPosition( &pose->position, out->origin );
	VR_ConvertOrientation( &pose->orientation, out->angles );
	out->valid = true;
}

/*
=================================================================
	initialization
=================================================================
*/
void VR_GetRequiredGLVersion( int *major, int *minor )
{
	// Defaults chosen to satisfy runtimes that demand >= 4.0 (VirtualDesktopXR
	// reports min 4.0 / max 5.0). A COMPATIBILITY profile is essential: ref_gl
	// still uses legacy fixed-function GL that a core profile would reject.
	if( major ) *major = vr.gl_major ? vr.gl_major : 4;
	if( minor ) *minor = vr.gl_minor ? vr.gl_minor : 3;
}

qboolean VR_IsAvailable( void ) { return vr.available; }
qboolean VR_IsActive( void )    { return vr.available && vr.session_ready && vr.running; }
int      VR_GetEyeCount( void ) { return vr.eye_count; }

/*
================
VR_SessionStateName
================
*/
static const char *VR_SessionStateName( XrSessionState s )
{
	switch( s )
	{
	case XR_SESSION_STATE_UNKNOWN:      return "UNKNOWN";
	case XR_SESSION_STATE_IDLE:         return "IDLE";
	case XR_SESSION_STATE_READY:        return "READY";
	case XR_SESSION_STATE_SYNCHRONIZED: return "SYNCHRONIZED";
	case XR_SESSION_STATE_VISIBLE:      return "VISIBLE";
	case XR_SESSION_STATE_FOCUSED:      return "FOCUSED";
	case XR_SESSION_STATE_STOPPING:     return "STOPPING";
	case XR_SESSION_STATE_LOSS_PENDING: return "LOSS_PENDING";
	case XR_SESSION_STATE_EXITING:      return "EXITING";
	default:                            return "?";
	}
}

/*
================
VR_Status_f - console diagnostic, "vr_status"
================
*/
static void VR_Status_f( void )
{
	Con_Printf( "--- OpenXR status ---\n" );
	Con_Printf( "  vr_enable      : %s\n", vr_enable.value ? "1" : "0" );
	Con_Printf( "  instance       : %s\n", vr.available ? "created" : "NONE" );
	Con_Printf( "  system (HMD)   : %s\n", vr.have_system ? "present" : "NOT FOUND" );
	Con_Printf( "  session        : %s\n", vr.session_ready ? "created" : "not created" );
	Con_Printf( "  session state  : %s\n", VR_SessionStateName( vr.session_state ));
	Con_Printf( "  running        : %s\n", vr.running ? "YES" : "no" );
	Con_Printf( "  frame started  : %s\n", vr.frame_started ? "yes" : "no" );
	Con_Printf( "  frames submitted: %d\n", vr.frames_submitted );
	Con_Printf( "  eyes           : %d\n", vr.eye_count );

	if( vr.eye_count > 0 )
	{
		Con_Printf( "  eye 0 target   : %ux%u\n",
			vr.swapchains[0].width, vr.swapchains[0].height );
	}

	Con_Printf( "  GL required    : %d.%d\n", vr.gl_major, vr.gl_minor );

	if( vr.have_system )
	{
		Con_Printf( "  hmd pose       : %.1f %.1f %.1f  angles %.1f %.1f %.1f\n",
			vr.hmd_pose.origin[0], vr.hmd_pose.origin[1], vr.hmd_pose.origin[2],
			vr.hmd_pose.angles[0], vr.hmd_pose.angles[1], vr.hmd_pose.angles[2] );
	}

	if( !vr.available )
		Con_Printf( "  -> no instance. Runtime missing, or vr_enable 0, or -novr.\n" );
	else if( !vr.have_system )
		Con_Printf( "  -> instance OK but no HMD. Start the streamer/headset; it retries automatically.\n" );
	else if( !vr.session_ready )
		Con_Printf( "  -> HMD found but session failed. Check GL context version.\n" );
	else if( !vr.running )
		Con_Printf( "  -> session created, waiting for runtime to signal READY.\n" );
	else
		Con_Printf( "  -> VR is live.\n" );
}

/*
================
VR_AcquireSystem

xrGetSystem fails while no HMD is connected. That is a NORMAL transient state -
the streamer may not be running yet, or the headset may be asleep - so we keep the
instance alive and retry rather than disabling VR for the whole session.
================
*/
static qboolean VR_AcquireSystem( void )
{
	XrSystemGetInfo sgi = { XR_TYPE_SYSTEM_GET_INFO };
	uint32_t count = 0;
	int i;
	XrResult res;

	if( vr.have_system )
		return true;

	if( !vr.available )
		return false;

	// don't hammer the runtime every frame
	if( host.realtime < vr.next_system_retry )
		return false;
	vr.next_system_retry = host.realtime + 2.0;

	sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	res = xrGetSystem( vr.instance, &sgi, &vr.system );
	if( XR_FAILED( res ))
	{
		if( vr_debug.value )
			Con_Printf( "VR: xrGetSystem: %s\n", VR_ResultString( res ));
		return false;
	}

	// view configuration
	if( XR_FAILED( xrEnumerateViewConfigurationViews( vr.instance, vr.system,
		XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &count, NULL )) || !count )
	{
		Con_Printf( S_ERROR "VR: no stereo view configuration\n" );
		return false;
	}

	vr.eye_count = Q_min((int)count, VR_MAX_EYES );
	for( i = 0; i < vr.eye_count; i++ )
		vr.view_configs[i].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;

	if( XR_FAILED( xrEnumerateViewConfigurationViews( vr.instance, vr.system,
		XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, vr.eye_count, &count, vr.view_configs )))
		return false;

	for( i = 0; i < vr.eye_count; i++ )
	{
		Con_Printf( "VR: eye %d %ux%u\n", i,
			vr.view_configs[i].recommendedImageRectWidth,
			vr.view_configs[i].recommendedImageRectHeight );
	}

	// GL version requirement
	if( XR_SUCCEEDED( xrGetInstanceProcAddr( vr.instance, "xrGetOpenGLGraphicsRequirementsKHR",
		(PFN_xrVoidFunction *)&vr.pfnGetOpenGLGraphicsRequirementsKHR ))
		&& vr.pfnGetOpenGLGraphicsRequirementsKHR )
	{
		XrGraphicsRequirementsOpenGLKHR gr = { XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR };

		if( XR_SUCCEEDED( vr.pfnGetOpenGLGraphicsRequirementsKHR( vr.instance, vr.system, &gr )))
		{
			vr.gl_major = XR_VERSION_MAJOR( gr.minApiVersionSupported );
			vr.gl_minor = XR_VERSION_MINOR( gr.minApiVersionSupported );
			Con_Printf( "VR: runtime requires OpenGL >= %d.%d\n", vr.gl_major, vr.gl_minor );
		}
	}

	vr.have_system = true;
	Con_Printf( "VR: HMD acquired (%d eyes)\n", vr.eye_count );

	{
		XrSystemProperties sp = { XR_TYPE_SYSTEM_PROPERTIES };

		VR_DiagPrintf( "--- system ---\n" );
		if( XR_SUCCEEDED( xrGetSystemProperties( vr.instance, vr.system, &sp )))
		{
			VR_DiagPrintf( "hmd            : %s (vendor %u)\n", sp.systemName, sp.vendorId );
			VR_DiagPrintf( "tracking       : orientation=%s position=%s\n",
				sp.trackingProperties.orientationTracking ? "yes" : "no",
				sp.trackingProperties.positionTracking ? "yes" : "no" );
		}
		for( i = 0; i < vr.eye_count; i++ )
		{
			VR_DiagPrintf( "eye %d target   : %ux%u (samples %u)\n", i,
				vr.view_configs[i].recommendedImageRectWidth,
				vr.view_configs[i].recommendedImageRectHeight,
				vr.view_configs[i].recommendedSwapchainSampleCount );
		}
		VR_DiagPrintf( "GL required    : %d.%d\n", vr.gl_major, vr.gl_minor );
		VR_DiagPrintf( "units/meter    : %.2f\n\n", VR_UNITS_PER_METER );
	}

	return true;
}

const vr_pose_t *VR_GetHMDPose( void )        { return &vr.hmd_pose; }
const vr_pose_t *VR_GetHandPose( int hand )   { return &vr.hand_pose[bound( 0, hand, 1 )]; }

void VR_SetWorldReference( const vec3_t origin )
{
	qboolean body_moved;

	if( !vr.hmd_origin_at_sync_valid )
	{
		body_moved = true;
	}
	else
	{
		vec3_t delta;
		VectorSubtract( origin, vr.world_origin, delta );
		// LIVE-MEASURED, not guessed: vr_diag.log during actual standing-
		// still testing showed the mod's own vieworigin jittering by ~1.3
		// units frame-to-frame on its own (view bob / prediction noise),
		// even with no player movement at all. An earlier 0.5-unit
		// threshold was smaller than that noise floor, so it resynced on
		// nearly every frame regardless - silently defeating the entire
		// point of this gate (confirmed live: "no change"). 8 units is
		// comfortably above the measured idle jitter and comfortably below
		// a real step.
		// FULL resync only for a discontinuity - teleport, level change, lift.
		// A step is not one of these.
		body_moved = ( VectorLength( delta ) > 64.0f );

		// CLOSE THE LOOP for room-scale - and ONLY for room-scale.
		//
		// Room-scale is open-loop without this: physically stepping aside makes
		// the body walk that way and KEEP walking, because the offset driving it
		// is never reduced by the body arriving. So the sync point has to
		// advance as the body catches up.
		//
		// But it must advance by room-scale's OWN contribution, not by however
		// far the body moved. Subtracting the whole of delta is exactly
		// self-cancelling: VR_PlayToWorld computes
		//     out = world_origin + R(+yaw)*(P - sync)
		// so advancing sync by R(-yaw)*delta while world_origin gains delta
		// gives
		//     out' = (W+D) + R*(P - S - R'D) = W + R*(P-S) = out
		// - the eye and every hand are then mathematically INVARIANT to body
		// motion. The play space stops following the player: stick locomotion,
		// trains, conveyors, basevelocity and NPC pushes all slide the world out
		// from under a play space that stays pinned where it was. Measured live
		// before this fix, vr_diag.log's "lean" climbed monotonically from 174
		// to 740 units over twenty seconds of ordinary play.
		//
		// So attribute the motion. Project the body's actual displacement onto
		// the direction room-scale asked for and advance by that component
		// only. Everything else the body does is left alone, and the play space
		// rides along with it as it should.
		//
		// This also degrades correctly in the cases that matter:
		//   - walking into a wall: the body does not move, the projection is
		//     ~0, the offset persists, and you stand with your face in geometry
		//     until you physically step back (see VR_GetRoomScaleMove);
		//   - room-scale disabled: nothing is ever published, the sync point
		//     holds still, and leaning to peek round a corner works as before;
		//   - stick and room-scale at once: only the room-scale-aligned share is
		//     taken, and any cross-talk is bounded by that projection and
		//     self-corrects on the following frame.
		if( !body_moved && vr.hmd_origin_at_sync_valid && vr.roomscale_cmd_valid )
		{
			vec3_t dir;
			float want = VectorLength( vr.roomscale_cmd );

			if( want > 0.0f )
			{
				float moved;

				VectorScale( vr.roomscale_cmd, 1.0f / want, dir );

				// Horizontal only - height is not room-scale's business, and
				// lifts/steps are handled by smooth_z below.
				moved = delta[0] * dir[0] + delta[1] * dir[1];

				// CLAMP, on two separate grounds. Without this the sync point
				// overshoots and manufactures a REVERSE offset that no physical
				// motion produced, which then feeds back as a spurious
				// room-scale command in the opposite direction and stalls the
				// drawn eye for a frame while the body keeps moving. In a
				// headset that reads as judder.
				//
				// 1. Never retire more offset than actually exists. delta is
				//    whatever the body did for ALL reasons - stick, trains,
				//    knockback - so on any ordinary run it dwarfs the head's
				//    real lead. Crediting all of it drives the offset through
				//    zero and out the far side, and the `moved > 0` gate below
				//    then blocks the correcting frame, so the flip persists for
				//    the rest of the movement burst rather than self-correcting.
				//
				// 2. Never retire more than room-scale itself asked for. delta
				//    is a DISPLACEMENT, while vr.roomscale_cmd is a VELOCITY -
				//    it lands in cmd->forwardmove. Room-scale's real
				//    contribution this frame is therefore want * frametime, and
				//    anything past that is another system's motion being
				//    silently attributed to room-scale.
				{
					vec3_t rel_now;
					float sy, cy, avail, share;

					VectorSubtract( vr.hmd_pose.origin, vr.hmd_origin_at_sync, rel_now );

					// Play -> world, so the available lead is measured along
					// the same axis dir lives in.
					SinCos( DEG2RAD( vr.body_yaw ), &sy, &cy );
					avail = ( rel_now[0] * cy - rel_now[1] * sy ) * dir[0]
					      + ( rel_now[0] * sy + rel_now[1] * cy ) * dir[1];

					if( moved > avail ) moved = avail;

					share = want * (float)host.frametime;
					if( moved > share ) moved = share;
				}

				if( moved > 0.0f )
				{
					float s, c;
					vec3_t world_share, play_delta;

					VectorScale( dir, moved, world_share );

					// World -> play space is the inverse of the play -> world
					// rotation in VR_PlayToWorld, so rotate by MINUS body yaw.
					SinCos( DEG2RAD( -vr.body_yaw ), &s, &c );

					play_delta[0] = world_share[0] * c - world_share[1] * s;
					play_delta[1] = world_share[0] * s + world_share[1] * c;
					play_delta[2] = 0.0f;

					VectorAdd( vr.hmd_origin_at_sync, play_delta, vr.hmd_origin_at_sync );
				}
			}
		}

		// Consume it either way. Command generation and rendering are not
		// locked to the same cadence, so a stale request must never be spent
		// twice - erring toward "the play space rides along" is the safe side.
		VectorClear( vr.roomscale_cmd );
		vr.roomscale_cmd_valid = false;
	}

	VectorCopy( origin, vr.world_origin );
	vr.world_yaw = vr.body_yaw;

	// Ease the eye up a step instead of snapping it.
	//
	// Only smooth changes small enough to BE a step. Anything larger is a
	// teleport, a level change, a lift, or a fall, and easing those would
	// leave the view visibly detached from the body for a long moment - much
	// worse than the jolt this exists to remove. MAX_STEP is GoldSrc's own
	// step height.
	{
		const float MAX_STEP = 18.0f;
		float dz;

		if( !vr.smooth_z_valid )
		{
			vr.smooth_z = origin[2];
			vr.smooth_z_valid = true;
		}

		dz = origin[2] - vr.smooth_z;

		if( fabs( dz ) > MAX_STEP || vr_step_smooth.value <= 0.0f )
		{
			vr.smooth_z = origin[2];		// snap: not a step
		}
		else
		{
			float t = vr_step_smooth.value * host.frametime;

			if( t > 1.0f ) t = 1.0f;
			vr.smooth_z += dz * t;
		}
	}

	if( body_moved )
	{
		VectorCopy( vr.hmd_pose.origin, vr.hmd_origin_at_sync );
		vr.hmd_origin_at_sync_valid = true;

		// A teleport or level change is not head movement. Drop the previous
		// sample so the discontinuity is not read as an enormous step.
		vr.neck_prev_valid = false;
	}
}

float VR_GetBodyYaw( void )
{
	return vr.body_yaw;
}

/*
================
VR_SelectOpen

True while the weapon select HUD is up.

Exists so melee can stand down. Fire is the select-s confirm and swing-to-hit
raises the same IN_ATTACK bit, so a hand moved quickly while choosing would
take whatever was highlighted - and reaching across to the select is exactly
the motion that trips the swing threshold.
================
*/
qboolean VR_SelectOpen( void )
{
	return vr.select_open;
}

/*
================
VR_PlayToWorld

Map a play-space position/orientation into game world space, using the same
anchor and rotation the eyes use so hands and view agree.
================
*/
static void VR_PlayToWorld( const vr_pose_t *pose, vec3_t out_org, vec3_t out_ang )
{
	vec3_t rel;
	float s, c;
	const float *hmd_ref = vr.hmd_origin_at_sync_valid ? vr.hmd_origin_at_sync : vr.hmd_pose.origin;

	VectorSubtract( pose->origin, hmd_ref, rel );
	SinCos( DEG2RAD( vr.body_yaw ), &s, &c );

	if( out_org )
	{
		out_org[0] = vr.world_origin[0] + ( rel[0] * c - rel[1] * s );
		out_org[1] = vr.world_origin[1] + ( rel[0] * s + rel[1] * c );
		out_org[2] = vr.world_origin[2] + rel[2];
	}

	if( out_ang )
	{
		// NOTE: no mesh calibration offset applied here. It was originally
		// here and shared by both consumers of this function - but the
		// weapon viewmodel and the bare-hand model are two DIFFERENT meshes
		// with, evidently, two different rest-pose conventions: +90 pitch
		// made the bare hand point correctly forward but made the equipped
		// weapon point up/inverted. Each mesh gets its own calibration at its
		// own call site instead of one shared here. See VR_DrawHands for the
		// hand offset; the weapon-pin code in cl_view.c currently applies
		// none (unconfirmed correct either way - not yet reported on
		// independently of the hand mesh).
		out_ang[PITCH] = pose->angles[PITCH];
		out_ang[YAW]   = anglemod( pose->angles[YAW] + vr.body_yaw );
		out_ang[ROLL]  = pose->angles[ROLL];
	}
}

qboolean VR_GetHandWorld( int hand, vec3_t out_org, vec3_t out_ang )
{
	hand = bound( 0, hand, 1 );

	if( !VR_IsActive() || !vr.hand_pose[hand].valid )
		return false;

	VR_PlayToWorld( &vr.hand_pose[hand], out_org, out_ang );
	return true;
}

/*
================
VR_GetListener

Where the player's EARS are, in world space.

The stereo loop hands S_UpdateFrame the mod's own view - the flat camera
pfnCalcRefdef produced - because that is what the flatscreen path does. In VR
that camera is not attached to the player's head: it does not turn when they
turn, it does not move when they walk the room, and it does not drop when they
physically crouch. The result is a sound field spatialised from a viewpoint
nobody is looking through, which is why turning your head in the headset did
not move the world around you audibly.

Same anchor and rotation the eyes and hands use, minus the per-eye IPD offset -
so the listener sits at the centre of the head and the whole rig agrees.
================
*/
qboolean VR_GetListener( vec3_t out_org, vec3_t out_ang )
{
	if( !VR_IsActive() || !vr.hmd_pose.valid )
		return false;

	VR_PlayToWorld( &vr.hmd_pose, out_org, out_ang );
	return true;
}

// Same, but GRIP pose - for rendering a held mesh (the hand model itself, or
// a weapon's visual placement) rather than a pointing/aim direction. See the
// design note on vr_profile_t for why these are two separate poses.
qboolean VR_GetHandGripWorld( int hand, vec3_t out_org, vec3_t out_ang )
{
	hand = bound( 0, hand, 1 );

	if( !VR_IsActive() || !vr.hand_grip_pose[hand].valid )
		return false;

	VR_PlayToWorld( &vr.hand_grip_pose[hand], out_org, out_ang );
	return true;
}

/*
=================================================================
	bare-hand visuals

The weapon viewmodel (clgame.viewent) already gets pinned to the right
controller by cl_view.c when a weapon is equipped - that draws a gun, not a
hand. This fills the two gaps: the LEFT hand, which has no weapon slot at
all, and the RIGHT hand when nothing is equipped (weapon=NONE), where
otherwise nothing is drawn.

These are client-only synthetic entities, added to the per-frame visible list
via CL_AddVisibleEntity - the same mechanism every normal entity in the game
goes through - rather than anything server/mod-specific, so this works
identically regardless of which GoldSrc mod is loaded.

Assets: models/v_hand_hevsuit.mdl and v_hand_labcoat.mdl are not part of
stock Half-Life. They are placed in the writable game directory (an overlay
on top of the read-only -rodir base) for local testing.
=================================================================
*/

static void VR_InitHandModels( void )
{
	int i;
	static int last_servercount = -1;

	// RELOAD ON EVERY LEVEL CHANGE.
	//
	// These models are synthetic - nothing precaches them, because no mod knows
	// they exist. Mod_LoadWorld purges the studio cache and Mod_FreeUnused then
	// frees anything unreferenced, which always includes these. The model_t
	// slot is zeroed and handed to the NEXT model that asks for one, so a
	// cached pointer here does not merely dangle, it silently aliases some
	// other mesh. Symptom is hands vanishing or turning into another model
	// partway through a campaign.
	//
	// Reloading per level is cheap (Mod_ForName returns the cached model when
	// it is still resident) and is the only way to stay correct without
	// hooking the mod's precache list, which we cannot do.
	if( cl.servercount != last_servercount )
	{
		last_servercount = cl.servercount;
		vr_hands_init_tried = false;
		vr_hand_model_suit = vr_hand_model_labcoat = NULL;

		for( i = 0; i < 2; i++ )
			vr_hand_ent[i].model = NULL;
		vr_offhand_ent.model = NULL;
	}

	if( vr_hands_init_tried )
		return;
	vr_hands_init_tried = true;

	vr_hand_model_suit    = Mod_ForName( "models/v_hand_hevsuit.mdl", false, false );
	vr_hand_model_labcoat = Mod_ForName( "models/v_hand_labcoat.mdl", false, false );

	if( !vr_hand_model_suit && !vr_hand_model_labcoat )
	{
		Con_Printf( S_WARN "VR: no bare-hand models found (models/v_hand_hevsuit.mdl / "
			"v_hand_labcoat.mdl) - hands will be invisible when unarmed\n" );
	}
	else
	{
		Con_Printf( "VR: bare-hand models loaded (suit=%s labcoat=%s)\n",
			vr_hand_model_suit ? "yes" : "no", vr_hand_model_labcoat ? "yes" : "no" );
	}

	for( i = 0; i < 2; i++ )
	{
		memset( &vr_hand_ent[i], 0, sizeof( vr_hand_ent[i] ));
		// CRASHED HERE with an arbitrary large index (0x7FFE): something in the
		// studio render path indexes a per-entity array directly with ->index,
		// with no bounds check, and clgame.maxEntities is a modest engine-sized
		// value (GI->max_edicts + headroom, see cl_parse.c) - nowhere near
		// 0x7FFE. That corrupted memory and the crash dump's stack walk landed
		// on the wreckage, not the real cause. Use the top of the array the
		// engine actually allocated, which is guaranteed in-bounds and, being
		// far above any real networked entity, effectively never collides.
		vr_hand_ent[i].index = clgame.maxEntities - 1 - i;
		vr_hand_ent[i].curstate.rendermode = kRenderNormal;
		vr_hand_ent[i].curstate.renderamt  = 255;

		// hand 0 = left. The mesh is an inherently right-handed model; mirror
		// the left copy across its local X axis (see the mirror code added to
		// R_StudioSetUpTransform in gl_studio.c, and the note there on why a
		// negative *uniform* scale would NOT be equivalent to this).
		vr_hand_ent[i].curstate.scale = ( i == 0 ) ? -1.0f : 0.0f;
	}
}

/*
================
VR_DrawHands

Called once per frame (not per eye - the visible-entity list persists across
both eye passes within a frame, same as every other entity). Draws the left
hand always, and the right hand only when VR_ShouldDrawWeapon determines no
weapon viewmodel is being shown this frame.
================
*/
/*
================
VR_ApplyMeshCalibration

Corrects a tracked pose for a mesh whose rest-pose axes don't line up with
the raw controller convention. Previously this was done by simply ADDING a
constant to the tracked pitch (vr_hand_pitch_offset) - live-tested and
confirmed broken: it only looked right near the pose it was tuned at (rest,
palms facing the body) and a real roll input (turning the palm to face away)
came out as a pitch-like change (palm tilting to the sky) the further the
hand rotated away from that reference pose. That symptom is the textbook
signature of Euler-angle addition, which is NOT the same operation as
rotating the mesh's local reference frame and does not commute with the
tracked rotation except very close to identity.

Checked against HLVR's actual working implementation (VRHelper.cpp) for how
a real, live-tested VR mod handles this: it applies NO per-axis Euler offset
at all anywhere in its controller-to-HL-angle pipeline - orientation stays
in matrix/basis-vector form and is only collapsed to angles at the very end
(GetHLAnglesFromVRMatrix). Where a mesh needs a fixed correction, the
correct operation is a genuine LOCAL-SPACE pre-rotation, composed as
matrices, not summed as angles.

cal_angles is applied as a rotation in the mesh's own local space (composed
via Matrix3x4_ConcatTransforms as tracked * calibration, i.e. calibration
happens first, in local space, then the real tracked rotation orients the
already-corrected frame) - this is what makes the correction pose-invariant
instead of only valid near the pose it was empirically tuned at.
================
*/
static void VR_ApplyMeshCalibration( vec3_t ang, const vec3_t cal_angles )
{
	matrix3x4 tracked, calib, result;
	vec3_t local_fwd = { 1.0f, 0.0f, 0.0f };   // HL convention: X = forward
	vec3_t local_right = { 0.0f, -1.0f, 0.0f }; // Y = left, so right = -Y
	vec3_t local_up = { 0.0f, 0.0f, 1.0f };    // Z = up
	vec3_t fwd, right, up;

	Matrix3x4_CreateFromEntity( tracked, ang, vec3_origin, 1.0f );
	Matrix3x4_CreateFromEntity( calib, (float *)cal_angles, vec3_origin, 1.0f );
	Matrix3x4_ConcatTransforms( result, tracked, calib );

	Matrix3x4_VectorRotate( result, local_fwd, fwd );
	Matrix3x4_VectorRotate( result, local_right, right );
	Matrix3x4_VectorRotate( result, local_up, up );

	VectorNormalize( fwd );
	VectorNormalize( right );
	VectorNormalize( up );

	// Shared full-range extraction - see VR_AnglesFromBasis. This function
	// previously carried its own private copy using the asin form, which is
	// clamped to +-90 degrees. With a 90-degree pitch calibration that put
	// the extraction permanently ON the fold-over boundary, where tiny real
	// rotations produce huge angle jumps - the direct cause of the
	// motion-sickness incident that got calibration disabled entirely.
	// Matrix composition here was never the problem; the extraction was.
	VR_AnglesFromBasis( fwd, right, up, ang );
}

/*
================
VR_CalibrateWeaponAngles

Applies the weapon viewmodel's rest-pose correction, for cl_view.c's
controller-pinning code. Exposed rather than duplicated so the weapon uses
the exact same local-pre-rotation path the hands do (never euler addition,
which does not compose - see VR_ApplyMeshCalibration).

Call this on the PHYSICAL tracked angles, BEFORE pre-negating pitch.
================
*/
/*
================
VR_ApplyTwoHandedAim

If the off hand is up at the weapon, take aim from the hand-to-hand vector
instead of the dominant controller's own orientation. See the cvar block for
why (Lambda1VR VrInputAlt2.c:183-196).

dom_org is the weapon hand's world position; ang is the PHYSICAL tracked
orientation, modified in place. Call BEFORE the mesh calibration and before
pre-negating pitch, so it stays in the same convention as everything else.

Returns true if two-handed aim was applied.
================
*/
qboolean VR_ApplyTwoHandedAim( const vec3_t dom_org, vec3_t ang )
{
	// Not while climbing. The off-hand grip means "hold this rung" on a
	// ladder, and the weapon is stowed anyway - so steadying a gun that is
	// not in the player's hands with a hand that is holding the ladder is
	// two kinds of wrong at once.
	if( VR_LadderHands( ))
		return false;

	static float blend = 0.0f;	// 0 = one-handed, 1 = fully two-handed
	static qboolean latched = false;	// see the hysteresis note below
	vec3_t off_org, off_ang, delta, dir, fwd, aim, va;
	float dist, target = 0.0f;

	// The weapon's current forward, i.e. where the barrel already points.
	AngleVectors( ang, fwd, NULL, NULL );

	// Seed `dir` to the weapon's own forward.
	//
	// It is only assigned inside the engage block below, but `blend` decays
	// over vr_twohand_smooth AFTER that block stops being entered - on grip
	// release, or any frame the off hand drops tracking. Those frames still
	// reach the VectorLerp at the end with blend near 1, so an unassigned
	// `dir` puts stack garbage straight into the aim, and
	// VR_AlignModelToFireRay then records it as the bore reference for the
	// next frame - so one bad frame poisons the following ones too.
	//
	// This is the same defect that was already found and fixed once here; the
	// fix covered the ENGAGE path only. Seeding makes the release path a no-op
	// blend toward the weapon's existing direction, which is what "letting go"
	// should mean anyway.
	VectorCopy( fwd, dir );

	vr_th.grip    = VR_GetButton( VR_BTN_OFFGRIP ) ? true : false;
	vr_th.offhand = false;
	vr_th.dist = vr_th.gap = -1.0f;
	vr_th.attach = false;
	vr_grip_valid = false;

	// Requires an actual grip press on the off hand, like Lambda1VR
	// (VrInputAlt2.c:109-124). Proximity alone was tried and is wrong: the
	// weapon would grab onto the off hand any time it drifted near, with no
	// way to just rest a hand there. Holding the grip is the deliberate
	// "I am supporting the weapon" signal; releasing drops back to one hand.
	// NOT WHILE THAT HAND IS FULL.
	//
	// Bracing and loading are the same input in the same place: off-hand
	// grip, held, beside the weapon. Carrying a round to the gun therefore
	// looked exactly like taking hold of it to steady it, and the aim would
	// snap to a supporting grip that was really a hand full of ammunition -
	// worst on the two-handed weapons, which are the ones worth bracing.
	//
	// Working the action is deliberately NOT excluded: a hand on the pump is
	// a hand on the weapon, and bracing through it is correct.
	if( VR_IsActive() && vr_twohand.value && VR_GetButton( VR_BTN_OFFGRIP ) &&
	    !vr.rl_holding &&
	    VR_GetHandWorld( VR_OffHand(), off_org, off_ang ))
	{
		VectorSubtract( off_org, dom_org, delta );
		dist = VectorLength( delta );
		vr_th.offhand = true;
		vr_th.dist = dist;

		// The hand-to-hand direction itself. This assignment went missing
		// when the old cone test was replaced by the barrel-segment test,
		// leaving `dir` as uninitialised stack garbage that was then blended
		// into the aim - the real cause of the weapon flying off. The log
		// made it plain: measured hand positions gave a hand-to-hand pitch
		// of about -21 degrees while the code was reporting +87.5.
		VectorCopy( delta, dir );
		VectorNormalize( dir );

		// HYSTERESIS - essential, not a refinement.
		//
		// Engaging rotates the weapon to follow the hand-to-hand line, which
		// moves the muzzle attachment, which moves the barrel axis this very
		// test measures against. So the instant it engaged it failed its own
		// entry test, disengaged, snapped back and re-engaged - once per
		// frame. Reported live as the gun having a seizure when grabbed.
		//
		// The geometry test is therefore an ENTRY condition only. Once
		// latched, the hold depends only on intent (grip button) and a loose
		// separation range - neither of which the weapon's own orientation
		// can influence. That breaks the feedback loop.
		if( latched
		    ? ( dist >= vr_twohand_min.value * 0.5f && dist <= vr_twohand_max.value * 1.5f )
		    : ( dist >= vr_twohand_min.value && dist <= vr_twohand_max.value ))
		{
			// Closest approach between the off hand and the barrel. Engaging
			// on that distance means the hand has to physically meet the
			// weapon, not merely be roughly in line with it.
			//
			// The barrel axis is taken from the weapon model's MUZZLE
			// ATTACHMENT when there is one: grip (dom_org) -> muzzle. That is
			// the gun the player can actually see, at its true length, so no
			// direction or length has to be assumed.
			//
			// Deriving it from AngleVectors( ang ) instead was a real bug:
			// `ang` is the raw controller pose, while the rendered weapon
			// points along the attachment bore, which is a different
			// direction entirely. The test measured against an imaginary
			// barrel roughly 45 degrees off the visible one, so putting a
			// hand on the actual foregrip never engaged - reported live as
			// two-handing simply not working on the rifle.
			float barrel_len = vr_twohand_barrel.value;
			float along;
			vec3_t axis, closest, gap;

			VectorCopy( fwd, axis );

			// attachment[0] alone is enough for the barrel AXIS; with none at
			// all the slot holds the entity origin and the "barrel" would run
			// from the grip to itself.
			if( vr_aim_attachment.value && VR_ModelAttachments( clgame.viewent.model ) >= 1 )
			{
				vec3_t to_muzzle;

				VectorSubtract( clgame.viewent.attachment[0], dom_org, to_muzzle );

				if( VectorLength( to_muzzle ) > 1.0f )
				{
					barrel_len = VectorLength( to_muzzle );
					VectorCopy( to_muzzle, axis );
					VectorNormalize( axis );
					vr_th.attach = true;
				}
			}

			along = DotProduct( delta, axis );
			along = bound( 0.0f, along, barrel_len );
			VectorMA( dom_org, along, axis, closest );
			VectorSubtract( off_org, closest, gap );
			vr_th.gap = VectorLength( gap );
			vr_th.barrel = barrel_len;

			// Remember where on the barrel the hand actually met it, so the
			// drawn hand can be put THERE rather than wherever the controller
			// physically is. Your real hand closes on empty air while the
			// virtual barrel is a solid object, so the two disagree by
			// however far you over-reached - and the hand is drawn inside the
			// weapon. Snapping to the barrel line hides that.
			// Only once the hand is ACTUALLY ON THE WEAPON.
			//
			// This used to be set here unconditionally, before the distance test
			// below - so it fired on hand SEPARATION alone, anywhere from 6 to 24
			// units. A hand held out to the side, well clear of the gun and never
			// engaging two-handed aim, still had its drawn model teleported onto
			// the barrel. That is the snap-from-across-the-body: not the grab
			// volume being too large, but the hand POSTING having no volume test at
			// all.
			if( VectorLength( gap ) <= vr_twohand_radius.value * ( latched ? 1.6f : 1.0f ))
			{
				VectorCopy( closest, vr_grip_org );
				VectorCopy( axis, vr_grip_axis );
				vr_grip_valid = true;
			}

			// HYSTERESIS ON THE DISTANCE, not a bypass of it.
			//
			// This was  latched || gap <= radius  - so the instant it engaged the
			// geometry test stopped being applied at all, and the only thing still
			// holding the grip was hand separation, which permits 91cm once the
			// 1.5x latch margin is counted. The off hand could wander clean across
			// the body and stay gripped, with the DRAWN hand slammed onto the
			// barrel throughout - reported as the grab volume being a sphere
			// rather than something that follows the shape of the weapon.
			//
			// Widening the radius while latched stops the release chattering at the
			// boundary, without ever abandoning the capsule the test is built on.
			if( VectorLength( gap ) <= vr_twohand_radius.value * ( latched ? 1.6f : 1.0f ))
				target = 1.0f;
		}
	}

	// Ease toward the target instead of switching hard.
	//
	// Integrated at most ONCE per frame. This function is called from both
	// CL_CreateCmd (to build the aim) and V_RenderView (to place the model),
	// which each run once a frame - so integrating per CALL made the ramp
	// advance twice per frame and engage in half the configured time.
	if( vr_twohand_smooth.value > 0.0f && host.frametime > 0.0f )
	{
		static double last_integrated = -1.0;

		if( host.realtime != last_integrated )
		{
			float step = (float)host.frametime / vr_twohand_smooth.value;

			last_integrated = host.realtime;

			if( target > blend ) blend = Q_min( blend + step, target );
			else                 blend = Q_max( blend - step, target );
		}
	}
	else blend = target;

	// Latch tracks the target: releasing the grip or pulling the hands
	// apart drops it, and re-entry requires the full geometry test again.
	latched = ( target > 0.0f );

	vr_th.blend = blend;

	if( blend <= 0.001f )
		return false;

	// SET the angles absolutely from the hand-to-hand line - do not steer
	// toward it, and do not let any further correction be applied on top.
	//
	// Ported from Lambda1VR (VrInputAlt2.c:183-204), which is the reference
	// that actually works. Its stabilised branch does:
	//
	//   VectorSet( weaponangles[ADJUSTED],
	//              RAD2DEG( atanf( y / zxDist ) ),
	//              forwardYaw - RAD2DEG( atan2f( x, -z ) ),
	//              weaponangles[ADJUSTED][ROLL] );
	//
	// i.e. pitch and yaw are overwritten outright and only ROLL survives.
	// The decisive detail is what that branch does NOT do: the one-handed
	// else-branch right below it applies `YAW += (viewyaw - hmdyaw)` and
	// `PITCH *= -1`, and the stabilised branch skips both. It also discards
	// vr_weapon_pitchadjust, the rest-pose correction baked in further up,
	// simply by overwriting the value.
	//
	// That is the whole lesson: once the hand-to-hand vector is in hand it is
	// already a finished WORLD direction, so every correction meant for a
	// controller-derived angle has to be dropped, not layered on. Steering by
	// a delta while the mesh correction still applied downstream is what sent
	// the gun wild; the caller now skips VR_CalibrateWeaponAngles whenever
	// this returns true.
	VectorLerp( fwd, blend, dir, aim );
	if( VectorNormalizeLength( aim ) == 0.0f )
		return false;

	VectorAngles( aim, va );

	// VectorAngles is pitch-positive-UP; entity angles follow AngleVectors,
	// where positive pitch is DOWN.
	ang[PITCH] = -va[PITCH];
	ang[YAW]   = va[YAW];

	vr_th.va_pitch  = va[PITCH];
	vr_th.va_yaw    = va[YAW];
	vr_th.out_pitch = ang[PITCH];
	vr_th.out_yaw   = ang[YAW];
	vr_th.braced    = ( blend >= 0.999f );
	// ROLL deliberately preserved, exactly as Lambda1VR does - wrist twist on
	// the firing hand still rolls the weapon while braced.

	return ( blend >= 0.999f );

}

/*
=================================================================
	Universal weapon profiling

VR behaviour needs to know things about the weapon in the player's hand: is
it melee, is it thrown, where is the muzzle. Hardcoding lists of Half-Life
weapon names answers that for exactly one mod and silently mis-handles every
other, which defeats the point of a mod-agnostic VR layer.

Instead the answers are read out of the MODEL, which every mod must ship and
which already carries the metadata:

  numattachments  - ranged weapons carry a muzzle attachment for their flash;
                    melee weapons generally carry none
  sequence labels - authored by whoever built the weapon: "fire", "shoot",
                    "reload", "throw", "pinpull", "holster", "pump"

None of that is Half-Life specific, so a mod's custom weapons classify
themselves with no per-mod table and no rebuild.

Cached per model pointer, so the parse happens once per weapon rather than
every frame.
=================================================================
*/
typedef struct
{
	const model_t *model;
	qboolean       valid;
	qboolean       melee;       // positively identified as a swung weapon
	qboolean       classified;  // the metadata actually said something either
	                            // way, so the name hint in VR_HoldingMelee is
	                            // not needed (and must not override it)
	qboolean       throwable;   // has throw/pin style sequences
	qboolean       has_muzzle;  // at least one attachment
	qboolean       pump;        // has a pump/bolt/lever action to work
	qboolean       slide;       // its action locks back on empty and must be released
} vr_wprofile_t;

static vr_wprofile_t vr_wprof;

static qboolean VR_SeqLabelContains( const studiohdr_t *hdr, const char *needle )
{
	const mstudioseqdesc_t *seq;
	int i;

	if( !hdr || hdr->numseq <= 0 )
		return false;

	seq = (const mstudioseqdesc_t *)((const byte *)hdr + hdr->seqindex);

	for( i = 0; i < hdr->numseq; i++ )
	{
		if( Q_stristr( seq[i].label, needle ))
			return true;
	}
	return false;
}

static const vr_wprofile_t *VR_GetWeaponProfile( void )
{
	const model_t *mod = clgame.viewent.model;
	studiohdr_t *hdr;
	static int last_servercount = -1;

	// Pointer identity is NOT enough to key this cache across a level change.
	// model_t slots are recycled: Mod_FreeUnused zeroes an unreferenced entry
	// and Mod_FindName hands the same address to a different model, so the
	// pointer matches while the mesh behind it is something else entirely.
	//
	// That misfires badly here rather than cosmetically. A pistol inheriting a
	// crowbar's melee=true means VR_GetMeleeAttack starts ORing IN_ATTACK on
	// hand velocity, so the gun fires whenever the hand moves quickly - and it
	// would also take the melee rest-pose angle. Drop the cache per level.
	if( cl.servercount != last_servercount )
	{
		last_servercount = cl.servercount;
		memset( &vr_wprof, 0, sizeof( vr_wprof ));
	}

	if( vr_wprof.model == mod && vr_wprof.valid )
		return &vr_wprof;	// cached

	memset( &vr_wprof, 0, sizeof( vr_wprof ));
	vr_wprof.model = mod;

	if( !mod || mod->type != mod_studio )
		return &vr_wprof;

	hdr = (studiohdr_t *)Mod_StudioExtradata( (model_t *)mod );
	if( !hdr )
		return &vr_wprof;

	vr_wprof.valid      = true;
	vr_wprof.has_muzzle = ( hdr->numattachments > 0 );

	// A PUMP, BOLT OR LEVER has to be animated to exist, so the sequence
	// list says whether this weapon has one. Half-Life's shotgun carries a
	// "pump" sequence; a mod's own pump-action carries one for the same
	// reason, and an autoloader carries none. No weapon needs naming.
	vr_wprof.pump =
		VR_SeqLabelContains( hdr, "pump" ) ||
		VR_SeqLabelContains( hdr, "bolt" ) ||
		VR_SeqLabelContains( hdr, "lever" );

	// A SLIDE THAT LOCKS BACK announces itself by needing two reloads.
	//
	// A weapon whose action stays open on an empty magazine has to animate
	// that reload differently from a topped-up one, so the model carries a
	// second reload sequence naming the distinction - Half-Life's pistol has
	// "reload" for the empty case and "reload_noshot" for the other. A
	// weapon with one reload animation has nothing to release, and asking
	// the player to work an action the gun never shows is miming.
	//
	// Deliberately keyed on a RELOAD variant rather than the word "empty"
	// alone, which also appears on firing-on-empty animations that say
	// nothing about the action.
	vr_wprof.slide =
		VR_SeqLabelContains( hdr, "reload_noshot" ) ||
		VR_SeqLabelContains( hdr, "reload_not" ) ||
		VR_SeqLabelContains( hdr, "reload_empty" );

	// Thrown weapons are the clearest signal: you cannot animate throwing
	// something without a sequence that says so.
	vr_wprof.throwable =
		VR_SeqLabelContains( hdr, "throw" ) ||
		VR_SeqLabelContains( hdr, "pinpull" ) ||
		VR_SeqLabelContains( hdr, "lob" );

	// MELEE IS IDENTIFIED POSITIVELY - never inferred from absence.
	//
	// This was previously melee = !fires, with fires keyed partly on
	// has_muzzle (numattachments > 0). Both halves proved wrong on real
	// content, in opposite and equally bad directions:
	//
	//   - numattachments carries no melee information on Valve-derived models.
	//     A VR-authored crowbar carrying four attachments read as "fires", so
	//     melee came out FALSE and swing-to-hit was silently off on the one
	//     weapon it exists for.
	//   - Retail Half-Life's v_satchel and v_tripmine have no attachments and
	//     no fire/shoot labels (idle1/fidget1/draw/drop and arm1/place), so
	//     "melee by absence" made them melee. VR_GetMeleeAttack then ORs
	//     IN_ATTACK on hand speed, and waving your arm plants tripmines and
	//     drops satchels.
	//
	// So require evidence, and default to NOT melee. The two failure modes are
	// not symmetric: a melee weapon misread as a gun costs the player a trigger
	// pull, while a gun misread as melee discharges it every time they move
	// their hand quickly. Only the first of those is acceptable.
	//
	// "hit" and "miss" carry most of the weight - GoldSrc melee weapons are
	// conventionally animated attack1hit / attack1miss, a naming no gun uses.
	// Still metadata rather than a name list, so a mod's pipe wrench, katana or
	// fire axe classifies correctly without appearing anywhere.
	if( !vr_wprof.throwable )
	{
		qboolean fires =
			VR_SeqLabelContains( hdr, "fire" ) ||
			VR_SeqLabelContains( hdr, "shoot" ) ||
			VR_SeqLabelContains( hdr, "reload" );

		qboolean swings =
			VR_SeqLabelContains( hdr, "hit" ) ||
			VR_SeqLabelContains( hdr, "miss" ) ||
			VR_SeqLabelContains( hdr, "swing" ) ||
			VR_SeqLabelContains( hdr, "slash" ) ||
			VR_SeqLabelContains( hdr, "stab" ) ||
			VR_SeqLabelContains( hdr, "chop" ) ||
			VR_SeqLabelContains( hdr, "bash" );

		vr_wprof.melee      = ( swings && !fires );
		vr_wprof.classified = ( swings || fires );
	}
	else
	{
		vr_wprof.classified = true;	// a throw sequence is evidence in itself
	}

	if( vr_debug.value )
	{
		Con_Printf( "VR: weapon profile %s -> muzzle=%d melee=%d throwable=%d seqs=%d\n",
			mod->name, vr_wprof.has_muzzle, vr_wprof.melee,
			vr_wprof.throwable, hdr->numseq );
	}

	return &vr_wprof;
}

qboolean VR_HoldingMelee( void )
{
	const char *name;

	if( !clgame.viewent.model || !clgame.viewent.model->name[0] )
		return false;

	// Model-derived first: works for any mod's custom melee weapon without
	// it appearing in any list. Name matching is kept only as a hint for
	// models that carry no usable metadata.
	//
	// Gated on `classified`, not just `valid`. Returning wp->melee whenever the
	// model parsed at all made the hint below unreachable dead code, so a
	// weapon whose sequences said nothing either way silently defaulted instead
	// of getting the one piece of evidence left.
	{
		const vr_wprofile_t *wp = VR_GetWeaponProfile();

		if( wp->valid && wp->classified )
			return wp->melee;
	}

	name = clgame.viewent.model->name;

	return Q_stristr( name, "crowbar" ) != NULL
	    || Q_stristr( name, "knife" )   != NULL
	    || Q_stristr( name, "wrench" )  != NULL
	    || Q_stristr( name, "pipe" )    != NULL;
}

static int VR_ModelAttachments( const model_t *mod )
{
	studiohdr_t *hdr;

	// attachment[] is a FIXED four-slot array on every cl_entity_t, and the
	// renderer only writes the slots a model actually declares. Slots beyond
	// numattachments keep whatever was there before - and gl_studio seeds an
	// unattached entity slot with the entity ORIGIN, so on a one-attachment
	// weapon [1]-[0] is not a short bore, it is a vector pointing back at the
	// player. Nothing about that is small enough for a length check to reject.
	if( !mod || mod->type != mod_studio )
		return 0;

	hdr = (studiohdr_t *)Mod_StudioExtradata( (model_t *)mod );

	return hdr ? hdr->numattachments : 0;
}

static qboolean VR_ModelHasBore( const model_t *mod )
{
	return ( VR_ModelAttachments( mod ) >= 2 ) ? true : false;
}

/*
================
VR_AlignModelToFireRay

Rotate the weapon MODEL so its visible barrel lies along the actual firing
line, by measuring the difference rather than assuming a constant.

The rest-pose correction gets the model roughly right, but a residual always
remains - the barrel sits inside each mesh at its own angle, so no single
number lines every weapon up. Reported live as the gun drawn slightly below
its own laser.

Both quantities here are measurable, so nothing has to be guessed: the fire
ray is known exactly, and the model's real bore comes from its muzzle
attachment, which ref_gl recomputes in world space every frame. Steering by
the difference converges regardless of what the rest-pose correction did, and
needs no per-weapon tuning.

No feedback risk while braced: aim comes from the hand-to-hand vector, which
this cannot influence. When not braced the fire ray is itself derived from
the attachment, so the difference is zero and this is a no-op.
================
*/
/*
================
VR_ResetModelAlign

Drop the accumulated correction. Called when entering a braced two-handed
hold, so releasing the grip does not hand the one-handed path a value that was
integrated under completely different conditions.
================
*/
void VR_ResetModelAlign( void )
{
	corr_p = corr_y = 0.0f;
	vr_bore_valid = false;
}

/*
================
VR_AlignModelToFireRay

Draw the weapon so its BORE lies along the angles it was handed.

Every viewmodel's barrel sits at some arbitrary angle inside the mesh, so
handing the entity a direction does not make the visible barrel point that way.
That offset is a FIXED PROPERTY OF THE MODEL, and it can simply be measured:

    drawing with angles A produced bore B   ->   offset = B - A
    to make the bore point at D             ->   draw with D - offset

ref_gl recomputes attachment[] in world space every frame, and we know what
angles we passed last frame, so both halves are already in hand.

This replaces a feedback integrator that steered toward the error with a small
per-frame gain. That was the wrong tool: an integrator has to be tuned, it lags
by the frame that attachment[] is behind, and it can ring, saturate or wind up.
All three happened - it shook on grip, it pinned at its clamp for whole runs,
and with the clamp originally used it wound to 180 degrees and drew the weapon
upside down. There is no error signal to chase here, only a constant to look
up, and a measurement cannot oscillate.

Light smoothing only, to reject a frame where attachment[] has not caught up
after a weapon change.
================
*/
qboolean VR_AlignModelToFireRay( vec3_t ang )
{
	vec3_t bore;
	int i;

	if( !vr_model_align.value || !VR_IsActive() )
		return false;

	if( !VR_ModelHasBore( clgame.viewent.model ))
	{
		vr_bore_valid = false;
		return false;
	}

	// Drop the measurement when the WEAPON or the LEVEL changes.
	//
	// The measured bore is a property of one specific mesh, so carrying it
	// across a weapon change applies the pistol's offset to the shotgun. Across
	// a level change it is worse: model_t slots are recycled, so the cached
	// pointer can match while the mesh behind it is a different model entirely.
	//
	// VR_ResetModelAlign existed for exactly this and ended up with no callers
	// at all when the surrounding code was restructured, so the state was never
	// being cleared by anything.
	{
		static const model_t *last_align_model = NULL;
		static int last_align_servercount = -1;

		if( clgame.viewent.model != last_align_model || cl.servercount != last_align_servercount )
		{
			last_align_model = clgame.viewent.model;
			last_align_servercount = cl.servercount;
			VR_ResetModelAlign();
			vr_bore_have_last = false;
		}
	}

	// Where the barrel ACTUALLY pointed, given what we drew last frame.
	VectorSubtract( clgame.viewent.attachment[1], clgame.viewent.attachment[0], bore );
	if( VectorLength( bore ) < 0.1f )
		return false;			// no usable attachment on this mesh

	VectorNormalize( bore );

	// Express the bore in the MODEL'S OWN frame, not the world's.
	//
	// This is the part that has to be right. Stored as world pitch/yaw deltas,
	// the offset is only correct while roll is zero: Euler components do not
	// compose, so once the weapon carries roll - which it always does while
	// braced, since two-handed aiming preserves wrist roll - a fixed mesh
	// offset stops mapping to fixed pitch/yaw. It swings with roll, and where
	// the decomposition folds it jumps about a quarter turn. Reported live as
	// the model pitching down 90 degrees the instant the grabbing hand moved.
	//
	// In the model's own frame the bore direction is a genuine constant, so
	// measure it there and apply it back as a LOCAL PRE-ROTATION, which does
	// compose. Same reason VR_ApplyMeshCalibration is a matrix concat rather
	// than an angle addition.
	if( vr_bore_have_last )
	{
		vec3_t fwd, right, up, local;

		AngleVectors( vr_bore_last_ang, fwd, right, up );

		// AngleVectors' `right` is the image of model -Y, so model +Y is -right.
		local[0] = DotProduct( bore, fwd );
		local[1] = -DotProduct( bore, right );
		local[2] = DotProduct( bore, up );

		VectorNormalize( local );

		if( !vr_bore_valid )
		{
			VectorCopy( local, vr_bore_local );
			vr_bore_valid = true;
		}
		else
		{
			// A constant in principle; smoothing only rejects the odd frame
			// where attachment[] has not caught up after a weapon change.
			for( i = 0; i < 3; i++ )
				vr_bore_local[i] += ( local[i] - vr_bore_local[i] ) * 0.25f;
			VectorNormalize( vr_bore_local );
		}
	}

	if( vr_bore_valid )
	{
		vec3_t cal;

		// The rotation that puts the mesh's bore onto model-forward is the
		// INVERSE of the bore's own local orientation - and an inverse is NOT
		// obtained by negating Euler components.
		//
		// Let VectorAngles(bore) = (P_up, Y, 0). The entity-convention
		// rotation whose forward IS the bore is
		//     B = Rz(Y) * Ry(pe),   pe = -P_up
		// so the correction we want is
		//     C = B^-1 = Ry(-pe) * Rz(-Y)
		//
		// VR_ApplyMeshCalibration feeds its angles through
		// Matrix3x4_CreateFromEntity, which composes in the FIXED order
		// Rz(yaw)*Ry(pitch)*Rx(roll). Handing it (P_up, -Y, 0) therefore builds
		// Rz(-Y)*Ry(P_up) - the two factors in the WRONG ORDER - and throws
		// away the roll term the true inverse carries. Those agree only when
		// the bore is off-axis in pitch alone or yaw alone; with both, the
		// barrel stays misaligned by an amount no amount of measurement can
		// remove, because the error is in the composition, not the input.
		//
		// Decompose the real inverse into that fixed ZYX order instead.
		{
			float pe, sp, cp, sy, cy;

			VectorAngles( vr_bore_local, cal );

			pe = DEG2RAD( -cal[PITCH] );	// VectorAngles is pitch-up
			sp = sin( pe ); cp = cos( pe );
			sy = sin( DEG2RAD( cal[YAW] )); cy = cos( DEG2RAD( cal[YAW] ));

			cal[PITCH] = RAD2DEG( -asin( bound( -1.0f, sp * cy, 1.0f )));
			cal[YAW]   = RAD2DEG( atan2( -sy, cp * cy ));
			cal[ROLL]  = RAD2DEG( atan2( sp * sy, cp ));
		}

		VR_ApplyMeshCalibration( ang, cal );
	}

	// Remember what we are about to draw, so next frame can measure against it.
	VectorCopy( ang, vr_bore_last_ang );
	vr_bore_have_last = true;

	return vr_bore_valid;
}

void VR_CalibrateWeaponAngles( vec3_t ang )
{
	vec3_t cal;
	float pitch;

	// Melee weapons are held like a tool rather than aimed like a gun, so
	// they rest at a different angle in the hand and need their own
	// correction. Lambda1VR carries a separate vr_crowbar_pitchadjust for
	// the same reason.
	pitch = VR_HoldingMelee() ? vr_melee_pitch_offset.value
	                          : vr_weapon_pitch_offset.value;

	if( pitch == 0.0f &&
	    vr_weapon_yaw_offset.value == 0.0f &&
	    vr_weapon_roll_offset.value == 0.0f )
		return;

	cal[PITCH] = pitch;
	cal[YAW]   = vr_weapon_yaw_offset.value;
	cal[ROLL]  = vr_weapon_roll_offset.value;

	VR_ApplyMeshCalibration( ang, cal );
}

/*
================
VR_FindHandSequence

Sequence index on the hand model whose label contains `needle`, or -1.

By LABEL, not by index. The hand model here happens to put fullgrab at 7, but
hardcoding that breaks the moment anyone swaps in a different hand model - and
this fork exists to work with content it has never seen. Every studio model
carries its sequence labels, so asking by name costs nothing and never goes
stale. Same principle as the weapon profiler.
================
*/
static int VR_FindHandSequence( const model_t *mod, const char *needle )
{
	studiohdr_t *hdr;
	const mstudioseqdesc_t *seq;
	int i;

	if( !mod || mod->type != mod_studio )
		return -1;

	hdr = (studiohdr_t *)Mod_StudioExtradata( (model_t *)mod );
	if( !hdr || hdr->numseq <= 0 )
		return -1;

	seq = (const mstudioseqdesc_t *)((const byte *)hdr + hdr->seqindex);

	for( i = 0; i < hdr->numseq; i++ )
	{
		if( Q_stristr( seq[i].label, needle ))
			return i;
	}

	return -1;
}

void VR_DrawHands( qboolean draw_right )
{
	int hand;

	if( !VR_IsActive() || !vr_hands.value )
		return;

	VR_InitHandModels();

	for( hand = 0; hand < 2; hand++ )
	{
		vec3_t org, ang;
		cl_entity_t *e = &vr_hand_ent[hand];
		model_t *mdl;

		// Refresh the synthetic entity index EVERY FRAME rather than freezing it
		// at init. clgame.maxEntities is re-derived per connection and is reset
		// to 2 in between (cl_game.c), so an index captured once can end up
		// pointing at a real networked entity - or past the end of the array,
		// which the studio path then memcpy's attachment data into with no
		// bounds check at all (gl_studio.c:3175). Recomputing is a subtraction;
		// the crash it avoids is not worth saving it.
		if( clgame.maxEntities > 8 )
			vr_hand_ent[hand].index = clgame.maxEntities - 1 - hand;
		else
			continue;	// entity array not sized yet - nothing safe to use

		if( hand == 1 && !draw_right )
			continue;

		// While dual wielding the off hand holds a gun (VR_DrawOffhandWeapon),
		// so the bare hand must not also be drawn there - reported live as
		// "2 hands on offhand", the mesh and the weapon stacked on each other.
		if( hand == 0 && VR_DualWieldActive( ))
			continue;

		// AIM pose, not grip - grip pose made the hand face the wrong way
		// entirely when tried. Aim pose plus a per-mesh calibration offset
		// (below) is what actually got the bare hand pointing forward,
		// confirmed live. This offset is NOT shared with the weapon
		// viewmodel - applying it there too made an equipped weapon point
		// up/inverted, so the two meshes evidently have different rest-pose
		// conventions and are calibrated independently.
		if( !VR_GetHandWorld( hand, org, ang ))
			continue;

		// POST the supporting hand onto the barrel while two-handing.
		//
		// Your real hand closes on empty air, so it keeps travelling past
		// where the virtual barrel is - and gets drawn buried inside the
		// weapon. Drawing it at the point on the barrel it actually met
		// instead reads as gripping the gun, and costs nothing in accuracy:
		// aiming uses the REAL controller position, which is untouched. Only
		// the visual moves.
		//
		// Eased rather than snapped, so releasing hands it back to the
		// tracked position without a jump.
		if( hand == VR_OffHand() && vr_grip_snap.value > 0.0f )
		{
			static float  posted = 0.0f;
			static vec3_t last_grip;
			float step = ( host.frametime > 0.0f ) ? (float)host.frametime * 8.0f : 1.0f;

			if( vr_grip_valid )
			{
				VectorCopy( vr_grip_org, last_grip );
				posted = Q_min( posted + step, 1.0f );
			}
			else posted = Q_max( posted - step, 0.0f );

			// Eases BOTH ways. Letting go has to hand the hand back to its
			// tracked position gradually too, or releasing the grip snaps it
			// across the gap you had over-reached by.
			if( posted > 0.0f )
			{
				vec3_t gf, gr, gu, gang;

				VectorLerp( org, posted * bound( 0.0f, vr_grip_snap.value, 1.0f ), last_grip, org );

				// Cosmetic nudge off the barrel line. The barrel axis is the
				// centre of the gun, so a hand centred on it reads as being
				// inside the weapon rather than wrapped under it - sitting it
				// slightly low looks like a grip. Purely where the hand is
				// DRAWN: engagement, aim and the fire ray all still use the
				// real controller position and are untouched.
				VectorAngles( vr_grip_axis, gang );
				gang[PITCH] = -gang[PITCH];
				AngleVectors( gang, gf, gr, gu );

				VectorMA( org, vr_grip_offset_fwd.value * posted,  gf, org );
				VectorMA( org, vr_grip_offset_side.value * posted, gr, org );
				VectorMA( org, vr_grip_offset_up.value * posted,   gu, org );
			}

			// Wrap the hand AROUND the barrel instead of leaving it in
			// whatever direction the controller happens to face. Aligning the
			// hand's forward with the barrel axis is what makes it read as a
			// grip rather than a hand that happens to be nearby.
			//
			// Tracked ROLL is kept, so twisting your wrist still rolls the
			// hand on the barrel, and the offsets below exist because no
			// single rotation suits every hand mesh - the same reason the
			// rest-pose corrections exist elsewhere in this file.
			if( posted > 0.5f && vr_grip_pose.value )
			{
				vec3_t axis_ang;

				VectorAngles( vr_grip_axis, axis_ang );
				axis_ang[PITCH] = -axis_ang[PITCH];	// VectorAngles is pitch-up
				axis_ang[ROLL]  = ang[ROLL];

				axis_ang[PITCH] += vr_grip_pose_pitch.value;
				axis_ang[YAW]   += vr_grip_pose_yaw.value;
				axis_ang[ROLL]  += vr_grip_pose_roll.value;

				VectorCopy( axis_ang, ang );
			}
		}

		// `ang` is the PHYSICAL controller orientation. The MESH, however,
		// does not rest along its own +X: bone-chain measurement puts the
		// hand's long axis (wrist joint -> fingertip centroid) 68.7 degrees
		// BELOW model +X, independently corroborated by the flashlight bones
		// (muzzle_pos -> muzzle_pos2 gives a beam 64.4 degrees below +X).
		// So drawn raw, the hand hangs visibly pitched down - reported live
		// as "the hands are pitched down like 45 degrees".
		//
		// Corrected as a LOCAL PRE-ROTATION (tracked * calib), NOT by adding
		// to the euler pitch. Euler addition does not compose: it was tried,
		// and away from the tuned pose it bled roll into pitch and turned a
		// simple palm-flip into a wide sweeping arc.
		//
		// Applied BEFORE the pivot block below on purpose - the pivot point
		// is expressed in MODEL space, so it has to rotate along with this
		// correction or it lands somewhere else entirely.
		if( vr_hand_pitch_offset.value != 0.0f || vr_hand_yaw_offset.value != 0.0f || vr_hand_roll_offset.value != 0.0f )
		{
			vec3_t cal = { vr_hand_pitch_offset.value, vr_hand_yaw_offset.value, vr_hand_roll_offset.value };
			VR_ApplyMeshCalibration( ang, cal );
		}

		// Move the mesh so the point named by vr_hand_pivot_* lands ON the
		// tracked point, instead of the model origin landing there. Solving
		// world = origin + R*local for origin gives origin = tracked - R*P.
		//
		// Must stay HERE, before the pitch pre-negation below: gl_studio.c
		// negates pitch again on the way in, so the basis the renderer
		// actually uses is AngleVectors( physical ang ) - which is what `ang`
		// still is at this point. Computing it after the flip would invert
		// the Z component, the same frame-inversion bug the earlier
		// Lambda1VR port introduced here.
		{
			vec3_t fwd, right, up;
			// Left hand is drawn mirrored: R_StudioSetUpTransform negates
			// matrix column 1 when curstate.scale < 0, mapping model +Y to
			// world RIGHT instead of LEFT. The Y term must flip with it or
			// the left hand's correction goes sideways.
			float ysign = ( e->curstate.scale < 0.0f ) ? -1.0f : 1.0f;

			AngleVectors( ang, fwd, right, up );

			// R*P = P.x*fwd + P.y*(-right) + P.z*up   (AngleVectors' `right`
			// is the image of model -Y, so model +Y is -right). Subtract it.
			VectorMA( org, -vr_hand_pivot_fwd.value,               fwd,   org );
			VectorMA( org,  vr_hand_pivot_left.value * ysign,      right, org );
			VectorMA( org, -vr_hand_pivot_up.value,                up,    org );
		}

		// PRE-NEGATE PITCH. R_StudioSetUpTransform (ref/gl/gl_studio.c:539)
		// negates pitch for every studio model unless the mod sets
		// ENGINE_COMPENSATE_QUAKE_BUG - the original Quake inverse-pitch bug.
		// Stock Half-Life does NOT set that flag, so the engine WILL negate.
		//
		// HLVR negates pitch a second time, at extraction, so its two
		// negations cancel: VRHelper.cpp:492 `angles.x = 360.f - angles.x`
		// feeding StudioModelRenderer.cpp:601 `angles[PITCH] = -angles[PITCH]`.
		// We only ever had the engine's one, leaving hand pitch NET INVERTED -
		// which is what "fingers to the ceiling renders wrist to the ceiling"
		// actually was. Matching HLVR's double negation here fixes it at the
		// source, instead of papering over it with a 90-degree offset that
		// rotated the whole frame and turned a simple palm-flip into a wide
		// sweeping arc.
		ang[PITCH] = -ang[PITCH];

		// TODO: switch to the labcoat model before the player has picked up
		// the suit. No verified way to query that state from here yet -
		// defaults to whichever asset is actually present.
		mdl = vr_hand_model_suit ? vr_hand_model_suit : vr_hand_model_labcoat;
		if( !mdl ) continue;

		if( e->model != mdl )
		{
			// modelindex is a precache-slot correlation used for network
			// delta compression; this entity is synthesized locally and never
			// networked, so it is left at 0 - only the model pointer matters
			// for rendering a client-only entity like this.
			e->model = mdl;
			e->curstate.sequence = 0;
			e->curstate.frame = 0;
			e->curstate.animtime = host.realtime;
			e->curstate.framerate = 1.0f;
		}

		// Close the hand when it is actually holding something. The model
		// ships fullgrab/halfgrab poses and we were drawing idle, which is
		// why a hand posted onto a barrel still read as an open palm laid
		// against it rather than a grip.
		{
			int want_seq = 0;

			if( hand == VR_OffHand() && vr_grip_valid && vr_grip_pose.value )
			{
				int grab = VR_FindHandSequence( mdl, "fullgrab" );

				if( grab < 0 )
					grab = VR_FindHandSequence( mdl, "halfgrab" );
				if( grab < 0 )
					grab = VR_FindHandSequence( mdl, "grab" );	// any grip pose at all

				if( grab >= 0 )
					want_seq = grab;
			}

			if( e->curstate.sequence != want_seq )
			{
				e->curstate.sequence = want_seq;
				e->curstate.animtime = host.realtime;
				e->curstate.frame = 0.0f;
			}
		}

		VectorCopy( org, e->origin );
		VectorCopy( org, e->curstate.origin );
		VectorCopy( org, e->latched.prevorigin );
		VectorCopy( ang, e->angles );
		VectorCopy( ang, e->curstate.angles );
		VectorCopy( ang, e->latched.prevangles );

		CL_AddVisibleEntity( e, ET_NORMAL );
	}
}

/*
================
VR_DrawOffhandWeapon

Draws the second gun while dual wielding.

There is no second weapon ENTITY to render - GoldSrc has one active weapon and
the game DLL owns it. So this draws another copy of the same viewmodel at the
off-hand controller, mirrored, purely client side. Nothing is networked and no
game state is touched: the shot it appears to fire is the DLL's business
(CBasePlayer::DualWieldPostFrame), this is only what you see.

Mirroring is the same trick the left hand already uses - curstate.scale < 0
makes R_StudioSetUpTransform negate matrix column 1 - so the off-hand gun reads
as a left-handed weapon rather than a clone facing the wrong way.
================
*/
/*
================
VR_DrawHeldRound

Put something in the hand that is carrying a round.

Reaching to your hip and closing an empty fist on nothing, then moving that
nothing to the gun, is a gesture with no object in it - the haptics say a
round was taken but the eyes never agree, and the hand looks the same whether
it is carrying a shell or has already fumbled it.

Half-Life ships a single shotgun shell as shotgunshell.mdl, which is the
ejected casing and exactly the right object. A cvar rather than that path
fixed, so a mod without it can point somewhere else or draw nothing, and an
absent model simply skips - a missing prop must never cost the gesture.
================
*/
void VR_DrawHeldRound( void )
{
	static model_t *mdl = NULL;
	static int last_servercount = -1;
	vec3_t org, ang;

	if( !VR_IsActive() || !vr.rl_holding || !vr_reload_model.string[0] )
		return;

	// Reloaded per level for the same reason the hand models are: model_t
	// slots are recycled, so a pointer kept across a level change can end up
	// naming something else entirely.
	if( cl.servercount != last_servercount )
	{
		last_servercount = cl.servercount;
		mdl = Mod_ForName( vr_reload_model.string, false, false );
	}

	if( !mdl )
		return;

	if( !VR_GetHandWorld( VR_OffHand(), org, ang ))
		return;

	// Pre-negate pitch, as every studio model drawn from here must - see the
	// long note in VR_DrawHands for why the engine negates it again later.
	ang[PITCH] = -ang[PITCH];

	if( vr_round_ent.model != mdl )
	{
		vr_round_ent.model = mdl;
		vr_round_ent.curstate.sequence = 0;
		vr_round_ent.curstate.frame = 0;
		vr_round_ent.curstate.animtime = host.realtime;
		vr_round_ent.curstate.framerate = 1.0f;
		vr_round_ent.curstate.rendermode = kRenderNormal;
		vr_round_ent.curstate.renderamt = 255;
		vr_round_ent.curstate.scale = 1.0f;
	}

	// Same synthetic-index safety as the hands and the off-hand weapon: in
	// bounds, recomputed per frame, and clear of the ones they take.
	if( clgame.maxEntities <= 8 )
		return;
	vr_round_ent.index = clgame.maxEntities - 4;

	VectorCopy( org, vr_round_ent.origin );
	VectorCopy( org, vr_round_ent.curstate.origin );
	VectorCopy( org, vr_round_ent.latched.prevorigin );
	VectorCopy( ang, vr_round_ent.angles );
	VectorCopy( ang, vr_round_ent.curstate.angles );
	VectorCopy( ang, vr_round_ent.latched.prevangles );

	CL_AddVisibleEntity( &vr_round_ent, ET_NORMAL );
}

void VR_DrawOffhandWeapon( void )
{

	vec3_t org, ang;
	model_t *mdl;

	if( !VR_DualWieldActive() || !vr_hands.value )
		return;

	mdl = clgame.viewent.model;
	if( !mdl )
		return;

	if( !VR_GetHandWorld( VR_OffHand(), org, ang ))
		return;

	// Same rest-pose correction the drawn main weapon gets, so both guns hang
	// in the hand the same way. Deliberately NOT VR_AlignModelToFireRay: that
	// closes the residual against the MAIN hand's fire ray, which has nothing
	// to do with where this one points.
	VR_CalibrateWeaponAngles( ang );

	// Pre-negate pitch, as every studio model drawn from here must - see the
	// long note in VR_DrawHands for why the engine negates it again downstream.
	ang[PITCH] = -ang[PITCH];

	if( vr_offhand_ent.model != mdl )
	{
		vr_offhand_ent.model = mdl;
		vr_offhand_ent.curstate.sequence = 0;
		vr_offhand_ent.curstate.frame = 0;
		vr_offhand_ent.curstate.animtime = host.realtime;
		vr_offhand_ent.curstate.framerate = 1.0f;
		vr_offhand_ent.curstate.rendermode = kRenderNormal;
		vr_offhand_ent.curstate.renderamt = 255;
	}

	// Same synthetic-index safety as the hands: in bounds, recomputed per
	// frame, and clear of the two the hands take.
	if( clgame.maxEntities <= 8 )
		return;
	vr_offhand_ent.index = clgame.maxEntities - 3;

	vr_offhand_ent.curstate.scale = -1.0f;		// mirror: left-handed copy
	vr_offhand_ent.curstate.body = clgame.viewent.curstate.body;
	vr_offhand_ent.curstate.skin = clgame.viewent.curstate.skin;

	VectorCopy( org, vr_offhand_ent.origin );
	VectorCopy( org, vr_offhand_ent.curstate.origin );
	VectorCopy( org, vr_offhand_ent.latched.prevorigin );
	VectorCopy( ang, vr_offhand_ent.angles );
	VectorCopy( ang, vr_offhand_ent.curstate.angles );
	VectorCopy( ang, vr_offhand_ent.latched.prevangles );

	CL_AddVisibleEntity( &vr_offhand_ent, ET_NORMAL );
}

/*
=================================================================
	Haptics, melee swings, hand-mounted flashlight
=================================================================
*/

/*
================
VR_Haptic

Fire-and-forget buzz on one controller. hand: 0 = left, 1 = right.
duration in seconds, amplitude 0..1.
================
*/
void VR_Haptic( int hand, float duration, float frequency, float amplitude )
{
	XrHapticVibration hv = { XR_TYPE_HAPTIC_VIBRATION };
	XrHapticActionInfo hai = { XR_TYPE_HAPTIC_ACTION_INFO };

	if( !VR_IsActive() || !vr_haptics.value )
		return;

	hand = bound( 0, hand, 1 );

	hv.duration  = (XrDuration)( bound( 0.01f, duration, 2.0f ) * 1000000000.0 ); // ns
	hv.frequency = frequency > 0.0f ? frequency : XR_FREQUENCY_UNSPECIFIED;
	hv.amplitude = bound( 0.0f, amplitude * vr_haptics.value, 1.0f );

	hai.action = vr.act_haptic[hand];

	xrApplyHapticFeedback( vr.session, &hai, (const XrHapticBaseHeader *)&hv );
}

/*
================
VR_GetFlashlightSource

Off-hand flashlight. Returns the world position and forward direction the
flashlight should project from, so it can be aimed independently of the
head. Called from CL_UpdateFlashlight (cl_tent.c); returns false to fall
back to the stock head-mounted behaviour.
================
*/
qboolean VR_GetFlashlightSource( vec3_t out_org, vec3_t out_fwd )
{
	vec3_t ang;
	int hand;

	if( !VR_IsActive() || !vr_flashlight_hand.value )
		return false;

	// 0 = left (off hand) by default; 1 puts it on the weapon hand.
	hand = ( vr_flashlight_hand.value >= 2.0f ) ? 1 : 0;

	if( !VR_GetHandWorld( hand, out_org, ang ))
		return false;

	AngleVectors( ang, out_fwd, NULL, NULL );
	return true;
}

/*
================
VR_UpdateMelee

Swing-to-hit for melee weapons. Tracks how fast the weapon hand is actually
moving through the world and reports a hit when it crosses a threshold, so
the crowbar responds to a real swing instead of a trigger pull.

Velocity is differentiated from the tracked position rather than taken from
OpenXR's velocity field, because the position is already being computed here
every frame and this avoids depending on a runtime reporting velocities.

Latching: once a swing fires, it will not fire again until the hand slows
back below the release threshold. Without that, a single swing spans several
frames above the threshold and would register as a burst of hits.
================
*/
static qboolean VR_UpdateMelee( void )
{
	static vec3_t prev_org;
	static qboolean have_prev = false;
	static qboolean swinging = false;
	static const model_t *last_model = NULL;
	static int last_servercount = -1;
	const vr_pose_t *pose;
	vec3_t delta;
	float speed;

	if( !VR_IsActive() || !vr_melee_swing.value || host.frametime <= 0.0f )
	{
		have_prev = false;
		swinging = false;
		return false;
	}

	// Guard against a swing being detected because the WEAPON or the LEVEL
	// changed, not because the hand moved.
	//
	// This function is only reached while a melee weapon is held, so while a
	// gun is equipped prev_org sits frozen at wherever the hand was during the
	// last melee frame. Re-selecting the crowbar then differentiates against
	// that stale point over a single frametime, which is thousands of units a
	// second - an instant phantom swing, complete with haptic, hitting whatever
	// happens to be in front of you. A level change does the same, since the
	// play space is re-anchored at the new spawn.
	if( clgame.viewent.model != last_model || cl.servercount != last_servercount )
	{
		last_model = clgame.viewent.model;
		last_servercount = cl.servercount;
		have_prev = false;
		swinging = false;
	}

	// Differentiate the hand in PLAY SPACE, not world space.
	//
	// VR_GetHandWorld anchors the pose to the player's world position, so the
	// body's own velocity lands in the result: sprinting is ~320 units/sec and
	// vr_melee_speed defaults to 220, which means simply RUNNING swings the
	// crowbar continuously without the hand moving at all. Tracking-space
	// position is relative to the play area and carries no body motion.
	pose = VR_GetHandPose( VR_DominantHand( ));

	if( !pose || !pose->valid )
	{
		have_prev = false;
		return false;
	}

	if( !have_prev )
	{
		VectorCopy( pose->origin, prev_org );
		have_prev = true;
		return false;
	}

	VectorSubtract( pose->origin, prev_org, delta );
	VectorCopy( pose->origin, prev_org );

	speed = VectorLength( delta ) / (float)host.frametime;	// HL units/sec

	// A zero or negative threshold would make every frame a swing AND never
	// clear the hysteresis below, latching the detector on permanently.
	if( vr_melee_speed.value <= 0.0f )
		return false;

	if( !swinging && speed >= vr_melee_speed.value )
	{
		swinging = true;
		VR_Haptic( VR_DominantHand(), 0.09f, 0.0f, 0.85f );
		return true;
	}

	// Hysteresis - require a clear slowdown before another swing counts.
	if( swinging && speed < vr_melee_speed.value * 0.5f )
		swinging = false;

	return false;
}

/*
================
VR_GetMeleeAttack

True on the frame a melee swing should register as an attack. Only active
while actually holding a melee weapon, so normal guns are unaffected.
Consumed by CL_CreateCmd, which ORs IN_ATTACK into the usercmd - the same
button the mod already handles, so no game-DLL knowledge is needed.
================
*/
qboolean VR_GetMeleeAttack( void )
{
	if( !VR_HoldingMelee( ))
		return false;

	return VR_UpdateMelee();
}

/*
=================================================================
	VR world overlays - laser sight and grenade arc

Drawn through the TriAPI from pfnDrawNormalTriangles, which ref_gl calls
during the 3D pass with the world transform already set. Engine-side, so it
is mod-agnostic.

Camera-facing ribbons rather than GL lines: a 1px line is nearly invisible
at VR resolutions and shimmers badly under reprojection, while a quad strip
turned to face the eye stays solid and reads at distance.
=================================================================
*/

/*
================
VR_GetWeaponAim

Where the weapon is actually pointing, in world space.

Deliberately does NOT apply VR_CalibrateWeaponAngles: that is a correction
for the MESH's rest pose, which exists so the model lines up with the
controller's aim pose. The aim pose is already the true pointing direction,
so folding the mesh correction in here would bend aim away from it by
whatever cosmetic angle the model happened to need.

Includes two-handed stabilisation, so a shouldered weapon shoots where the
steadied barrel points.
================
*/
qboolean VR_AimFromWeapon( void )
{
	return ( VR_IsActive() && vr_aim_from_weapon.value != 0.0f ) ? true : false;
}

qboolean VR_WeaponOriginActive( void )
{
	return ( VR_IsActive() && vr_weapon_origin.value != 0.0f ) ? true : false;
}

/*
=================================================================
	Room-scale, comfort, handedness
=================================================================
*/

/*
================
VR_DominantHand / VR_OffHand

Which controller holds the weapon. Everything that used to say "1" for the
weapon hand asks here instead, so left-handed play is a cvar rather than a
fork. 0 = left, 1 = right.
================
*/
int VR_DominantHand( void )
{
	return ( vr_lefthand.value != 0.0f ) ? 0 : 1;
}

int VR_OffHand( void )
{
	return ( vr_lefthand.value != 0.0f ) ? 1 : 0;
}

/*
================
VR_GetPhysicalCrouch

Duck by ducking. The HMD's height above the tracking floor is compared with
the standing height in vr_height, and dropping below vr_crouch_ratio of it
raises IN_DUCK.

A ratio rather than an absolute number, because players differ by a foot or
more and a fixed "crouched is below 40 units" is either unreachable for a
short player or triggered by good posture in a tall one. vr_height is what
gets calibrated (see vr_calibrate); the ratio then holds for everyone.
================
*/
qboolean VR_GetPhysicalCrouch( void )
{
	// Never while seated. Physical crouch measures the head against a STANDING
	// baseline, and a seated player is permanently below it - so the game ducks
	// them and never stops, and they cannot stand up to cancel it. Reported as
	// spawning half in the floor.
	if( vr_seated.value != 0.0f )
		return false;

	float standing, head;

	if( !VR_IsActive() || vr_crouch.value == 0.0f )
		return false;

	standing = vr_height.value;
	if( standing < 1.0f )
		return false;

	head = vr.hmd_pose.origin[2];

	return ( head < standing * vr_crouch_ratio.value ) ? true : false;
}

/*
================
VR_CalibrateHeight_f

Take your current headset height as standing height. Console command rather
than a cvar you type a number into, because nobody knows their own eye height
in Half-Life units, and in a headset you cannot read a number off a screen and
do arithmetic anyway. Stand up straight, run it once.
================
*/
static void VR_CalibrateHeight_f( void )
{
	float head;

	if( !VR_IsActive( ))
	{
		Con_Printf( "VR is not running\n" );
		return;
	}

	head = vr.hmd_pose.origin[2];

	if( head < 8.0f )
	{
		Con_Printf( "VR: headset is at %.1f units - too low to be standing height, ignoring\n", head );
		return;
	}

	Cvar_SetValue( "vr_height", head );
	Con_Printf( "VR: standing height calibrated to %.1f units (crouch below %.1f)\n",
		head, head * vr_crouch_ratio.value );
}

/*
================
VR_GetUseSource

Reach out and touch things.

Half-Life is full of buttons, levers, keypads and valves, and pointing at them
from across the room with a crosshair is the least VR thing left in this port.

No new interaction code is needed for it, though - and crucially, no knowledge
of which entities a mod considers usable, which the engine cannot have.
CBasePlayer::PlayerUse() already does the search, and it does it from
pev->origin + pev->view_ofs along pev->v_angle. Point those at the HAND for
the duration of that one call and the mod's own use logic runs from your
fingertips instead of your face, whatever mod it is.

Same substitution the weapon origin uses, aimed at a different call:
PlayerUse runs inside PreThink, not PostThink.

Returns the off hand's pose - the free hand, the one you would actually reach
out with.
================
*/
/*
================
VR_DiagModelAngles

Traces the drawn weapon's angles through every stage that touches them, so a
wrong orientation can be attributed to a stage instead of guessed at. Sampled,
not per frame.

Exists because "the weapon is upside down" has now survived three fixes aimed
at three different suspected causes. Each stage below can plausibly invert a
model, and reading four numbers settles which one does.
================
*/
void VR_DiagModelAngles( const vec3_t raw, const vec3_t after_cal, const vec3_t after_align, const vec3_t final )
{
	static double next = 0.0;

	if( !vr_debug.value )
		return;

	if( host.realtime < next )
		return;

	next = host.realtime + 0.5;

	VR_DiagPrintf( "MODEL raw=(p%7.1f y%7.1f r%7.1f) cal=(p%7.1f y%7.1f r%7.1f) "
		"align=(p%7.1f y%7.1f r%7.1f) final=(p%7.1f y%7.1f r%7.1f) health=%d\n",
		raw[PITCH], raw[YAW], raw[ROLL],
		after_cal[PITCH], after_cal[YAW], after_cal[ROLL],
		after_align[PITCH], after_align[YAW], after_align[ROLL],
		final[PITCH], final[YAW], final[ROLL],
		cl.local.health );
}

/*
================
VR_HandReachable

Is there a clear line from the player's own eye to this hand?

Room-scale lets a hand go where the body cannot follow - most often by being
rested on cover, which puts it through a thin wall. Stock PlayerUse() cannot
notice: it is a sphere search plus an un-normalised dot test with no trace in
it at all. Our substitution then moves its origin onto the hand, which promotes
whatever is on the far side from "unreachable" to "nearest candidate".

The problem this actually causes is not cheating, it is unintended activation.
Contact alone raises IN_USE and holds it for vr_touch_hold afterwards, so
leaning a hand on a wall can press a button on the other side that the player
never reached for and cannot see.

World brushes only. A func_button, func_door or func_breakable must never block
itself, and keeping brush entities non-blocking preserves the existing
reach-extender behaviour for things behind glass.
================
*/
static qboolean VR_HandReachable( const vec3_t hand )
{
	cl_entity_t *player = CL_GetLocalPlayer();
	vec3_t eye, target;
	pmtrace_t tr;

	if( vr_touch_los.value == 0.0f )
		return true;

	// No body to measure from yet - do not start refusing interactions.
	if( !player )
		return true;

	VectorAdd( player->origin, cl.viewheight, eye );
	VectorCopy( hand, target );

	tr = CL_TraceLine( eye, target, PM_STUDIO_IGNORE );

	if( tr.fraction >= 1.0f || tr.ent != 0 )
		return true;

	// Blocked by world - but allow it if the blocker is essentially AT the
	// hand rather than between the player and it.
	//
	// Half-Life recesses its chargers and buttons into wall alcoves, so
	// reaching for one clips the surrounding brush and a strict test refused
	// the interaction - the select sound played and nothing charged. Reaching
	// THROUGH a wall puts the blocker far short of the hand instead, which is
	// the case worth refusing.
	{
		vec3_t miss;

		VectorSubtract( target, tr.endpos, miss );
		return ( VectorLength( miss ) < 10.0f ) ? true : false;
	}
}

qboolean VR_GetTouchContact( void )
{
	vec3_t org, ang, fwd, start, end;
	pmtrace_t tr;
	static qboolean was_touching = false;
	qboolean touching;

	if( !VR_IsActive() || vr_touch_use.value == 0.0f )
	{
		was_touching = false;
		return false;
	}

	if( !VR_GetHandWorld( VR_OffHand(), org, ang ))
	{
		was_touching = false;
		return false;
	}

	if( !VR_HandReachable( org ))
	{
		was_touching = false;
		return false;
	}

	VR_ApplyMeshCalibration( ang, (vec3_t){ vr_hand_pitch_offset.value,
		vr_hand_yaw_offset.value, vr_hand_roll_offset.value } );
	AngleVectors( ang, fwd, NULL, NULL );

	// Short probe through the hand. Which entities are USABLE is the mod's
	// business and the engine cannot know it - so do not try. Detect only that
	// the hand is in contact with something, raise IN_USE, and let the mod's
	// own PlayerUse() decide whether anything happens. Touching a bare wall
	// costs nothing, because there is nothing there to use.
	VectorMA( org, -vr_touch_reach.value, fwd, start );
	VectorMA( org, vr_touch_reach.value, fwd, end );

	tr = CL_TraceLine( start, end, PM_STUDIO_BOX );

	// ONLY ENTITIES COUNT, NEVER THE WORLD.
	//
	// This used to raise IN_USE for anything the hand touched, including
	// plain walls - and pm_shared.c cuts maxspeed to a THIRD while IN_USE is
	// held (PM_CheckParamters). So resting a hand in geometry, which happens
	// constantly in room-scale, silently crippled movement speed.
	//
	// Filtering on tr.ent stays mod-agnostic: everything usable in GoldSrc is
	// a brush or point ENTITY - buttons, doors, chargers, levers - and world
	// geometry is never usable. So the engine can tell the difference without
	// knowing what any of them are.
	touching = ( tr.fraction < 1.0f && tr.ent != 0 ) ? true : false;

	// HOLD contact briefly after the probe stops hitting.
	//
	// Health and HEV chargers are FCAP_CONTINUOUS_USE: PlayerUse() charges
	// only while pev->button still has IN_USE, every frame. A hand resting on
	// a wall is never perfectly still, so the raw probe drops out for a frame
	// here and there - enough to keep re-triggering the "found something"
	// select sound (an EDGE, so it fires on every re-entry) while never
	// holding long enough to actually charge. Reported exactly that way: the
	// interact sound plays but no juice is given.
	//
	// Buttons were unaffected because they are FCAP_IMPULSE_USE and fire on
	// the edge, which flicker supplies plenty of.
	{
		static double hold_until = 0.0;

		if( touching )
			hold_until = host.realtime + vr_touch_hold.value;
		else if( host.realtime < hold_until )
			touching = true;
	}

	// Buzz on the frame contact begins. Without this you are reaching for
	// things you cannot feel, and there is no way to tell you have arrived.
	if( touching && !was_touching )
		VR_Haptic( VR_OffHand(), 0.04f, 0.0f, 0.5f );

	was_touching = touching;
	return touching;
}

qboolean VR_GetUseSource( vec3_t out_org, vec3_t out_ang )
{
	vec3_t ang;

	if( !VR_IsActive() || vr_touch_use.value == 0.0f )
		return false;

	if( !VR_GetHandWorld( VR_OffHand(), out_org, ang ))
		return false;

	// Same reachability rule as the contact probe. Refusing here leaves
	// sv_pmove.c's PreThink substitution unarmed, so the mod's ordinary
	// crosshair-based use runs instead of the hand-sourced one - a graceful
	// degradation rather than the interaction silently doing nothing.
	if( !VR_HandReachable( out_org ))
		return false;

	// Rest-pose correction, so "forward" is where the hand actually points
	// rather than where the raw controller axis does.
	VR_ApplyMeshCalibration( ang, (vec3_t){ vr_hand_pitch_offset.value,
		vr_hand_yaw_offset.value, vr_hand_roll_offset.value } );

	// Sit the use origin BEHIND the hand rather than at it.
	//
	// PlayerUse() builds its line of sight as
	//   vecLOS = target - ( pev->origin + pev->view_ofs )
	// and then dots that against forward WITHOUT NORMALISING, requiring the
	// result to clear VIEW_FIELD_NARROW. Put view_ofs exactly on the hand and
	// touching something shrinks that vector toward zero, so the dot fails and
	// nothing happens. UTIL_ClampVectorToBox then subtracts the target's
	// half-size, which finishes off anything large.
	//
	// That is why small buttons worked while health and HEV chargers did not:
	// a charger is a big brush, so the clamp zeroed its LOS entirely. Backing
	// off restores a vector with real length that still points at whatever the
	// hand is on.
	{
		vec3_t fwd;

		AngleVectors( ang, fwd, NULL, NULL );
		VectorMA( out_org, -vr_touch_backoff.value, fwd, out_org );
	}

	VectorCopy( ang, out_ang );
	return true;
}

/*
================
VR_GetRoomScaleMove

Walking in your room walks in the game.

Without this the play space is anchored to the player entity and physical
movement offsets only the EYE and the HANDS. You can lean through a wall, your
hitbox never leaves the spot you teleported it to with the stick, and NPCs aim
at where the entity stands rather than where you are actually crouched. The
view is room-scale; the body is not.

The fix is to stop treating physical movement as a view offset and start
treating it as INPUT: measure how far the headset has drifted from the point
the body was last synced to, and ask the body to walk there. The entity then
collides with the world normally, because it is moving through the ordinary
movement path rather than being teleported.

That collision is the entire point, and it is also what makes walking into a
real wall behave: the body stops, the offset persists, and you are standing
with your face in geometry until you step back - which is correct, and is what
every room-scale game does short of fading the screen out.

Output is in the usercmd's own frame, so it needs the yaw that cmd will carry.
================
*/
/*
=================================================================
	Weapon identity, learned from the mod's own messages

Half-Life switches weapons BY NAME: the weapon select confirm calls
ServerCmd( gpActiveSel->szName ). So one command switches instantly, and the
only question is what the weapon is called.

The mod tells us, without being asked. Every GoldSrc mod announces its
weapons in a WeaponList user message carrying name, slot, position and id,
and reports the held one in CurWeapon. The engine already intercepts user
messages by name (ScreenShake, ScreenFade in cl_parse.c), so observing two
more costs nothing and the mod still receives them untouched.

That is what makes this mod-agnostic: no weapon table is written here, it is
read from whatever the mod happens to ship.
=================================================================
*/
#define VR_MAX_WEAPON_IDS 64

static struct
{
	char names[VR_MAX_WEAPON_IDS][32];
	int  cur_id;
} vr_wlist;

void VR_ObserveUserMessage( const char *name, int size, const void *buf )
{
	const byte *p = (const byte *)buf;

	if( !name || !p || size <= 0 )
		return;

	if( !Q_strcmp( name, "CurWeapon" ) && size >= 2 )
	{
		// state, id, clip - a negative id means "nothing held".
		signed char id = (signed char)p[1];

		if( id >= 0 && id < VR_MAX_WEAPON_IDS )
			vr_wlist.cur_id = id;

		// The clip count, which the mod sends here and nothing else exposes.
		// A reload that raises it by exactly one is a tube being fed a shell;
		// one that jumps it to full is a magazine. Recorded now because that
		// distinction is the only thing separating the two reload styles, and
		// reading it from the running game beats keeping a list of weapons.
		if( size >= 3 )
			vr.rl_clip = (signed char)p[2];

		return;
	}

	if( !Q_strcmp( name, "WeaponList" ))
	{
		// null-terminated name, then ammo1/max1/ammo2/max2/slot/pos/id/flags
		int len = 0;

		while( len < size && p[len] )
			len++;

		if( len > 0 && len + 8 < size )
		{
			int id = p[len + 7];

			if( id >= 0 && id < VR_MAX_WEAPON_IDS )
				Q_strncpy( vr_wlist.names[id], (const char *)p, sizeof( vr_wlist.names[id] ));
		}
	}
}

// Name of the weapon currently held, or NULL if the mod has not said.
static const char *VR_CurrentWeaponName( void )
{
	if( vr_wlist.cur_id < 0 || vr_wlist.cur_id >= VR_MAX_WEAPON_IDS )
		return NULL;
	if( !vr_wlist.names[vr_wlist.cur_id][0] )
		return NULL;
	return vr_wlist.names[vr_wlist.cur_id];
}

/*
================
VR_NeckOrigin

Where the base of the neck is, in play space.

A head does not rotate in place - it swings on a neck, so tilting or leaning
TRANSLATES the headset by several units without the body going anywhere. Room
scale reads that translation as intent to walk, and the player drifts in
whichever direction they tilted. No dead zone fixes it: leaning clears any
threshold big enough to still allow real stepping.

Measuring from the neck pivot instead removes the whole class of error. The
pivot barely moves when the head turns or tilts, and moves fully when the body
walks - which is exactly the distinction room-scale needs and could not
otherwise make.
================
*/
static void VR_NeckOrigin( const vr_pose_t *pose, vec3_t out )
{
	vec3_t fwd, right, up;

	VectorCopy( pose->origin, out );

	if( vr_neck_model.value == 0.0f )
		return;

	AngleVectors( pose->angles, fwd, right, up );

	// Down and back from the headset, in the HEAD's own frame - that is what
	// makes the pivot stay put while the head swings around it.
	VectorMA( out, -vr_neck_up.value, up, out );
	VectorMA( out, -vr_neck_fwd.value, fwd, out );
}

void VR_GetRoomScaleMove( float view_yaw, float *forward, float *side )
{
	vec3_t rel, world;
	float s, c, gain, len, cap;

	*forward = *side = 0.0f;

	// Cleared on every path out, so a frame that commands nothing publishes
	// nothing and VR_SetWorldReference leaves the sync point alone.
	VectorClear( vr.roomscale_cmd );
	vr.roomscale_cmd_valid = false;

	if( !VR_IsActive() || vr_roomscale.value == 0.0f )
		return;

	if( !vr.hmd_origin_at_sync_valid )
		return;

	// DISPLACEMENT MATCHING, not a position servo.
	//
	// This used to measure how far the head had drifted from a sync point and
	// command movement to close that gap. That is a servo, and it has a
	// structural fault: standing off-centre in the play space IS a permanent
	// gap, so it commanded movement forever and the player drifted while
	// standing still. Widening the dead zone only enlarges the region where
	// room-scale does not work; it cannot fix it, because the error being
	// integrated is a position rather than a motion.
	//
	// Match the displacement instead: the head moved this far this frame, so
	// move the body that far. No target, no gap, nothing to converge on.
	//
	// The property that matters is that it is MEMORYLESS. Standing off-centre
	// is not motion, so it commands nothing however far off-centre it is.
	// Sway nets to zero because the deltas cancel. Errors cannot accumulate
	// into sustained motion, which is what a dead zone was papering over -
	// so all that is left to reject is genuine tracker noise.
	//
	// It is also what room-scale physically means. You do not move because
	// your head is somewhere; you move because your head went somewhere.
	{
		vec3_t neck_now, delta;
		float len;

		VR_NeckOrigin( &vr.hmd_pose, neck_now );

		if( !vr.neck_prev_valid )
		{
			VectorCopy( neck_now, vr.neck_prev );
			vr.neck_prev_valid = true;
			return;
		}

		VectorSubtract( neck_now, vr.neck_prev, rel );
		VectorCopy( neck_now, vr.neck_prev );
		rel[2] = 0.0f;	// horizontal only - height is VR_GetPhysicalCrouch

		len = VectorLength( rel );

		// Only the tracker's own jitter is rejected now, not a band of real
		// movement. A tenth of a millimetre per frame is noise; anything a
		// person actually does is far above it.
		if( len < Q_max( 0.0f, vr_roomscale_noise.value ))
			return;
	}

	// Play space -> world, by the same body yaw the eyes use.
	SinCos( DEG2RAD( vr.body_yaw ), &s, &c );
	world[0] = rel[0] * c - rel[1] * s;
	world[1] = rel[0] * s + rel[1] * c;
	world[2] = 0.0f;

	// forwardmove and sidemove are velocities, and this is a displacement, so
	// dividing by frametime asks for exactly the distance the head covered -
	// no more, no less. vr_roomscale_gain is left as a trim for players who
	// want more or less than 1:1 in a small play space.
	if( host.frametime > 0.0 )
		VectorScale( world, vr_roomscale_gain.value / (float)host.frametime, world );

	// Cap it. A tracking glitch that reports the headset several metres away
	// would otherwise fire the player across the map in one frame.
	cap = vr_roomscale_max.value;
	len = VectorLength( world );
	if( len > cap && len > 0.0f )
		VectorScale( world, cap / len, world );

	// Publish what we just asked the body to do, for the loop-closing test.
	VectorCopy( world, vr.roomscale_cmd );
	vr.roomscale_cmd_valid = true;

	// World -> the usercmd's frame. forwardmove/sidemove are interpreted
	// along cmd->viewangles, which is NOT necessarily the head yaw: while
	// aiming from the weapon it carries the WEAPON yaw instead, so using the
	// head's would send the body off at an angle.
	SinCos( DEG2RAD( view_yaw ), &s, &c );
	*forward = world[0] * c + world[1] * s;
	*side = -world[0] * s + world[1] * c;
}

/*
================
VR_GetLadderMove

Hand-over-hand ladder climbing.

Worth doing here rather than writing it off as game-DLL territory: ladder
movement in Xash lives in pm_shared, which is ENGINE code, and the climb is
driven by usercmd upmove. So the whole thing is expressible as input, and stays
mod-agnostic - both reference ports needed an SDK fork for this.

The gesture is the physical one: grab with a grip button and pull down, and you
go up. Vertical hand velocity in tracking space drives it, which means it is
independent of where you are looking or facing.

Returns 0 when not climbing.
================
*/
/*
================
VR_LadderHands

True while the player is on a ladder and climbing it by hand.

You cannot climb a ladder holding a shotgun. The weapon is stowed for the
duration - drawn away, unable to fire, and both hands appear on the rungs -
and comes back the instant you step off. Automatic rather than a holster the
player has to remember: Half-Life has a great many ladders, and a required
gesture at each one is friction, not immersion.

It also removes the reason hand-over-hand felt pointless: stock GoldSrc
ladder movement is driven by forwardmove along the view direction, so pushing
the stick climbs perfectly well and the hands were decoration on top of it.
Suppressing the stick while on a ladder is what makes the hands the actual
means of climbing.
================
*/
/*
================
VR_GetLadderClimb

The climb rate computed this frame, without recomputing it.

VR_GetLadderMove differentiates hand height, so calling it twice in a frame
would consume the same movement twice and halve it. The server reads this
instead.
================
*/
float VR_GetLadderClimb( void )
{
	return vr.ladder_climb;
}

qboolean VR_LadderHands( void )
{
	// GRIPPING, not merely on a ladder. VR_OnLadder is proximity - it is true
	// whenever the player is against the brush - so keying the stow and the
	// stick suppression off it killed movement for standing near a ladder and
	// left no way to walk back off.
	return ( VR_IsActive() && vr_ladder_hands.value != 0.0f
		&& vr_ladder.value != 0.0f && vr.ladder_gripping ) ? true : false;
}

qboolean VR_OnLadder( void )
{
	int i;
	vec3_t p;

	if( !clgame.pmove )
		return false;

	VectorCopy( cl.simorg, p );

	// Ladders are brush entities marked SOLID_NOT with skin == CONTENTS_LADDER,
	// and the engine already separates them into the moveents list for exactly
	// this reason (see cl_pmove.c:435, sv_pmove.c:296). So the engine can tell
	// you are on a ladder WITHOUT asking the mod - which is what lets the whole
	// climbing gesture stay mod-agnostic.
	for( i = 0; i < clgame.pmove->nummoveent; i++ )
	{
		physent_t *pe = &clgame.pmove->moveents[i];
		vec3_t mins, maxs;
		int j;

		if( !pe->model )
			continue;

		VectorAdd( pe->origin, pe->model->mins, mins );
		VectorAdd( pe->origin, pe->model->maxs, maxs );

		// Generous margin: the brush is the ladder's climbable volume, and you
		// stand just outside it rather than inside.
		for( j = 0; j < 3; j++ )
		{
			// 16, not 24. PM_Ladder expands the brush by the player hull and
			// then demands the origin be inside it, so a wider margin here just
			// creates a band where the gesture engages and the game refuses to
			// move anybody.
			mins[j] -= 16.0f;
			maxs[j] += 16.0f;
		}

		// AND STOPS DEAD AT THE TOP OF THE RUNGS.
		//
		// This once reached 36 units higher, copying HLVR, so a last pull
		// could carry the player over the lip. That belonged to a cresting
		// scheme that no longer exists, and it actively breaks the dismount
		// that replaced it.
		//
		// Getting off a ladder is ordinary walking, and the whole ladder
		// system exists to suppress ordinary walking: the climb clears the
		// movement buttons, and hands-only strips any stick input heading
		// into the rungs. Staying "on the ladder" above the lip therefore
		// left the player hanging there with the climb still steering by
		// gaze - sliding them sideways at the exact moment they were trying
		// to step onto the platform.
		//
		// Ending here makes every part of it go inert at once, from one
		// test, and hands the player back to the movement the game has
		// always used to step off a ladder.

		if( p[0] >= mins[0] && p[0] <= maxs[0] &&
		    p[1] >= mins[1] && p[1] <= maxs[1] &&
		    p[2] >= mins[2] && p[2] <= maxs[2] )
		{
			// Keep where the rungs are. Which way the ladder faces is what
			// decides whether a pull climbs it or shoves the player off it,
			// and this loop is the only place that knows.
			VectorAdd( pe->model->mins, pe->model->maxs, vr.ladder_center );
			VectorScale( vr.ladder_center, 0.5f, vr.ladder_center );
			VectorAdd( vr.ladder_center, pe->origin, vr.ladder_center );
			VectorSubtract( pe->model->maxs, pe->model->mins, vr.ladder_size );
			vr.ladder_have_dir = true;
			return true;
		}
	}

	return false;
}

/*
================
VR_GetLadderDir

Which way the ladder is, flat, from where the player stands.

Needed because pm_shared decides what a climb input MEANS from the view
angles: it dots the intended velocity against the ladder face and turns the
part going into the ladder to vertical. Face the rungs and forward climbs;
face away and the same input is read as stepping off, which on the ground it
answers by shoving the player clear at full climb speed.

That is correct for a keyboard, where forward is wherever you look. It is
wrong for hands, where the pull carries its own direction and the head is
free to look anywhere - including down at the very rung being grabbed, which
is exactly when it threw the player backwards off the ladder.
================
*/
/*
================
VR_LadderHandsOnly

Whether the stick is allowed to climb a ladder at all.

It climbs perfectly well on its own - pm_shared turns any movement into the
rungs into vertical motion, so a ladder goes up under stick input whether or
not anyone reaches for it. That leaves hand-over-hand as decoration: the
faster, duller option is always right there, and the climb never has to be
performed to be passed.

Off by default is the wrong default while the gesture is still being proven -
if the stick is available it is what gets used, and the hands never get
tested. A cvar rather than a hard removal because this is a comfort choice in
the end, not a correctness one, and the launcher will want to offer it.
================
*/
qboolean VR_LadderHandsOnly( void )
{
	return ( VR_IsActive() && vr_ladder.value != 0.0f
		&& vr_ladder_hands_only.value != 0.0f ) ? true : false;
}

qboolean VR_GetLadderDir( vec3_t out )
{
	vec3_t d;

	if( !VR_IsActive() || !vr.ladder_have_dir )
		return false;

	VectorSubtract( vr.ladder_center, cl.simorg, d );
	d[2] = 0.0f;

	// SQUARE ON TO THE FACE, not at the middle of the rungs.
	//
	// pm_shared splits the intended velocity against the ladder face and
	// keeps both halves: the part going INTO the face becomes the climb, and
	// the part running ALONG it survives as real sideways movement -
	//
	//     VectorSubtract( velocity, cross, lateral );
	//     VectorMA( lateral, -normal, tmp, pmove->velocity );
	//
	// Pointing at the centre therefore slid the player sideways by exactly
	// how far off-centre they were standing when they took hold. Grab a
	// ladder a little to its right and you drift left, every time.
	//
	// A ladder brush is a thin axis-aligned box, so the face is normal to
	// whichever horizontal axis it is thinnest on. Dropping the other axis
	// leaves a purely perpendicular approach - no lateral term at all, and
	// the climb comes out vertical wherever along the rungs you grabbed.
	if( vr.ladder_size[0] < vr.ladder_size[1] )
		d[1] = 0.0f;
	else
		d[0] = 0.0f;

	if( VectorNormalizeLength( d ) == 0.0f )
		return false;

	VectorCopy( d, out );
	return true;
}

float VR_GetLadderMove( void )
{
	static float last_z[2];
	static qboolean have_last[2];
	float move = 0.0f;
	qboolean held = false;
	int hand;

	// ONLY while actually on a ladder.
	//
	// This is what makes the gesture safe. Both reference ports had to pick a
	// side here: HLVR gets real hand-over-hand climbing but pays for it with a
	// forked SDK - a custom net message, per-controller ladder state and their
	// object-drag system (player.cpp:314, :5958, VRControllerInteractionManager
	// .cpp:1204). Lambda1VR stays engine-side but only steers, using the stick
	// with direction taken from the HMD or off hand (L1VR_SurfaceView.c:400).
	//
	// Neither had a way to ask the engine "am I on a ladder". We do, because
	// ladders are identifiable brushes (VR_OnLadder), so we get HLVR's gesture
	// with Lambda1VR's mod-agnosticism.
	//
	// It also resolves the button clash. The dominant-hand grip is +attack2, so
	// reading it as "grab rung" everywhere would fire the gauss or the shotgun's
	// double barrel mid-climb. Gated to a ladder, that cannot happen: nobody
	// shoots while hauling themselves up one.
	if( !VR_IsActive() || vr_ladder.value == 0.0f || !VR_OnLadder( ))
	{
		have_last[0] = have_last[1] = false;
		vr.ladder_gripping = false;
		vr.ladder_climb = 0.0f;
		return 0.0f;
	}

	for( hand = 0; hand < 2; hand++ )
	{
		const vr_pose_t *pose = VR_GetHandPose( hand );
		qboolean gripping;
		float z;

		if( !pose || !pose->valid )
		{
			have_last[hand] = false;
			continue;
		}

		gripping = ( hand == VR_OffHand() ) ? VR_GetButton( VR_BTN_OFFGRIP ) : VR_GetButton( VR_BTN_ATTACK2 );

		// PLAY SPACE, which is what hand_pose already is.
		//
		// It is the pose fed INTO VR_PlayToWorld, not the result of it, so it
		// is measured against the room and carries none of the player's world
		// motion. A pull is therefore already immune to the climb it causes.
		//
		// Subtracting the player origin here to "fix" a feedback loop added
		// one instead: it injected world motion into a room-space number, so
		// climbing inflated the very pull driving it and the value pinned to
		// its ceiling on contact - a hundred and eighty units a second, held,
		// for as long as a rung was touched.
		z = pose->origin[2];

		if( gripping && have_last[hand] && host.frametime > 0.0 )
		{
			// Pulling the hand DOWN (z decreasing) climbs UP.
			//
			// A VELOCITY, not a per-frame delta. This was
			//     ( last_z - z ) * speed
			// which is two bugs at once: a brisk pull moves the hand about 0.2
			// units per frame, so at the old default of 8 it produced an upmove
			// of roughly 1.6 against the hundreds pm_shared expects - and being
			// per-frame it also climbed at different rates on different hardware.
			// Dividing by frametime gives units per second, which is the same
			// quantity forwardmove and upmove are already expressed in.
			move += (( last_z[hand] - z ) / (float)host.frametime ) * vr_ladder_speed.value;
		}

		last_z[hand] = z;
		have_last[hand] = gripping;

		if( gripping )
			held = true;
	}

	// Whether a rung is actually being HELD, as opposed to merely standing
	// against a ladder brush. VR_OnLadder is proximity, and suppressing the
	// stick on proximity alone stranded the player: movement went dead just
	// for being near a ladder, with no way to walk back off it.
	// CAP IT. The pull is a real hand velocity, and a sharp jerk is easily
	// several metres per second - unclamped that launches the player up the
	// shaft like a superhero. The cap is near GoldSrc MAX_CLIMB_SPEED, so a
	// hard pull tops out at about the speed the game would climb anyway while
	// a gentle one stays proportional.
	{
		float cap = Q_max( 1.0f, vr_ladder_max.value );

		if( move > cap ) move = cap;
		else if( move < -cap ) move = -cap;
	}

	// HOLD IT BRIEFLY after the last gripping frame.
	//
	// Tracked grip is not perfectly steady, and a single dropped frame here
	// unstows the weapon, re-arms the weapon-cycle modifier and cancels the
	// climb - all of which the player sees as the ladder working sometimes
	// and not others. Nobody lets go of a rung for a sixtieth of a second
	// on purpose, so a short hold costs nothing and removes the flicker.
	{
		static double hold_until = 0.0;

		if( held )
			hold_until = host.realtime + 0.25;
		else if( host.realtime < hold_until )
			held = true;
	}

	vr.ladder_gripping = held;
	vr.ladder_climb = move;

	if( vr_diag.value != 0.0f && ( held || move != 0.0f ))
		{
			vec3_t ld;
			qboolean have = VR_GetLadderDir( ld );

			VR_DiagPrintf( "LADDER held=%d move=%.1f onladder=%d size=(%.0f %.0f %.0f) dir=%s(%.2f %.2f)\n",
				held ? 1 : 0, move, VR_OnLadder() ? 1 : 0,
				vr.ladder_size[0], vr.ladder_size[1], vr.ladder_size[2],
				have ? "ok" : "--", have ? ld[0] : 0.0f, have ? ld[1] : 0.0f );
		}

	return move;
}

/*
================
VR_DualWieldActive / VR_GetOffhandFire

VR akimbo. The off hand fires the equipped weapon a second time, from its own
controller, on its own cooldown.

The engine cannot create a second weapon - GoldSrc has one m_pActiveItem and
one refire timer, and both live in the game DLL. So the split is: the DLL owns
the second shot (CBasePlayer::DualWieldPostFrame in hlsdk-portable), and the
engine owns WHERE that shot comes from, handed over in pev->vuser1/vuser2 -
vec3 fields entvars_t reserves "For mods" and nothing in stock Half-Life reads.
Same shape as the view_ofs substitution that moves the main hand's shot to the
muzzle, just aimed at fields the DLL is free to interpret.

Requires our own hl.dll, so this is the one VR feature that is NOT mod-
agnostic. It stays off unless a game DLL that understands it is loaded.
================
*/
qboolean VR_DualWieldActive( void )
{
	if( !VR_IsActive() || vr_dual_wield.value == 0.0f )
		return false;

	// Only while actually holding something.
	return ( clgame.viewent.model != NULL ) ? true : false;
}

qboolean VR_GetOffhandFire( vec3_t out_org, vec3_t out_dir )
{
	vec3_t org, ang, fwd;

	if( !VR_DualWieldActive( ))
		return false;

	if( !VR_GetHandWorld( VR_OffHand(), org, ang ))
		return false;

	// EXACT path: read the off-hand gun's own muzzle attachment, filled by the
	// renderer as it drew vr_offhand_ent. This is the same fix that got the
	// main hand's laser onto its barrel, and for the same reason - no tuned
	// angle can ever converge, because each model's barrel sits at its own
	// arbitrary orientation, so the error is not a constant rotation.
	//
	// Without this the off hand fell back to the calibrated-angle path below,
	// which is precisely the path that used to send the main hand's laser
	// straight up. Reported live as exactly that.
	if( vr_aim_attachment.value != 0.0f && VR_ModelHasBore( vr_offhand_ent.model ))
	{
		vec3_t bore;

		VectorSubtract( vr_offhand_ent.attachment[1], vr_offhand_ent.attachment[0], bore );

		if( VectorLength( bore ) > 0.01f )
		{
			VectorNormalize( bore );
			VectorCopy( vr_offhand_ent.attachment[0], out_org );
			VectorCopy( bore, out_dir );
			return true;
		}
	}

	// FALLBACK, for a model carrying no usable attachment: rest-pose
	// correction on the tracked angles, and push the origin forward out of the
	// fist so the shot leaves a barrel rather than the palm.
	VR_CalibrateWeaponAngles( ang );
	AngleVectors( ang, fwd, NULL, NULL );

	VectorMA( org, vr_offhand_muzzle.value, fwd, out_org );
	VectorCopy( fwd, out_dir );

	return true;
}

/*
================
VR_SetFireWindow / VR_CheckTraceOutsideWindow

Diagnostic only - never changes a trace, a position or an angle.

"Fire from the controller" works by substituting entvars_t.view_ofs around the
pfnPlayerPostThink call (sv_pmove.c). That is correct for every weapon path
verified against real SDK source: hitscan, and the projectile weapons too, since
RPG/crossbow/hornet/snark/satchel all derive their spawn point from
GetGunPosition() inside PrimaryAttack/WeaponIdle -> ItemPostFrame -> PostThink.

But that cannot be PROVEN for arbitrary mods. A mod is free to override
ItemPreFrame, or fire out of a custom think, and then its shot would leave the
eye with our substitution not in scope. Rather than guess - or, worse, start
heuristically rewriting pfnTraceLine, which would corrupt every monster's
FVisible() line-of-sight, since that traces to EyePosition() = origin + view_ofs
- just WATCH for it and say so.

If a trace begins at the VR player's eye while the trigger is held and we are
NOT inside the substitution window, that is a mod firing somewhere we do not
cover. The log line names the exact spot to widen to. Silence across a mod is
positive evidence that PostThink covers it.
================
*/
static int      vr_fire_phase;		// VR_FIRE_PHASE_*
static qboolean vr_fire_eye_valid;
static vec3_t   vr_fire_eye;		// VR player's real (unsubstituted) eye
static int      vr_fire_buttons;
static double   vr_fire_warn_time;

void VR_SetFirePhase( int phase, const float *eye, int buttons )
{
	vr_fire_phase = phase;
	vr_fire_buttons = buttons;

	if( eye )
	{
		VectorCopy( eye, vr_fire_eye );
		vr_fire_eye_valid = true;
	}
	else vr_fire_eye_valid = false;
}

void VR_CheckTraceOutsideWindow( const float *start )
{
	vec3_t delta;

	if( !vr_debug.value || !vr_fire_eye_valid || !start )
		return;

	// ONLY PreThink. Not "anything outside PostThink" - that first version
	// stayed armed for the whole server frame and flagged AI and physics
	// traces that merely began near the player's eye, which is noise, not a
	// weapon. PreThink is the one other place the game DLL is handed this
	// usercmd, so it is the only realistic uncovered fire path (a mod
	// overriding ItemPreFrame). Everything outside SV_RunCmd entirely is AI
	// and physics and cannot be usercmd-driven weapon fire.
	if( vr_fire_phase != VR_FIRE_PHASE_PRETHINK )
		return;

	if( !FBitSet( vr_fire_buttons, IN_ATTACK ))
		return;

	VectorSubtract( start, vr_fire_eye, delta );

	// Anything further than this is ordinary world tracing, not a shot
	// originating at the player's eye.
	if( VectorLength( delta ) > 2.0f )
		return;

	// A mod that does this every frame would otherwise flood the log.
	if( host.realtime - vr_fire_warn_time < 1.0 )
		return;

	vr_fire_warn_time = host.realtime;

	VR_DiagPrintf( "VR: fire outside window - trace from eye (%.1f %.1f %.1f) "
		"with IN_ATTACK held, during PreThink. This mod fires from a path the "
		"PostThink substitution does not cover (ItemPreFrame?).\n",
		vr_fire_eye[0], vr_fire_eye[1], vr_fire_eye[2] );
}

/*
================
VR_UpdateFireRay / VR_GetFireRay

THE single source of truth for where the weapon shoots.

Previously the laser sight and the usercmd each called VR_GetWeaponAim
separately. They were supposed to agree by construction, and did not: the
laser rendered ~90 degrees off the actual firing line, because the two reads
happened at different points in the frame and did not see the same
calibration state.

Now it is computed ONCE per client frame and cached. The laser draws this
ray, and the usercmd is built from this ray. They cannot diverge, whatever
the calibration does - if aim is wrong, the laser is visibly wrong in exactly
the same way, which makes it a real sight instead of a decoration that lies.
================
*/
static vec3_t vr_fire_org;
static vec3_t vr_fire_ang;
static qboolean vr_fire_valid = false;

void VR_UpdateFireRay( void )
{
	vec3_t org, ang;

	vr_fire_valid = false;

	if( !VR_IsActive() || !VR_GetHandWorld( VR_DominantHand(), org, ang ))
		return;

	qboolean braced = VR_ApplyTwoHandedAim( org, ang );
	qboolean used_attachment = false;

	// PREFERRED: take the firing line straight off the weapon model's own
	// MUZZLE ATTACHMENT, which ref_gl fills in world space every frame
	// (gl_studio.c:1227). This is what HLVR does (VRHelper.cpp GetGunPosition
	// / GetAutoaimVector via viewent->attachment[]).
	//
	// Why not a tuned angle: reported live that no single pitch value could
	// be made to line up. That is the tell that the error is NOT a constant
	// pitch. The barrel sits inside the model at its own arbitrary
	// orientation - generally pitch AND yaw AND roll - so a pitch-only
	// correction can never converge, and every weapon would need different
	// numbers anyway. The attachment is authored into each model, so it is
	// exact and it is per-weapon for free.
	//
	// attachment[0] is the muzzle; attachment[1], where present, is a second
	// point down the barrel, so [0]->[1] is the true bore line.
	if( !braced && vr_aim_attachment.value && VR_ModelHasBore( clgame.viewent.model ))
	{
		const cl_entity_t *ve = &clgame.viewent;
		vec3_t bore;

		VectorSubtract( ve->attachment[1], ve->attachment[0], bore );

		if( VectorLength( bore ) > 0.1f )
		{
			vec3_t va;

			VectorNormalize( bore );
			VectorAngles( bore, va );

			// VectorAngles is pitch-positive-up; entity/usercmd angles use
			// AngleVectors' positive-down convention.
			ang[PITCH] = -va[PITCH];
			ang[YAW]   = va[YAW];

			// Muzzle itself is the shot origin, so tracers and decals leave
			// the barrel rather than the grip.
			VectorCopy( ve->attachment[0], org );
			used_attachment = true;
		}
	}

	// SECOND BEST: muzzle POSITION from one attachment, direction from the hand.
	//
	// A single attachment pins where the barrel ends but says nothing about
	// which way it points, so the bore test above - which needs two - rejects
	// it outright. That was throwing the muzzle away on exactly the content
	// this fork exists to play: every retail Half-Life, Opposing Force and Blue
	// Shift gun carries precisely ONE attachment (the muzzle flash point), and
	// melee weapons and throwables carry none. So on genuine Valve content the
	// bore path never ran, and BOTH origin and direction fell back to a
	// hand-tuned constant. It only ever appeared to work because VR-authored
	// models with four attachments each happened to be installed over the top.
	//
	// Taking just the origin is a strict improvement and costs nothing:
	// tracers, decals and the mod's own trace all start at the real barrel tip
	// instead of the grip - which is what makes contact-range shots behave -
	// while direction still comes from the calibrated controller pose below.
	if( !braced && !used_attachment && vr_aim_attachment.value
		&& cl.local.viewmodel != 0
		&& VR_ModelAttachments( clgame.viewent.model ) >= 1 )
	{
		VectorCopy( clgame.viewent.attachment[0], org );
	}

	// FALLBACK for models with no usable muzzle attachment.
	//
	// Stock Half-Life viewmodels all carry one, but a MOD's custom weapons
	// may not - and without this the angles would be left as the raw
	// controller pose, roughly 45 degrees off, so every custom gun in every
	// mod would shoot wide. Falling back to the mesh rest-pose correction
	// restores the pre-attachment behaviour, which was close, so an
	// unattributed mod weapon degrades gracefully instead of breaking.
	//
	// ONLY when a weapon is actually held. The correction exists to cancel a
	// MESH's rest pose, so with no mesh there is nothing to cancel and the raw
	// hand pose is already the right answer. This matters on mounted guns:
	// CFuncTank::StartControl holsters the weapon and sets viewmodel = 0, so
	// without this test the tank would be aimed through vr_weapon_pitch_offset
	// - 45 degrees above where the player is pointing.
	if( !braced && !used_attachment && cl.local.viewmodel != 0 )
		VR_CalibrateWeaponAngles( ang );

	// Manual per-axis override, for dialling in one weapon by hand.
	// Skipped while braced: the two-handed angles are already a finished
	// world direction, so layering a rest-pose correction on top is what
	// sent the weapon wild. Lambda1VR skips its equivalent the same way.
	// Also weapon-only, and for the same reason as the fallback above: this is
	// a per-weapon dial, so with nothing in hand there is nothing to dial.
	if( !braced && cl.local.viewmodel != 0
		&& ( vr_aim_pitch_offset.value != 0.0f || vr_aim_yaw_offset.value != 0.0f ))
	{
		vec3_t cal = { vr_aim_pitch_offset.value, vr_aim_yaw_offset.value, 0.0f };
		VR_ApplyMeshCalibration( ang, cal );
	}

	// GEOMETRY HONESTY - keep the shot origin on the player's side of the world.
	//
	// The muzzle is a freely positionable tracked controller, so it can be put
	// somewhere the player's body is not: through a thin wall, inside a
	// doorframe, buried in a crate they are bracing against. Two separate
	// things go wrong, and the second one is the reason this is not optional:
	//
	//   1. Shooting through walls. sv_pmove.c substitutes this origin into
	//      view_ofs, so the mod's own trace starts wherever the muzzle is -
	//      including the far side of cover. A desktop player cannot do that,
	//      so leaving it would also be a crossplay-fairness hole on a listen
	//      server.
	//   2. Bullets that quietly do nothing. When the muzzle is INSIDE solid,
	//      the mod's UTIL_TraceLine from GetGunPosition() starts in solid and
	//      returns fraction 0, so the shot dies at the muzzle. The player
	//      empties a magazine into the wall they are leaning on and gets no
	//      damage, no impact, and no feedback at all. Room-scale players hit
	//      this constantly and cannot avoid it.
	//
	// Clamping to the last clear point fixes both at once, and is strictly
	// better than refusing to substitute: the shot still leaves the barrel tip
	// rather than reverting to the face, it just leaves it at the surface the
	// barrel is pressed against - which is where a real barrel would be.
	//
	// World only. A brush entity the player is legitimately shooting or
	// standing on (func_breakable, glass, a lift) must not clamp the shot, and
	// PM_STUDIO_IGNORE keeps a monster stepping between body and muzzle from
	// counting as cover.
	{
		cl_entity_t *player = CL_GetLocalPlayer();

		if( player )
		{
			vec3_t eye;
			pmtrace_t tr;

			VectorAdd( player->origin, cl.viewheight, eye );
			tr = CL_TraceLine( eye, org, PM_STUDIO_IGNORE );

			if( tr.fraction < 1.0f && tr.ent == 0 )
			{
				vec3_t back;

				// Sit just short of the surface, not exactly on it: a trace
				// starting precisely on a plane is the startsolid case this
				// exists to avoid.
				VectorSubtract( eye, org, back );
				VectorNormalize( back );
				VectorMA( tr.endpos, 1.0f, back, org );
			}
		}
	}

	VectorCopy( org, vr_fire_org );
	VectorCopy( ang, vr_fire_ang );
	vr_fire_valid = true;

	if( vr_diag_aim.value > 0.0f )
	{
		vec3_t fwd;

		Cvar_DirectSet( &vr_diag_aim, va( "%d", (int)vr_diag_aim.value - 1 ));
		AngleVectors( vr_fire_ang, fwd, NULL, NULL );
		VR_DiagPrintf( "FIRERAY org=(%.1f %.1f %.1f) ang=(p%.1f y%.1f r%.1f) "
			"fwd=(%.2f %.2f %.2f) wpitch=%.0f melee=%d origin_ovr=%d\n",
			vr_fire_org[0], vr_fire_org[1], vr_fire_org[2],
			vr_fire_ang[PITCH], vr_fire_ang[YAW], vr_fire_ang[ROLL],
			fwd[0], fwd[1], fwd[2],
			vr_weapon_pitch_offset.value, VR_HoldingMelee() ? 1 : 0,
			VR_WeaponOriginActive() ? 1 : 0 );
	}
}

qboolean VR_GetFireRay( vec3_t out_org, vec3_t out_ang )
{
	if( !vr_fire_valid )
		return false;

	if( out_org ) VectorCopy( vr_fire_org, out_org );
	if( out_ang ) VectorCopy( vr_fire_ang, out_ang );
	return true;
}

/*
================
VR_GetAimAngles

Angles to put in the usercmd so the shot LANDS where the weapon is pointing.

The mod's weapon code fires from CBasePlayer::GetGunPosition(), which in
stock Half-Life is pev->origin + pev->view_ofs - the player's eye. That is
game-DLL code, so an engine-only VR layer cannot move the shot's origin.
Both reference ports solved it by overriding GetGunPosition() in a forked
hlsdk (HLVR player.cpp:4763 returns the muzzle attachment, Lambda1VR
player.cpp:371 returns the controller position). That is not an option here:
the whole point is to play ARBITRARY mods with their own custom content,
and a forked hl.dll only fixes the one mod it was built for.

So instead of moving the origin, aim the origin we are stuck with:

  1. trace from the real muzzle along the real barrel -> impact point P
     (the same trace the laser sight draws, so they agree by construction)
  2. hand back the angles from the EYE to P

The bullet still leaves the eye, but it now converges on exactly the point
the gun is pointed at, so shots land on the laser dot. What remains is
cosmetic: server-spawned tracers still originate at the head.

Falls back to the plain weapon angles if the player entity is not available.
================
*/
qboolean VR_GetAimAngles( vec3_t out_ang )
{
	vec3_t muzzle, ang, fwd, target, eye, dir, va;
	cl_entity_t *player;
	pmtrace_t tr;

	if( !VR_GetWeaponAim( muzzle, ang ))
		return false;

	// The eye convergence below exists ONLY for the case where the shot origin
	// is stuck at the eye. With vr_weapon_origin on, sv_pmove.c has already
	// moved the origin to the muzzle, so the barrel's own direction is already
	// correct and converging on top of it stacks two corrections instead of
	// cancelling them: the shot then leaves the MUZZLE along an EYE-relative
	// direction. That is a roughly constant lateral miss equal to the
	// muzzle-eye offset (~20-25 units) - under a degree across a room, but
	// ~20 degrees at contact range, which is what "emptied a magazine into him
	// point blank and he took no damage" looks like.
	if( VR_WeaponOriginActive( ))
	{
		VectorCopy( ang, out_ang );
		return true;
	}

	AngleVectors( ang, fwd, NULL, NULL );

	player = CL_GetLocalPlayer();
	if( !player )
	{
		VectorCopy( ang, out_ang );
		return true;
	}

	// Where the barrel is actually pointed.
	VectorMA( muzzle, vr_laser_range.value, fwd, target );
	tr = CL_TraceLine( muzzle, target, PM_STUDIO_BOX );
	VectorCopy( tr.endpos, target );

	// The eye the game will fire from.
	VectorAdd( player->origin, cl.viewheight, eye );

	VectorSubtract( target, eye, dir );
	if( VectorNormalizeLength( dir ) == 0.0f )
	{
		VectorCopy( ang, out_ang );
		return true;
	}

	VectorAngles( dir, va );

	// VectorAngles is pitch-positive-up; usercmd view angles follow the
	// AngleVectors convention where positive pitch is DOWN.
	out_ang[PITCH] = -va[PITCH];
	out_ang[YAW]   = va[YAW];
	out_ang[ROLL]  = 0.0f;

	return true;
}

qboolean VR_GetWeaponAim( vec3_t out_org, vec3_t out_ang )
{
	vec3_t org, ang;

	// Delegates to the cached per-frame fire ray so every consumer - laser,
	// arc, usercmd, muzzle origin - reads the exact same numbers. See
	// VR_UpdateFireRay for why they must not be recomputed independently.
	if( VR_GetFireRay( out_org, out_ang ))
		return true;

	if( !VR_GetHandWorld( VR_DominantHand(), org, ang ))
		return false;

	VR_ApplyTwoHandedAim( org, ang );

	// APPLY the mesh correction, and that is the whole point.
	//
	// This was deliberately skipped at first, on the theory that the
	// correction was purely cosmetic - that it only nudged the model into
	// place while the raw aim pose remained the true pointing direction.
	// Live testing proved that wrong: with the correction at -45, the laser
	// sight and every bullet went 45 degrees UP from the barrel of the gun
	// you could actually see.
	//
	// The correction is what rotates the visible weapon into alignment, so
	// the direction the player sees the barrel pointing IS the calibrated
	// forward. Aim must come from the same angles the model is drawn with,
	// or the gun lies about where it shoots. Deriving it from the shared
	// value rather than hardcoding a second 45 also means the two can never
	// drift apart when the mesh angle is retuned.
	VR_CalibrateWeaponAngles( ang );

	if( out_org ) VectorCopy( org, out_org );
	if( out_ang ) VectorCopy( ang, out_ang );
	return true;
}

// Weapon muzzle in world space, for both the laser and the arc origin.
static qboolean VR_GetMuzzle( vec3_t out_org, vec3_t out_fwd )
{
	vec3_t ang;

	if( !VR_GetWeaponAim( out_org, ang ))
		return false;

	AngleVectors( ang, out_fwd, NULL, NULL );
	return true;
}

/*
================
VR_BindOverlayTexture

TriBegin (ref/gl/gl_triapi.c:81) only calls glBegin - it sets NO texture
state at all. Whatever texture happened to be bound from the previous draw
is what gets modulated by our colour, so with additive blending and a dark
world texture the overlay came out invisible. Reported live as the laser
sight simply not being findable.

Binding the engine's built-in flat white gives the colour something to
multiply against, so the ribbons render as their actual colour.
================
*/
static void VR_BindOverlayTexture( void )
{
	static int white = 0;

	if( !white )
		white = ref.dllFuncs.GL_FindTexture( REF_WHITE_TEXTURE );

	if( white )
		ref.dllFuncs.GL_Bind( XASH_TEXTURE0, white );
}

// One camera-facing quad between two points. refState.vieworg is the
// engine-side mirror of the renderer's view origin - RI lives in ref_gl and
// is not visible from here.
static void VR_RibbonSegment( const vec3_t a, const vec3_t b, float halfwidth )
{
	vec3_t dir, to_eye, side, p;

	VectorSubtract( b, a, dir );
	if( VectorNormalizeLength( dir ) == 0.0f )
		return;

	VectorSubtract( a, refState.vieworg, to_eye );
	CrossProduct( dir, to_eye, side );
	if( VectorNormalizeLength( side ) == 0.0f )
		return;
	VectorScale( side, halfwidth, side );

	VectorAdd( a, side, p );      ref.dllFuncs.Vertex3fv( p );
	VectorSubtract( a, side, p ); ref.dllFuncs.Vertex3fv( p );
	VectorSubtract( b, side, p ); ref.dllFuncs.Vertex3fv( p );
	VectorAdd( b, side, p );      ref.dllFuncs.Vertex3fv( p );
}

// Small camera-facing quad centred on a point, for impact/landing markers.
static void VR_Marker( const vec3_t at, float radius )
{
	vec3_t right, up, p, vfwd;

	AngleVectors( refState.viewangles, vfwd, right, up );
	VectorScale( right, radius, right );
	VectorScale( up, radius, up );

	VectorAdd( at, right, p ); VectorAdd( p, up, p );        ref.dllFuncs.Vertex3fv( p );
	VectorSubtract( at, right, p ); VectorAdd( p, up, p );   ref.dllFuncs.Vertex3fv( p );
	VectorSubtract( at, right, p ); VectorSubtract( p, up, p ); ref.dllFuncs.Vertex3fv( p );
	VectorAdd( at, right, p ); VectorSubtract( p, up, p );   ref.dllFuncs.Vertex3fv( p );
}

static void VR_DrawLaserBeam( const vec3_t org, const vec3_t fwd )
{
	vec3_t end;
	pmtrace_t tr;
	float r, g, b;

	VectorMA( org, vr_laser_range.value, fwd, end );
	tr = CL_TraceLine( org, end, PM_STUDIO_BOX );
	VectorCopy( tr.endpos, end );

	r = vr_laser_r.value; g = vr_laser_g.value; b = vr_laser_b.value;

	ref.dllFuncs.GL_SetRenderMode( kRenderTransAdd );
	VR_BindOverlayTexture();

	if( vr_laser.value >= 2.0f )
	{
		ref.dllFuncs.Color4f( r, g, b, vr_laser_alpha.value );
		ref.dllFuncs.Begin( TRI_QUADS );
		VR_RibbonSegment( org, end, vr_laser_width.value );
		ref.dllFuncs.End();
	}

	// Impact dot always drawn, and drawn solid - it is the part that
	// actually tells you where the shot goes.
	ref.dllFuncs.Color4f( r, g, b, 1.0f );
	ref.dllFuncs.Begin( TRI_QUADS );
	VR_Marker( end, vr_laser_dot.value );
	ref.dllFuncs.End();
}

/*
================
VR_DrawVignette

Comfort vignette. Darkens the periphery while you move and opens back up when
you stop.

This is the standard mitigation for the mismatch that causes VR motion
sickness - your eyes report motion your inner ear does not - and it works by
cutting the peripheral optical flow the vestibular system reacts most strongly
to. Not cosmetic: this project has already produced one real bout of motion
sickness, so it is on by default.

Drawn as a ring of camera-facing quads a short distance in front of the eye,
so it sits in the world at a fixed apparent size in both eyes rather than
being a 2D overlay that would break stereo.
================
*/
static void VR_DrawVignette( void )
{
	static float amount = 0.0f;
	float fwd_move, side_move, speed, target;
	vec3_t fwd, right, up, center;
	int i;
	const int SEGMENTS = 32;
	const float DIST = 6.0f;	// units in front of the eye

	if( vr_vignette.value == 0.0f )
	{
		amount = 0.0f;
		return;
	}

	VR_GetMovement( &fwd_move, &side_move );
	speed = sqrt( fwd_move * fwd_move + side_move * side_move );

	// Normalised against the configured move speed, so it reads the same
	// whether the player runs fast or slow.
	target = ( vr_movespeed.value > 1.0f ) ? ( speed / vr_movespeed.value ) : 0.0f;
	if( target > 1.0f ) target = 1.0f;

	// Ease toward the target - snapping the vignette on and off is itself
	// uncomfortable, and worse than not having one.
	amount += ( target - amount ) * ( vr_vignette_fade.value * host.frametime );
	if( amount < 0.001f )
		return;

	AngleVectors( refState.viewangles, fwd, right, up );
	VectorMA( refState.vieworg, DIST, fwd, center );

	// Inner edge of the dark ring. At full speed it closes to
	// vr_vignette_size of the way out; at rest it is fully open.
	{
		float open = 1.0f - ( 1.0f - vr_vignette_size.value ) * amount;
		float inner = DIST * open * 1.2f;
		float outer = inner * 3.0f;

		ref.dllFuncs.GL_SetRenderMode( kRenderTransTexture );
		VR_BindOverlayTexture();
		ref.dllFuncs.Begin( TRI_QUADS );

		for( i = 0; i < SEGMENTS; i++ )
		{
			float a0 = ( i / (float)SEGMENTS ) * M_PI2;
			float a1 = ( ( i + 1 ) / (float)SEGMENTS ) * M_PI2;
			vec3_t p;
			int k;
			const float ca[2] = { cos( a0 ), cos( a1 ) };
			const float sa[2] = { sin( a0 ), sin( a1 ) };

			// Inner verts transparent, outer verts opaque, so the darkness
			// fades in rather than showing a hard black circle edge.
			for( k = 0; k < 2; k++ )
			{
				ref.dllFuncs.Color4f( 0.0f, 0.0f, 0.0f, 0.0f );
				VectorCopy( center, p );
				VectorMA( p, ca[k] * inner, right, p );
				VectorMA( p, sa[k] * inner, up, p );
				ref.dllFuncs.Vertex3fv( p );
			}
			for( k = 1; k >= 0; k-- )
			{
				ref.dllFuncs.Color4f( 0.0f, 0.0f, 0.0f, 1.0f );
				VectorCopy( center, p );
				VectorMA( p, ca[k] * outer, right, p );
				VectorMA( p, sa[k] * outer, up, p );
				ref.dllFuncs.Vertex3fv( p );
			}
		}

		ref.dllFuncs.End();
	}
}

static void VR_DrawLaser( void )
{
	vec3_t org, fwd;

	if( vr_laser.value <= 0.0f )
		return;

	if( VR_GetMuzzle( org, fwd ))
		VR_DrawLaserBeam( org, fwd );

	// The second gun gets its own sight. One laser across two guns tells you
	// nothing about where the off hand is pointed, which is the whole reason
	// the sight exists in VR.
	if( VR_GetOffhandFire( org, fwd ))
		VR_DrawLaserBeam( org, fwd );
}

/*
================
VR_DrawArc

Ballistic prediction for a thrown grenade. Steps the same simple integration
the server uses for a MOVETYPE_TOSS entity - constant gravity, straight
segments between samples - and traces each segment against the world, so the
arc terminates exactly where the grenade would first hit something.

Only drawn while actually holding a throwable, otherwise it is visual noise.
================
*/
static qboolean VR_HoldingThrowable( void )
{
	const char *name;

	if( !clgame.viewent.model || !clgame.viewent.model->name[0] )
		return false;

	name = clgame.viewent.model->name;

	// Stock Half-Life throwables. Matched by viewmodel name so no game-DLL
	// knowledge is needed; unknown mods simply will not match.
	// Model-derived first - a mod's custom throwable announces itself
	// through its own animation names, so nothing needs listing here.
	{
		const vr_wprofile_t *wp = VR_GetWeaponProfile();

		if( wp->valid && wp->throwable )
			return true;
	}

	return Q_stristr( name, "v_grenade" ) != NULL
	    || Q_stristr( name, "v_satchel" ) != NULL
	    || Q_stristr( name, "v_tripmine" ) != NULL
	    || Q_stristr( name, "v_squeak" ) != NULL;
}

static void VR_DrawArc( void )
{
	vec3_t org, fwd, pos, vel, next;
	int i, steps;
	float dt, grav;

	if( vr_arc.value <= 0.0f || !VR_HoldingThrowable() || !VR_GetMuzzle( org, fwd ))
		return;

	steps = bound( 4, (int)vr_arc_steps.value, 256 );
	dt    = bound( 0.005f, vr_arc_step_time.value, 0.5f );
	grav  = vr_arc_gravity.value;

	VectorCopy( org, pos );
	VectorScale( fwd, vr_arc_speed.value, vel );

	ref.dllFuncs.GL_SetRenderMode( kRenderTransAdd );
	VR_BindOverlayTexture();
	ref.dllFuncs.Color4f( vr_arc_r.value, vr_arc_g.value, vr_arc_b.value, vr_arc_alpha.value );
	ref.dllFuncs.Begin( TRI_QUADS );

	for( i = 0; i < steps; i++ )
	{
		pmtrace_t tr;

		vel[2] -= grav * dt;
		VectorMA( pos, dt, vel, next );

		tr = CL_TraceLine( pos, next, PM_STUDIO_IGNORE );

		VR_RibbonSegment( pos, tr.endpos, vr_arc_width.value );

		if( tr.fraction < 1.0f )
		{
			VectorCopy( tr.endpos, pos );
			break;
		}

		VectorCopy( next, pos );
	}

	ref.dllFuncs.End();

	// Landing marker, brighter than the ribbon so the endpoint is obvious.
	ref.dllFuncs.Color4f( vr_arc_r.value, vr_arc_g.value, vr_arc_b.value, 1.0f );
	ref.dllFuncs.Begin( TRI_QUADS );
	VR_Marker( pos, 2.0f );
	ref.dllFuncs.End();
}

/*
=================================================================
	Teleport locomotion

Smooth stick movement is the only option this fork has offered, and for a
sizeable fraction of players continuous artificial locomotion is the thing that
ends the session. Teleport is the standard alternative, and both reference
ports are worth noting here: HLVR has a full parabolic teleporter, Lambda1VR
has none at all.

Built as a MODE rather than an extra binding. With vr_teleport on, the movement
stick stops walking and starts aiming: push it to raise an arc from the off
hand, release to commit. That costs no new OpenXR action and therefore no
changes to any of the four interaction-profile binding tables, and it matches
what most VR titles do - the stick you would have walked with is the stick you
teleport with.

The arc itself is the grenade predictor (VR_DrawArc) pointed somewhere else:
same constant-gravity integration, same per-segment CL_TraceLine, same ribbon
and marker helpers. Only the source, the validity test and the commit are new.

Validation is deliberately done TWICE and differently. The client's test decides
what colour the marker is; the server re-tests with the real player hull before
moving anything, because the client's line traces cannot know whether a body
actually fits. That split is the normal authority split, not belt-and-braces:
the indicator only has to be honest, while the move has to be safe.
=================================================================
*/
static qboolean vr_tp_aiming = false;
static qboolean vr_tp_valid = false;
static vec3_t   vr_tp_dest;
static qboolean vr_tp_pending = false;	// committed, waiting for the server
static vec3_t   vr_tp_pending_dest;

qboolean VR_TeleportAiming( void )
{
	return vr_tp_aiming;
}

/*
================
VR_TraceTeleportArc

Integrate the arc from the off hand and report where it lands. Returns true
when it terminated on something the player could plausibly stand on.

`draw` also renders it, so the predictor and the indicator can never disagree
about where the arc goes - the same mistake the fire ray already had to be
restructured to avoid.
================
*/
static qboolean VR_TraceTeleportArc( vec3_t out_dest, qboolean draw )
{
	vec3_t org, ang, fwd, pos, vel, next;
	int i, steps;
	float dt, grav;
	qboolean landed = false, standable = false;

	if( !VR_GetHandWorld( VR_OffHand(), org, ang ))
		return false;

	VR_ApplyMeshCalibration( ang, (vec3_t){ vr_hand_pitch_offset.value,
		vr_hand_yaw_offset.value, vr_hand_roll_offset.value } );
	AngleVectors( ang, fwd, NULL, NULL );

	steps = bound( 4, (int)vr_teleport_steps.value, 256 );
	dt    = bound( 0.005f, vr_teleport_step_time.value, 0.5f );
	grav  = vr_teleport_gravity.value;

	VectorCopy( org, pos );
	VectorScale( fwd, vr_teleport_speed.value, vel );

	for( i = 0; i < steps; i++ )
	{
		pmtrace_t tr;

		vel[2] -= grav * dt;
		VectorMA( pos, dt, vel, next );

		tr = CL_TraceLine( pos, next, PM_STUDIO_IGNORE );

		if( draw )
			VR_RibbonSegment( pos, tr.endpos, vr_teleport_width.value );

		if( tr.fraction < 1.0f )
		{
			VectorCopy( tr.endpos, pos );
			landed = true;

			// Floor, not a wall or a ceiling. GoldSrc players can climb a
			// slope up to about 45 degrees, so accept anything at least that
			// upright and reject the rest - teleporting onto a wall face
			// leaves the body embedded in it.
			standable = ( tr.plane.normal[2] >= 0.7f );
			break;
		}

		VectorCopy( next, pos );
	}

	VectorCopy( pos, out_dest );

	if( !landed || !standable )
		return false;

	// Headroom. A line trace is not a hull test, so this only rejects the
	// obvious cases (landing under a vent, a crawlspace, a closed lift); the
	// server's hull test is what actually decides. Lifted slightly off the
	// surface first, or the trace starts inside the floor it just hit.
	{
		vec3_t from, to;
		pmtrace_t tr;

		VectorCopy( pos, from );
		from[2] += 1.0f;
		VectorCopy( from, to );
		to[2] += 72.0f;			// standing player height in HL units

		tr = CL_TraceLine( from, to, PM_STUDIO_IGNORE );
		if( tr.fraction < 1.0f )
			return false;
	}

	return true;
}

/*
================
VR_UpdateTeleport

Called once per client frame from CL_CreateCmd, before movement is built.
================
*/
void VR_UpdateTeleport( void )
{
	float mag;
	qboolean want;

	if( !VR_IsActive() || vr_teleport.value == 0.0f )
	{
		vr_tp_aiming = false;
		vr_tp_valid = false;
		return;
	}

	mag = sqrt( vr.move_x * vr.move_x + vr.move_y * vr.move_y );
	want = ( mag >= Q_max( 0.0f, vr_deadzone.value )) ? true : false;

	if( want )
	{
		vr_tp_valid = VR_TraceTeleportArc( vr_tp_dest, false );
		vr_tp_aiming = true;
		return;
	}

	// Stick released - commit if the last aimed point was good.
	if( vr_tp_aiming )
	{
		if( vr_tp_valid )
		{
			VectorCopy( vr_tp_dest, vr_tp_pending_dest );
			vr_tp_pending = true;

			// Confirm the commit in the hand that aimed it. Without this the
			// only feedback is the world changing, which is exactly the moment
			// the player is least able to tell whether their input registered.
			VR_Haptic( VR_OffHand(), 0.05f, 0.0f, 0.6f );
		}

		vr_tp_aiming = false;
		vr_tp_valid = false;
	}
}

/*
================
VR_ConsumeTeleport

Server side, from SV_RunCmd. Returns true exactly once per committed teleport
and hands back the destination.

Read directly out of client state rather than sent as a command, which is the
same thing sv_pmove.c already does for VR_GetWeaponAim: every VR substitution
is gated on NET_IsLocalAddress, so by construction this only ever runs for a
player sharing the process with the server. Nothing goes on the wire, so a
vanilla client connecting to a VR host is unaffected.
================
*/
qboolean VR_ConsumeTeleport( vec3_t out_dest )
{
	if( !vr_tp_pending )
		return false;

	VectorCopy( vr_tp_pending_dest, out_dest );
	vr_tp_pending = false;
	return true;
}

static void VR_DrawTeleportArc( void )
{
	vec3_t dest;
	qboolean ok;

	if( !vr_tp_aiming )
		return;

	ref.dllFuncs.GL_SetRenderMode( kRenderTransAdd );
	VR_BindOverlayTexture();

	// Colour carries the validity, so the player never has to guess whether a
	// release will do anything.
	ok = vr_tp_valid;
	ref.dllFuncs.Color4f( ok ? 0.2f : 1.0f, ok ? 1.0f : 0.2f, 0.2f,
		vr_teleport_alpha.value );

	ref.dllFuncs.Begin( TRI_QUADS );
	VR_TraceTeleportArc( dest, true );
	ref.dllFuncs.End();

	ref.dllFuncs.Color4f( ok ? 0.2f : 1.0f, ok ? 1.0f : 0.2f, 0.2f, 1.0f );
	ref.dllFuncs.Begin( TRI_QUADS );
	VR_Marker( dest, ok ? 6.0f : 3.0f );
	ref.dllFuncs.End();
}

/*
================
VR_DrawOverlays

Called from pfnDrawNormalTriangles (ref_common.c) each eye, inside the 3D
pass. Everything drawn here is additive and depth-tested against the world.
================
*/
void VR_DrawOverlays( void )
{
	if( vr_diag_aim.value > 0.0f )
	{
		vec3_t m, a;
		qboolean got = VR_GetWeaponAim( m, a );

		Cvar_DirectSet( &vr_diag_aim, va( "%d", (int)vr_diag_aim.value - 1 ));
		VR_DiagPrintf( "OVERLAY called: active=%d viewmodel=%d aim=%d "
			"muzzle=(%.1f %.1f %.1f) ang=(p%.1f y%.1f r%.1f) laser=%.0f melee=%d\n",
			VR_IsActive() ? 1 : 0, cl.local.viewmodel, got ? 1 : 0,
			m[0], m[1], m[2], a[PITCH], a[YAW], a[ROLL],
			vr_laser.value, VR_HoldingMelee() ? 1 : 0 );
	}

	if( !VR_IsActive() )
		return;

	// BEFORE the viewmodel check below: comfort does not depend on holding a
	// weapon, and being unarmed is exactly when you tend to be running around.
	VR_DrawVignette();

	// Nothing to aim with when no weapon is up.
	if( cl.local.viewmodel == 0 )
		return;

	VR_DrawArc();
	VR_DrawTeleportArc();
	VR_DrawLaser();
}

/*
================
VR_Begin2D / VR_End2D

The engine's 2D pass builds an ortho sized to the WINDOW and sets the viewport
to the whole window. Rendering that straight into a 2496x2688 eye texture would
stretch the HUD across the entire field of view at the wrong aspect - unreadable
and, at the periphery, invisible. Instead we keep the ortho and remap it into a
centred rect that preserves the window aspect, which is also just better VR
practice: HUD content belongs near the centre where the eye can actually resolve
it.
================
*/
/*
=================================================================
	VR menu: world anchor and controller pointer

Two problems, both reported from a headset:

The menu is painted straight into the eye texture at a fixed viewport rect, so
it is welded to your face. Turning your head takes the menu with you, which is
exactly what a menu must not do - there is nothing to look AT, so there is
nothing to aim at either.

And there was no pointer at all. Navigation meant reaching for a mouse you
cannot see while wearing a headset.

Anchoring is done by offsetting the 2D rect against head rotation rather than by
rendering the layer to a texture and hanging it on a world-space quad. The quad
is the "proper" answer and it is a much larger change - render-to-texture plus a
textured world draw - for a panel that only needs to hold still while it is
looked at. Offsetting is a small-angle approximation of the same thing and is
exact enough over the range a menu is actually read at.

It comes with a leash: past vr_menu_leash degrees the anchor is dragged along
rather than allowed to slide out of view, because a menu that can be lost
entirely by turning your head is worse than one that follows.

The pointer maps the dominant controller's aim direction onto that same rect and
feeds the result to the ordinary menu mouse entry points, so the menu itself
needs no VR knowledge - it thinks a mouse is moving.
=================================================================
*/
static qboolean vr_menu_active = false;	// menu was visible last frame
static float    vr_menu_yaw, vr_menu_pitch;	// where it was anchored
static int      vr_menu_ofs_x, vr_menu_ofs_y;	// consumed by VR_Begin2D
static qboolean vr_menu_click_prev = false;

/*
================
VR_Get2DRect

Where the flat 2D layer lands inside one eye texture.

Factored out because the menu POINTER has to agree with it exactly: the cursor
is derived by asking which part of this rect the controller is pointing at, so
if the two computed the rect differently the cursor would sit somewhere other
than where the menu was drawn. Same reasoning as the cached fire ray.

Does not include the per-eye parallax shift or the menu anchor offset - those
are applied by VR_Begin2D, and the pointer wants the rect's neutral position.
================
*/
static void VR_Get2DRect( int *out_x, int *out_y, int *out_w, int *out_h )
{
	const vr_swapchain_t *sc = &vr.swapchains[0];
	float scale = bound( 0.2f, vr_hud_scale.value, 1.0f );
	int ew, eh, w, h;
	float aspect;

	ew = (int)sc->width;
	eh = (int)sc->height;

	aspect = ( refState.height > 0 )
		? (float)refState.width / (float)refState.height : ( 16.0f / 9.0f );

	w = (int)( ew * scale );
	h = (int)( w / aspect );
	if( h > eh * scale )
	{
		h = (int)( eh * scale );
		w = (int)( h * aspect );
	}

	if( out_x ) *out_x = ( ew - w ) / 2;
	if( out_y ) *out_y = ( eh - h ) / 2;
	if( out_w ) *out_w = w;
	if( out_h ) *out_h = h;
}

void VR_Begin2D( void )
{
	const vr_swapchain_t *sc = &vr.swapchains[0];
	float scale = bound( 0.2f, vr_hud_scale.value, 1.0f );
	int ew, eh, w, h, x, y;
	float aspect;

	if( !VR_IsActive() || !vrgl.loaded || !vrgl.Viewport )
		return;

	ew = (int)sc->width;
	eh = (int)sc->height;

	// match the aspect ref_gl built its 2D ortho from (the window)
	aspect = ( refState.height > 0 )
		? (float)refState.width / (float)refState.height : ( 16.0f / 9.0f );

	// fit a window-aspect rect inside the eye, then shrink by vr_hud_scale
	w = (int)( ew * scale );
	h = (int)( w / aspect );
	if( h > eh * scale )
	{
		h = (int)( eh * scale );
		w = (int)( h * aspect );
	}

	x = ( ew - w ) / 2;
	y = ( eh - h ) / 2;

	// STEREO DEPTH. Shift the rect horizontally, in opposite directions per eye.
	//
	// Compositing the identical rect into both eyes gives zero disparity, and
	// zero disparity is what "infinitely far away" looks like - while the world
	// behind it sits a couple of metres off. The eyes cannot converge on both
	// at once, so the HUD splits into two overlapping copies and the player
	// fights it for the whole session. It reads as eye strain rather than as a
	// bug, which is why it survives being looked at.
	//
	// Shifting the LEFT eye's copy right and the RIGHT eye's left is crossed
	// disparity, which pulls the HUD in front of infinity to roughly
	//     D ~= IPD / ( parallax * horizontal FOV )
	// - about two metres at the default, a comfortable reading distance and
	// close to where the weapon already is, so the eyes barely re-converge
	// when glancing between them. vr_hud_parallax 0 restores the old
	// at-infinity behaviour for anyone who prefers it.
	{
		float p = bound( 0.0f, vr_hud_parallax.value, 0.1f );
		int shift = (int)(( ew * p ) * 0.5f );

		if( vr.cur_eye == 0 )      x += shift;
		else if( vr.cur_eye == 1 ) x -= shift;
	}

	// MENU ANCHOR. While the menu is open the panel is pinned in the world
	// rather than to the face - see VR_UpdateMenu2D for why that is worth
	// doing. Zero whenever the menu is closed, so the HUD is unaffected.
	x += vr_menu_ofs_x;
	y += vr_menu_ofs_y;

	vrgl.Viewport( x, y, w, h );
}


/*
================
VR_UpdateDeath

Dying in VR is a dead end without a keyboard.

Half-Life singleplayer has no respawn - death drops you at the load menu, and
reaching that in a headset means taking it off. So the trigger reloads the
last quicksave, and one is kept current automatically so there is always
something to load.

Autosave is deliberately not tied to anything clever: a fixed interval, only
while alive, only in single player - the engine refuses to save at all when
maxclients != 1, so this can never fire during co-op or crossplay.
================
*/
static void VR_UpdateDeath( void )
{
	static qboolean fire_prev = false;
	static double next_save = 0.0;
	qboolean fire, dead;

	if( !VR_IsActive() || cls.state != ca_active )
		return;

	dead = ( cl.frames[cl.parsecountmod].clientdata.deadflag != 0 );
	// Either trigger, and the grip too. Dead is no time to be precise about
	// which button, and none of them mean anything else while dead.
	fire = ( VR_GetButton( VR_BTN_ATTACK ) || VR_GetButton( VR_BTN_ATTACK2 )
		|| VR_GetButton( VR_BTN_USE )) ? true : false;

	if( dead )
	{
		if( vr_deathload.value != 0.0f && fire && !fire_prev )
			Cbuf_AddText( "load quick\n" );

		// Do not let the timer fire while dead - it would overwrite the
		// save being reloaded with the corpse.
		next_save = host.realtime + Q_max( 5.0f, vr_autosave.value );
	}
	else if( vr_autosave.value > 0.0f && cl.maxclients == 1 )
	{
		if( next_save == 0.0 )
			next_save = host.realtime + vr_autosave.value;
		else if( host.realtime >= next_save )
		{
			Cbuf_AddText( "save quick\n" );
			next_save = host.realtime + vr_autosave.value;
		}
	}

	fire_prev = fire;
}

/*
================
VR_HandInHeadSpot

Is `hand` inside a hotspot hung off the head, `side` units out along the
head's right vector?

Head-relative rather than world-fixed so the spot travels with the player and
stays reachable whichever way they face. Offset BEHIND the head as well: in
front of the face is where a weapon is actually held, and a hotspot there
would fire constantly.

Shared by both shoulder gestures so they cannot drift apart - the same reason
the fire ray is computed once and cached.
================
*/
/*
================
VR_BodySpotDist

How far `hand` is from a hotspot hung off the head, or -1 if it cannot be
worked out. The distance rather than a yes/no, because when a gesture will
not fire the only useful question is whether the hand was nearly there or
nowhere near, and a boolean cannot answer it.
================
*/
static float VR_BodySpotDist( int hand_id, float side, float back, float lift )
{
	vec3_t hand, hang, head, hang_w, fwd, right, up, spot, d;

	if( !VR_GetHandWorld( hand_id, hand, hang ))
		return -1.0f;

	// The REAL tracked head, in world space - not a reconstruction.
	//
	// This was built as world_origin + vr_height with vr.hmd_pose.angles, and
	// both halves were in the wrong frame. world_origin is the BODY, while the
	// hands come through VR_PlayToWorld and carry the room-scale lean, which
	// runs 3-7 units in ordinary play - so the spot and the hands were never
	// measured from the same place. And hmd_pose.angles is PLAY space, missing
	// body_yaw, so "beside your head" pointed somewhere else entirely unless
	// the player happened to be facing world north.
	//
	// VR_GetListener already returns the head through the same transform the
	// hands use, so both are in one frame by construction.
	if( !VR_GetListener( head, hang_w ))
		return -1.0f;

	AngleVectors( hang_w, fwd, right, up );

	VectorCopy( head, spot );
	VectorMA( spot, side, right, spot );
	VectorMA( spot, -back, fwd, spot );
	spot[2] += lift;

	VectorSubtract( hand, spot, d );
	return VectorLength( d );
}

static qboolean VR_HandInBodySpot( int hand_id, float side, float back,
	float lift, float radius )
{
	float dist = VR_BodySpotDist( hand_id, side, back, lift );

	return ( dist >= 0.0f && dist < Q_max( 1.0f, radius )) ? true : false;
}

// The shoulder gestures, in the terms they were written in.
static qboolean VR_HandInHeadSpot( int hand_id, float side )
{
	return VR_HandInBodySpot( hand_id, side, vr_shoulder_back.value,
		vr_shoulder_up.value, vr_shoulder_radius.value );
}

/*
================
VR_UpdateAction

Working the action: pump, bolt or lever.

A pump gun that fires as fast as the trigger is pulled is the single most
un-VR thing about holding one. The mod cycles it for you as an animation,
because on a keyboard there is no hand free to do it - in a headset there is,
and it is most of what makes the weapon feel like itself.

So the shot is withheld until the action has been worked. Take hold of the
weapon with the off hand and pull back.

Two things keep this general. Whether a weapon HAS an action to work is read
from its sequence labels, because a pump must be animated to exist. And a
shot is noticed by the clip going down, which the mod reports in every
CurWeapon message - so this never has to agree with our own button
bookkeeping about whether a trigger pull actually became a shot.

Measured as displacement ALONG the weapon rather than a position in the
world, so it reads the same however the gun is held, and cannot be satisfied
by the weapon moving while the hand stays still.
================
*/
static void VR_UpdateAction( void )
{
	static qboolean grip_prev = false;
	const vr_wprofile_t *wp;
	vec3_t hand, hang, wpn, wang, fwd, d;
	qboolean grip;
	float proj;

	if( !VR_IsActive() || vr_pump.value == 0.0f )
	{
		vr.act_needs = false;
		return;
	}

	vr.act_worked = false;

	wp = VR_GetWeaponProfile();

	// What the weapon was CLASSIFIED as, once per change. Every exit below is
	// silent, so a weapon that never gates its fire looks identical to one
	// whose gesture is failing - and they are opposite problems.
	if( vr_diag.value != 0.0f )
	{
		static const model_t *last = NULL;

		if( clgame.viewent.model != last )
		{
			last = clgame.viewent.model;
			VR_DiagPrintf( "PROFILE %s valid=%d pump=%d slide=%d clip=%d\n",
				last ? last->name : "(none)", wp ? wp->valid : 0,
				( wp && wp->pump ) ? 1 : 0, ( wp && wp->slide ) ? 1 : 0, vr.rl_clip );
		}
	}

	// Switching weapons resets everything: a clip count from the last gun
	// says nothing about this one, and comparing them would read as a shot.
	if( vr_wlist.cur_id != vr.act_id )
	{
		vr.act_id = vr_wlist.cur_id;
		vr.act_clip = vr.rl_clip;
		vr.act_needs = false;
		vr.act_armed = false;
		return;
	}

	if( !wp || !wp->valid || ( !wp->pump && !wp->slide ))
	{
		vr.act_needs = false;
		vr.act_clip = vr.rl_clip;
		return;
	}

	// WHAT LEAVES THE ACTION SPENT depends on the weapon.
	//
	// A pump gun needs working after every shot, so a round leaving the tube
	// is the trigger. A weapon whose slide locks back needs it only after
	// loading from empty, so the trigger is the clip coming back up from
	// nothing - which is also exactly when the mod plays its empty-reload
	// animation, so the two agree without being told about each other.
	//
	// Both read the clip rather than our own record of sending IN_ATTACK,
	// because a trigger pull is not a shot: it can land during the refire
	// delay, on an empty chamber, or while the mod has the weapon busy.
	if( wp->pump && vr.rl_clip < vr.act_clip )
		vr.act_needs = true;

	if( wp->slide && vr.act_clip == 0 && vr.rl_clip > 0 )
		vr.act_needs = true;

	vr.act_clip = vr.rl_clip;

	// A GRATUITOUS PUMP still counts, when it is allowed to.
	//
	// Working a loaded action throws the chambered round on the floor, and
	// a player who does that to a real pump gun gets the same result. The
	// mod decides what it costs; this only reports that it happened.
	//
	// Off by default, and not timidity. The pull is two and a half units,
	// which a bracing hand covers just by walking, so leaving it on quietly
	// empties the weapon of somebody who never asked for it. Losing rounds
	// you did not choose to lose is a worse failure than not having this.
	if( !vr.act_needs && vr_pump_ejects.value == 0.0f )
	{
		vr.act_armed = false;
		grip_prev = VR_GetButton( VR_BTN_OFFGRIP ) ? true : false;
		return;
	}

	if( !VR_GetHandWorld( VR_OffHand(), hand, hang )
		|| !VR_GetWeaponAim( wpn, wang ))
		return;

	grip = VR_GetButton( VR_BTN_OFFGRIP ) ? true : false;

	AngleVectors( wang, fwd, NULL, NULL );
	VectorSubtract( hand, wpn, d );
	proj = DotProduct( d, fwd );

	// TAKE HOLD, not take hold ANEW.
	//
	// This wanted a fresh grip press, which a shotgun is never going to
	// get: you brace it two-handed to aim, so the off hand is ALREADY
	// closed when the shot goes off. The rising edge could then never
	// arrive, the action could never be worked, and the weapon stopped
	// firing for good - sixteen hundred frames of needing a pump with a
	// hand sat on it the whole time.
	//
	// Arming from the hand simply BEING there measures travel from
	// wherever it rests, so a brace that never moves does nothing and a
	// deliberate pull works the action.
	if( grip && !vr.act_armed
		// Its OWN reach, not the loading port doubled. The fore-end you pump
		// sits well forward of the gate you feed shells into, and borrowing
		// that number put the arming radius at 26 units while a braced hand
		// rests around 30 - so the action could not be worked from the very
		// position the weapon is held in.
		&& VectorLength( d ) < Q_max( 1.0f, vr_pump_reach.value ))
	{
		// Took hold of the fore-end.
		vr.act_ref = proj;
		vr.act_armed = true;
		VR_Haptic( VR_OffHand(), 0.03f, 0.0f, 0.4f );
	}
	else if( !grip )
	{
		vr.act_armed = false;
		vr.act_pull = 0.0f;
		vr.act_back = false;
	}
	else if( vr.act_armed )
	{
		// OUT AND BACK, both of them the hand's doing.
		//
		// A pump is a stroke, not a distance reached. Pulling back opens the
		// action and pushing forward closes it, and the round is only chambered
		// when it has been closed again - so the cycle completes on the RETURN,
		// not the moment the hand gets far enough away.
		//
		// Nothing plays on its own at any point. The animation is wherever the
		// hand has put it, forwards or backwards, for as long as the hand is on
		// the weapon.
		float travel = Q_max( 0.1f, vr_pump_travel.value );
		float pull = ( vr.act_ref - proj ) / travel;

		if( pull < 0.0f ) pull = 0.0f;
		if( pull > 1.0f ) pull = 1.0f;

		vr.act_pull = pull;

		if( pull >= 1.0f )
			vr.act_back = true;

		// Closed again. A little short of all the way, because a hand does not
		// return to the exact unit it started from and demanding that leaves the
		// action hanging open with nothing the player can do about it.
		if( vr.act_back && pull <= 0.15f )
		{
			vr.act_needs = false;
			vr.act_back = false;
			vr.act_pull = 0.0f;
			vr.act_worked = true;
			vr.act_armed = false;

			if( vr_action_sound.string[0] )
				S_StartLocalSound( vr_action_sound.string, VOL_NORM, false );

			VR_Haptic( VR_OffHand(), 0.08f, 0.0f, 1.0f );
			VR_Haptic( VR_DominantHand(), 0.08f, 0.0f, 0.9f );
		}
		else if( pull >= 1.0f && !vr.act_back )
		{
			// Reaching the stop is felt, so the player knows to push forward.
			VR_Haptic( VR_OffHand(), 0.04f, 0.0f, 0.6f );
		}
	}

	if( vr_diag.value != 0.0f )
		VR_DiagPrintf( "ACTION needs=%d armed=%d grip=%d travel=%.1f/%.0f pull=%.2f back=%d hand=%.1f clip=%d\n",
			vr.act_needs ? 1 : 0, vr.act_armed ? 1 : 0, grip ? 1 : 0,
			vr.act_armed ? ( vr.act_ref - proj ) : 0.0f,
			vr_pump_travel.value, vr.act_pull, vr.act_back ? 1 : 0,
			VectorLength( d ), vr.rl_clip );

	grip_prev = grip;
}

/*
================
VR_GetActionImpulse

The impulse that tells the mod its action was worked by hand, or 0.

An impulse because it is an EVENT, and userinfo carries state. The mod plays
the pump animation itself on receiving it, which is the only way the weapon
visibly cycles when the player cycles it rather than on the mod's own
schedule - a sound from the engine cannot move the model.

An unhandled impulse falls through harmlessly, so mods that know nothing
about this are unaffected. 210 is clear of everything Half-Life uses.
================
*/
int VR_GetActionImpulse( void )
{
	return ( VR_IsActive() && vr.act_worked ) ? 210 : 0;
}

/*
================
VR_HoldViewModel

Stop the weapon mid-animation while it is waiting to be worked, and carry on
from the same frame once it has been.

A pump gun animates its own pump as part of firing, so the gun cycled on the
mod's schedule no matter what the player did - shoot, pump, shoot - with the
player's pull an unrelated gesture happening alongside a weapon that had
already cycled itself. Playing a pump sequence on the gesture only made it
twice.

The animation position is elapsed time - the renderer takes the frame from
cl.time minus weaponstarttime - so holding that difference constant freezes
the picture wherever it had got to, and releasing it carries on rather than
jumping to where it would have been. The recoil plays, the gun stops with the
action unworked, and the pump happens when the player pumps.

Costs nothing on weapons that never ask for the action, and works without the
mod knowing: it is the engine's own viewmodel clock.
================
*/
void VR_HoldViewModel( void )
{
	static double held = -1.0;
	double elapsed;

	if( !VR_IsActive() || !vr.act_needs )
	{
		held = -1.0;
		return;
	}

	elapsed = cl.time - cl.local.weaponstarttime;

	// Let the shot itself play first. Freezing the instant the round leaves
	// catches the weapon mid-flash, which reads as a hitch rather than a gun
	// waiting to be worked.
	if( held < 0.0 )
	{
		// THE SAME FRAME EVERY TIME.
		//
		// This used to hold wherever the animation had reached when the
		// shot was noticed - and it is noticed when the clip drop arrives
		// in a CurWeapon message, whose latency varies. So every shot
		// stopped the weapon somewhere slightly different and the timing
		// came out right only by luck.
		//
		// Naming the frame outright makes it identical shot to shot, and
		// makes vr_pump_recoil mean what it says: where in the firing
		// animation the weapon waits.
		if( elapsed < (double)Q_max( 0.0f, vr_pump_recoil.value ))
			return;

		held = (double)Q_max( 0.0f, vr_pump_recoil.value );
	}

	// SCRUBBED BY THE HAND, not merely stopped.
	//
	// Holding the elapsed time still freezes the weapon; offsetting it by
	// how far the action is pulled runs the same animation forwards and
	// backwards under the hand. The fore-end goes back when the hand goes
	// back and returns when it does, so the player is manipulating the
	// mechanism rather than triggering it.
	//
	// It lands on the same frames the sequence would have played anyway -
	// nothing is invented, it is only being read at the hand's pace
	// instead of the clock's.
	// THE FRAME IS THE HAND'S POSITION, the whole way through.
	//
	// Out along the first half of the stroke and back along the second, so
	// the fore-end goes back as the hand goes back and returns as it returns.
	// Nothing here plays: the animation is only ever read at wherever the
	// hand has put it, which is the difference between working a mechanism
	// and setting one off.
	{
		float progress = vr.act_back
			? ( 0.5f + ( 1.0f - vr.act_pull ) * 0.5f )
			: ( vr.act_pull * 0.5f );

		cl.local.weaponstarttime = cl.time
			- ( held + (double)( progress * Q_max( 0.0f, vr_pump_scrub.value )));
	}
}

/*
================
VR_ActionBlocked

True while the weapon is waiting for its action to be worked. The trigger is
held off rather than the shot being swallowed later, so an empty click never
happens and the mod never sees a pull it would have to explain.
================
*/
qboolean VR_ActionBlocked( void )
{
	return ( VR_IsActive() && vr.act_needs ) ? true : false;
}

/*
================
VR_UpdateReload

Load the gun by hand: reach to the pouch at your off-side hip, close your
hand on a round, bring it to the weapon and let go.

One round at a time, which is deliberately the same two moves for every
weapon. A magazine finishes in one trip and a shell tube takes as many as it
takes, but that difference belongs entirely to the mod: each completed insert
raises IN_RELOAD exactly once, and the mod's own Reload() decides whether that
fills the gun or adds a single shell. So this never has to know which kind of
weapon it is holding, and no weapon needs listing anywhere.

That is also why the reload stays entirely the mod's: its animation, its
timing, its ammo accounting, unmodified. All that changes is WHEN the button
is pressed - which the VR layer already owned.

Client-local, so a VR player simply reloads later than a flatscreen one and
nothing about it reaches the protocol.
================
*/
static void VR_UpdateReload( void )
{
	// Tell the server what this player is doing, when it changes.
	//
	// Only on a change, because setting a userinfo cvar sends an update to
	// the server - every frame would be a steady trickle of them for a fact
	// that alters about once a session.
	{
		qboolean want = ( VR_IsActive() && vr_reload.value != 0.0f );

		if( want != ( vr_handload.value != 0.0f ))
			Cvar_SetValue( "vr_handload", want ? 1.0f : 0.0f );
	}

	static qboolean grip_prev = false;
	vec3_t hand, hang, wpn, wang, fwd, port, d;
	qboolean grip;
	float side;

	vr.rl_insert = false;

	// The off-hand grip means something else in all of these: a rung to hold,
	// a menu to steer, and nothing at all with empty hands.
	if( !VR_IsActive() || vr_reload.value == 0.0f
		|| VR_LadderHands() || VR_SelectOpen() || cl.local.viewmodel == 0 )
	{
		vr.rl_holding = false;
		grip_prev = false;
		return;
	}

	if( !VR_GetHandWorld( VR_OffHand(), hand, hang ))
		return;

	grip = VR_GetButton( VR_BTN_OFFGRIP ) ? true : false;
	side = ( VR_OffHand() == 0 ) ? -vr_reload_side.value : vr_reload_side.value;

	if( !vr.rl_holding )
	{
		// Closing the hand ON the pouch, not merely having it closed nearby -
		// an edge, or walking past with a fist would keep loading rounds.
		if( grip && !grip_prev
			&& VR_HandInBodySpot( VR_OffHand(), side, vr_reload_back.value,
				-vr_reload_down.value, vr_reload_radius.value ))
		{
			vr.rl_holding = true;
			VR_Haptic( VR_OffHand(), 0.05f, 0.0f, 0.7f );
		}
	}
	else if( !grip )
	{
		qboolean at_port = false;

		// The port sits a little ahead of where the gun is held, which is
		// close enough for a breech, a magwell or a loading gate without
		// needing to know which one this weapon has.
		if( VR_GetWeaponAim( wpn, wang ))
		{
			AngleVectors( wang, fwd, NULL, NULL );
			VectorMA( wpn, vr_reload_port_fwd.value, fwd, port );
			VectorSubtract( hand, port, d );
			at_port = ( VectorLength( d ) < Q_max( 1.0f, vr_reload_port.value ));
		}

		if( at_port )
		{
			// Both hands, because a round going in is felt by the hand holding
			// the gun as much as the one loading it.
			vr.rl_insert = true;
			VR_Haptic( VR_OffHand(), 0.07f, 0.0f, 1.0f );
			VR_Haptic( VR_DominantHand(), 0.05f, 0.0f, 0.6f );
		}
		else
		{
			// Dropped it. A softer buzz, so a fumble is felt rather than
			// silently mistaken for a round that went in.
			VR_Haptic( VR_OffHand(), 0.03f, 0.0f, 0.3f );
		}

		vr.rl_holding = false;
	}

	// Every stage, every frame it is live. Which half of the gesture is
	// failing - reaching the pouch or reaching the gun - is invisible from
	// inside the headset and guessable from outside only by luck.
	if( vr_diag.value != 0.0f && ( grip || vr.rl_holding || vr.rl_insert ))
	{
		float pouch = VR_BodySpotDist( VR_OffHand(), side, vr_reload_back.value,
			-vr_reload_down.value );
		float pdist = -1.0f;

		if( VR_GetWeaponAim( wpn, wang ))
		{
			AngleVectors( wang, fwd, NULL, NULL );
			VectorMA( wpn, vr_reload_port_fwd.value, fwd, port );
			VectorSubtract( hand, port, d );
			pdist = VectorLength( d );
		}

		VR_DiagPrintf( "RELOAD grip=%d hold=%d insert=%d pouch=%.1f/%.0f port=%.1f/%.0f clip=%d\n",
			grip ? 1 : 0, vr.rl_holding ? 1 : 0, vr.rl_insert ? 1 : 0,
			pouch, vr_reload_radius.value, pdist, vr_reload_port.value, vr.rl_clip );
	}

	grip_prev = grip;
}

/*
================
VR_GetReloadCmd

Whether IN_RELOAD should be raised this frame.

One place decides, because there are three ways to arrive at a reload and
they must not fight: a completed physical insert, the ordinary button when
physical reloading is off, and holding that button down as a way out.

That last one matters more than it looks. If the gesture cannot be completed
on some weapon - a model whose grip sits somewhere unexpected, a hotspot that
does not fit the player - the alternative is somebody who cannot reload at
all, which is a far worse failure than a clumsy reload. A held button always
works, on every weapon, in every mod.
================
*/
qboolean VR_GetReloadCmd( void )
{
	static double held_since = 0.0;
	qboolean button;

	if( !VR_IsActive( ))
		return false;

	button = VR_GetButton( VR_BTN_RELOAD ) ? true : false;

	if( vr_reload.value == 0.0f )
	{
		held_since = 0.0;
		return button;
	}

	if( vr.rl_insert )
		return true;

	if( !button )
	{
		held_since = 0.0;
		return false;
	}

	if( held_since == 0.0 )
		held_since = host.realtime;

	return ( host.realtime - held_since >= (double)Q_max( 0.1f, vr_reload_hold.value ));
}

/*
================
VR_UpdateShoulderMelee

Reach beside your head to swap to the melee weapon, reach again to swap back.

Deliberately ONE hotspot rather than a set of body holsters. A full holster rig
means deciding where every weapon lives on a body wearing an HEV suit, which is
an environment suit with no webbing, no slings and nowhere to put a shotgun -
so every answer looks wrong. Grabbing a crowbar from beside your head to break a
crate is a single, obvious gesture that needs none of that settled.

Mod-agnostic on both halves. Melee is slot 1 in every GoldSrc mod, so getting
there is "slot1". Coming back cannot be, because the client DLLs here register
no lastinv command - checked, only slot1..slot10, cancelselect, invnext and
invprev exist - so the weapon is remembered by its m_iId, which the engine
already decodes from clientdata_t on any mod, and restored by stepping invnext
until it comes back round.

That stepping is spread over frames on purpose: m_iId only updates once the
server has acknowledged the switch, so a loop inside one frame would compare
against a stale value and run to its limit every time.
================
*/
static void VR_UpdateShoulderMelee( void )
{
	// NOT WHILE CLIMBING.
	//
	// Reaching up for a rung puts the hand beside the head, which is exactly
	// the shoulder-holster gesture - so climbing a ladder kept triggering the
	// crowbar swap. Worse, the weapon is stowed while climbing so the
	// viewmodel is 0, the restore could never match its target, and its phase
	// machine ran over and over issuing "hud_fastswitch 0; invnext" - which
	// left the weapon select permanently open on screen.
	if( VR_LadderHands( ))
	{
		vr.sh_inside = vr.sh_light_inside = false;
		vr.sh_restore_tries = 0;
		return;
	}

	qboolean inside;

	if( !VR_IsActive( ))
		return;

	// --- flashlight, off-hand side ---
	//
	// Mirrored deliberately: weapon on the dominant side, light on the off
	// side. Two gestures that cannot be confused with each other, and each
	// belongs to the hand that would naturally reach it.
	if( vr_shoulder_light.value != 0.0f )
	{
		float lside = ( VR_OffHand() == 0 ) ? -vr_shoulder_side.value : vr_shoulder_side.value;
		qboolean lin = VR_HandInHeadSpot( VR_OffHand(), lside );

		if( lin && !vr.sh_light_inside )
		{
			Cbuf_AddText( "impulse 100\n" );
			VR_Haptic( VR_OffHand(), 0.05f, 0.0f, 0.7f );
		}
		vr.sh_light_inside = lin;
	}

	// --- melee swap, dominant side ---
	if( vr_shoulder_melee.value == 0.0f )
		return;

	// Walk back toward the remembered weapon: highlight, then CONFIRM.
	//
	// invnext DOES NOT SWITCH WEAPONS. CHudAmmo::UserCmd_NextWeapon only moves
	// gpActiveSel, the selection highlight - the switch happens when a confirm
	// reaches ammo.cpp and it calls ServerCmd( name ). The earlier version fired
	// twelve invnexts and changed nothing; telemetry showed the viewmodel sitting
	// at one index through every step.
	//
	// slotN is the exception, and the reason DRAWING the crowbar always worked:
	// with fast-switch on it selects outright when the bucket holds one weapon.
	//
	// This used to fire an invnext every frame. m_iId only updates once the
	// server has acknowledged the switch, which takes several frames, so a
	// per-frame loop issued a dozen switches before the first one landed -
	// cycling clean past the target weapon and then giving up. Reported as
	// simply not swapping back.
	//
	// So step only when the previous step has visibly taken effect, with a
	// wall-clock timeout in case a switch is refused outright (no ammo, a
	// weapon dropped while swapped) and m_iId therefore never moves.
	if( vr.sh_restore_tries > 0 )
	{
		int now_id = cl.local.viewmodel;

		if( now_id == vr.sh_restore_id )
		{
			vr.sh_restore_tries = 0;
			vr.sh_phase = 0;
			vr.sh_swapped = false;
			Cbuf_AddText( "hud_fastswitch 1\n" );
			VR_Haptic( VR_DominantHand(), 0.04f, 0.0f, 0.5f );
			VR_DiagPrintf( "SHOULDER restored to %d\n", now_id );
		}
		else if( host.realtime >= vr.sh_step_time )
		{
			switch( vr.sh_phase )
			{
			case 0:
				Cbuf_AddText( "hud_fastswitch 0; invnext\n" );
				vr.sh_phase = 1;
				break;
			case 1:
				Cbuf_AddText( "+attack\n" );
				vr.sh_phase = 2;
				break;
			default:
				Cbuf_AddText( "-attack\n" );
				vr.sh_phase = 0;
				vr.sh_restore_tries--;
				VR_DiagPrintf( "SHOULDER step have=%d want=%d left=%d\n",
					now_id, vr.sh_restore_id, vr.sh_restore_tries );
				if( vr.sh_restore_tries == 0 )
				{
					vr.sh_swapped = false;
					Cbuf_AddText( "hud_fastswitch 1\n" );
					VR_DiagPrintf( "SHOULDER gave up, want %d have %d\n",
						vr.sh_restore_id, now_id );
				}
				break;
			}

			// One phase per 60ms: +attack must survive a frame to be sampled by
			// CL_ButtonBits, so press and release cannot share a command string.
			vr.sh_step_time = host.realtime + 0.06;
		}
	}

	{
		float mside = ( VR_DominantHand() == 0 ) ? -vr_shoulder_side.value : vr_shoulder_side.value;
		inside = VR_HandInHeadSpot( VR_DominantHand(), mside );
	}

	// Edge only: entering swaps, and the hand must leave before it can swap
	// again. Without that, resting a hand near the head toggles every frame.
	if( inside && !vr.sh_inside )
	{
		if( !vr.sh_swapped )
		{
			// A stowed weapon has no viewmodel, so there would be nothing to come
			// back to and the walk would never terminate.
			if( cl.local.viewmodel == 0 )
				return;

			vr.sh_restore_id = cl.local.viewmodel;
			Cbuf_AddText( "hud_fastswitch 1; slot1\n" );
			{
				const char *wn = VR_CurrentWeaponName();
				Q_strncpy( vr.sh_restore_name, wn ? wn : "", sizeof( vr.sh_restore_name ));
			}
			vr.sh_swapped = true;
			VR_DiagPrintf( "SHOULDER stored id %d, drawing melee\n", vr.sh_restore_id );
			vr.sh_restore_tries = 0;
		}
		else
		{
			// Bounded by what can be carried, so a weapon dropped while
			// swapped cannot spin this forever.
			// Straight there when the mod has told us the name - Half-Life's own
			// select confirms with ServerCmd( name ), and an unrecognised console
			// command is forwarded to the server, so this is one command and one
			// switch with no cycling through weapons the player did not ask for.
			if( vr.sh_restore_name[0] )
			{
				Cbuf_AddText( va( "%s\n", vr.sh_restore_name ));
				vr.sh_swapped = false;
				vr.sh_restore_tries = 0;
				// The same confirm sound Half-Life plays when you pick a weapon from
				// its own select. Switching by name bypasses that HUD path entirely, so
				// the swap was silent - correct, but it did not feel like it had
				// happened.
				S_StartLocalSound( "common/wpn_select.wav", VOL_NORM, false );
				VR_Haptic( VR_DominantHand(), 0.04f, 0.0f, 0.5f );
				VR_DiagPrintf( "SHOULDER jumped back to %s\n", vr.sh_restore_name );
			}
			else
			{
				// Fallback: no name yet, walk it the slow way.
				vr.sh_restore_tries = 12;
				vr.sh_seen_id = cl.local.viewmodel;
				vr.sh_phase = 0;
			}
			vr.sh_step_time = 0.0;	// step immediately on the next frame
			VR_DiagPrintf( "SHOULDER restoring, want %d\n", vr.sh_restore_id );
		}

		VR_Haptic( VR_DominantHand(), 0.05f, 0.0f, 0.7f );
	}

	vr.sh_inside = inside;
}

/*
================
VR_UpdateMenu2D

Once per frame from VR_BeginFrame, which is the one place that runs in BOTH the
world path and the menu-only path (V_RenderVRMenu) - the main menu has no
CL_CreateCmd to hang off. Input is already synced by the time this is called.
================
*/
static void VR_UpdateMenu2D( void )
{
	qboolean visible = UI_IsVisible() ? true : false;
	float fov_x, fov_y, ppd_x, ppd_y, dyaw, dpitch, leash;
	int rx, ry, rw, rh;

	vr_menu_ofs_x = vr_menu_ofs_y = 0;

	if( !visible || vr_menu_lock.value == 0.0f )
	{
		// Never leave a click held. The menu closing mid-press would otherwise
		// leave K_MOUSE1 down forever from its point of view.
		if( vr_menu_click_prev )
		{
			UI_KeyEvent( K_MOUSE1, false );
			vr_menu_click_prev = false;
		}

		vr_menu_active = false;
		return;
	}

	// Degrees of eye texture per degree of head rotation. Taken from the
	// runtime's own reported FOV rather than assumed, since it differs per
	// headset.
	fov_x = RAD2DEG( fabs( vr.views[0].fov.angleLeft ) + fabs( vr.views[0].fov.angleRight ));
	fov_y = RAD2DEG( fabs( vr.views[0].fov.angleUp ) + fabs( vr.views[0].fov.angleDown ));

	if( fov_x < 1.0f || fov_y < 1.0f )
		return;		// views not located yet

	VR_Get2DRect( &rx, &ry, &rw, &rh );

	ppd_x = (float)vr.swapchains[0].width / fov_x;
	ppd_y = (float)vr.swapchains[0].height / fov_y;

	// Anchor on the frame the menu appears, at wherever the player was looking.
	if( !vr_menu_active )
	{
		vr_menu_yaw   = vr.hmd_pose.angles[YAW];
		vr_menu_pitch = vr.hmd_pose.angles[PITCH];
		vr_menu_active = true;
	}

	dyaw   = vr.hmd_pose.angles[YAW] - vr_menu_yaw;
	dpitch = vr.hmd_pose.angles[PITCH] - vr_menu_pitch;

	while( dyaw > 180.0f )  dyaw -= 360.0f;
	while( dyaw < -180.0f ) dyaw += 360.0f;

	// The leash. Drag the anchor rather than let the panel leave the eye.
	leash = Q_max( 5.0f, vr_menu_leash.value );

	if( dyaw > leash )       { vr_menu_yaw += dyaw - leash; dyaw = leash; }
	else if( dyaw < -leash ) { vr_menu_yaw += dyaw + leash; dyaw = -leash; }

	if( dpitch > leash )       { vr_menu_pitch += dpitch - leash; dpitch = leash; }
	else if( dpitch < -leash ) { vr_menu_pitch += dpitch + leash; dpitch = -leash; }

	// Turning left (yaw increasing) should slide a world-fixed panel right.
	vr_menu_ofs_x = (int)( dyaw * ppd_x );
	vr_menu_ofs_y = (int)( dpitch * ppd_y * vr_menu_pitch_sign.value );

	// POINTER. Where the dominant hand points, expressed against the same rect
	// the menu was drawn into, then handed over as a plain mouse position.
	{
		vec3_t hand_org, hand_ang;
		float ax, ay, fx, fy;
		qboolean click;

		if( !VR_GetHandWorld( VR_DominantHand(), hand_org, hand_ang ))
			return;

		ax = hand_ang[YAW] - vr_menu_yaw;
		ay = hand_ang[PITCH] - vr_menu_pitch;

		while( ax > 180.0f )  ax -= 360.0f;
		while( ax < -180.0f ) ax += 360.0f;

		// Angular half-size of the panel, from its actual pixel size - so the
		// cursor and the drawn menu cannot disagree about where the edges are.
		fx = 0.5f - ( ax * ppd_x ) / (float)rw;
		fy = 0.5f + ( ay * ppd_y * vr_menu_pitch_sign.value ) / (float)rh;

		fx = bound( 0.0f, fx, 1.0f );
		fy = bound( 0.0f, fy, 1.0f );

		UI_MouseMove( (int)( fx * (float)refState.width ),
			(int)( fy * (float)refState.height ));

		// Trigger is the click. Edge-triggered, so a held trigger does not
		// re-fire every frame.
		click = VR_GetButton( VR_BTN_ATTACK ) ? true : false;

		if( click != vr_menu_click_prev )
		{
			UI_KeyEvent( K_MOUSE1, click );
			vr_menu_click_prev = click;
		}
	}
}

void VR_End2D( void )
{
	const vr_swapchain_t *sc = &vr.swapchains[0];

	if( !VR_IsActive() || !vrgl.loaded || !vrgl.Viewport )
		return;

	vrgl.Viewport( 0, 0, (int)sc->width, (int)sc->height );
}

/*
================
VR_OverrideViewAngles

cl.viewangles decides movement direction and weapon aim. Left mouse-controlled,
the player walks wherever the mouse points instead of where they are looking.
Take the mod's yaw as BODY facing (so turning still works) and add head yaw.
================
*/
void VR_OverrideViewAngles( vec3_t angles )
{
	float hmd_yaw;

	if( !VR_IsActive( ) || !angles )
		return;

	// CRITICAL: the yaw arriving here is last frame's OUTPUT plus this frame's
	// mouse delta - and last frame's output already contained the head yaw we
	// injected. Treating it as body facing therefore re-adds head yaw every
	// frame, compounding into an uncontrollable horizontal spin. Subtract what
	// we injected to recover the true body facing.
	// Recover body facing, then fold in this frame's stick turn. body_yaw
	// persists across frames via cl.viewangles: what we write below comes back
	// next frame as angles[YAW], and subtracting injected_yaw recovers it.
	vr.body_yaw = anglemod( angles[YAW] - vr.injected_yaw + vr.turn_delta );
	vr.turn_delta = 0.0f;	// consumed

	hmd_yaw = vr.hmd_pose.angles[YAW];
	vr.injected_yaw = hmd_yaw;

	angles[PITCH] = vr.hmd_pose.angles[PITCH];
	angles[YAW]   = anglemod( vr.body_yaw + hmd_yaw );
	angles[ROLL]  = vr.hmd_pose.angles[ROLL];
}

/*
================
VR_Init

Creates the OpenXR instance and picks the system. Deliberately does NOT create
the session - that needs a live GL context, which does not exist this early.
================
*/
qboolean VR_Init( void )
{
	uint32_t count = 0, i;
	XrExtensionProperties *exts;
	qboolean have_gl = false;
	qboolean have_depth = false;
	XrInstanceCreateInfo ci = { XR_TYPE_INSTANCE_CREATE_INFO };
	const char *enabled[4];
	uint32_t n_enabled = 0;
	XrSystemGetInfo sgi = { XR_TYPE_SYSTEM_GET_INFO };
	XrInstanceProperties ip = { XR_TYPE_INSTANCE_PROPERTIES };

	memset( &vr, 0, sizeof( vr ));
	vr.instance = XR_NULL_HANDLE;
	vr.session  = XR_NULL_HANDLE;
	vr.system   = XR_NULL_SYSTEM_ID;

	// Only one process can usefully hold an OpenXR session. If another XashVR is
	// already running (including one wedged on a crash dialog), take the
	// flatscreen path instead of fighting over the runtime and exhausting its
	// session limit. This also makes running a second local instance for
	// multiplayer testing work sensibly - first one gets the headset.
	vr.instance_mutex = CreateMutexA( NULL, TRUE, "XashVR_OpenXR_SingleInstance" );
	if( vr.instance_mutex && GetLastError() == ERROR_ALREADY_EXISTS )
	{
		Con_Printf( S_WARN "VR: another XashVR instance already holds the headset; running flatscreen\n" );
		Con_Printf( S_WARN "VR: if no other copy is visible, a previous one is stuck - close it in Task Manager\n" );
		CloseHandle( vr.instance_mutex );
		vr.instance_mutex = NULL;
		return false;
	}

	// Backstop for exit paths that never reach R_Shutdown (Sys_Error, a crash
	// dialog, Host_Abort). Without it the runtime keeps the session alive.
	atexit( VR_Shutdown );

	Cvar_RegisterVariable( &vr_enable );
	Cvar_RegisterVariable( &vr_debug );
	Cvar_RegisterVariable( &vr_pitch_sign );
	Cvar_RegisterVariable( &vr_yaw_sign );
	Cvar_RegisterVariable( &vr_roll_sign );
	Cvar_RegisterVariable( &vr_compose_yaw );
	Cvar_RegisterVariable( &vr_diag );
	Cvar_RegisterVariable( &vr_diag_interval );
	Cvar_RegisterVariable( &vr_movespeed );
	Cvar_RegisterVariable( &vr_turnspeed );
	Cvar_RegisterVariable( &vr_snap_turn );
	Cvar_RegisterVariable( &vr_snap_angle );
	Cvar_RegisterVariable( &vr_deadzone );
	Cvar_RegisterVariable( &vr_hud_scale );
	Cvar_RegisterVariable( &vr_hud_parallax );
	Cvar_RegisterVariable( &vr_deathload );
	Cvar_RegisterVariable( &vr_autosave );
	Cvar_RegisterVariable( &vr_shoulder_light );
	Cvar_RegisterVariable( &vr_shoulder_melee );
	Cvar_RegisterVariable( &vr_reload );
	Cvar_RegisterVariable( &vr_reload_side );
	Cvar_RegisterVariable( &vr_reload_down );
	Cvar_RegisterVariable( &vr_reload_back );
	Cvar_RegisterVariable( &vr_reload_radius );
	Cvar_RegisterVariable( &vr_reload_port );
	Cvar_RegisterVariable( &vr_reload_port_fwd );
	Cvar_RegisterVariable( &vr_handload );
	Cvar_RegisterVariable( &vr_pump );
	Cvar_RegisterVariable( &vr_reload_model );
	Cvar_RegisterVariable( &vr_pump_scrub );
	Cvar_RegisterVariable( &vr_pump_ejects );
	Cvar_RegisterVariable( &vr_pump_recoil );
	Cvar_RegisterVariable( &vr_pump_reach );
	Cvar_RegisterVariable( &vr_action_sound );
	Cvar_RegisterVariable( &vr_pump_travel );
	Cvar_RegisterVariable( &vr_reload_hold );
	Cvar_RegisterVariable( &vr_shoulder_radius );
	Cvar_RegisterVariable( &vr_shoulder_side );
	Cvar_RegisterVariable( &vr_shoulder_back );
	Cvar_RegisterVariable( &vr_shoulder_up );
	Cvar_RegisterVariable( &vr_menu_lock );
	Cvar_RegisterVariable( &vr_menu_leash );
	Cvar_RegisterVariable( &vr_menu_pitch_sign );
	Cvar_RegisterVariable( &vr_teleport );
	Cvar_RegisterVariable( &vr_teleport_speed );
	Cvar_RegisterVariable( &vr_teleport_gravity );
	Cvar_RegisterVariable( &vr_teleport_steps );
	Cvar_RegisterVariable( &vr_teleport_step_time );
	Cvar_RegisterVariable( &vr_teleport_width );
	Cvar_RegisterVariable( &vr_teleport_alpha );
	Cvar_RegisterVariable( &vr_supersample );
	Cvar_RegisterVariable( &vr_msaa );
	Cvar_RegisterVariable( &vr_depth_submit );
	Cvar_RegisterVariable( &vr_mirror );
	Cvar_RegisterVariable( &vr_hand_pitch_offset );
	Cvar_RegisterVariable( &vr_hand_yaw_offset );
	Cvar_RegisterVariable( &vr_hand_roll_offset );
	Cvar_RegisterVariable( &vr_twohand );
	Cvar_RegisterVariable( &vr_twohand_min );
	Cvar_RegisterVariable( &vr_twohand_max );
	Cvar_RegisterVariable( &vr_twohand_radius );
	Cvar_RegisterVariable( &vr_twohand_barrel );
	Cvar_RegisterVariable( &vr_twohand_smooth );
	Cvar_RegisterVariable( &vr_laser );
	Cvar_RegisterVariable( &vr_laser_r );
	Cvar_RegisterVariable( &vr_laser_g );
	Cvar_RegisterVariable( &vr_laser_b );
	Cvar_RegisterVariable( &vr_laser_alpha );
	Cvar_RegisterVariable( &vr_laser_width );
	Cvar_RegisterVariable( &vr_laser_dot );
	Cvar_RegisterVariable( &vr_laser_range );
	Cvar_RegisterVariable( &vr_arc );
	Cvar_RegisterVariable( &vr_arc_speed );
	Cvar_RegisterVariable( &vr_arc_gravity );
	Cvar_RegisterVariable( &vr_arc_steps );
	Cvar_RegisterVariable( &vr_arc_step_time );
	Cvar_RegisterVariable( &vr_arc_width );
	Cvar_RegisterVariable( &vr_arc_r );
	Cvar_RegisterVariable( &vr_arc_g );
	Cvar_RegisterVariable( &vr_arc_b );
	Cvar_RegisterVariable( &vr_arc_alpha );
	Cvar_RegisterVariable( &vr_melee_pitch_offset );
	Cvar_RegisterVariable( &vr_melee_swing );
	Cvar_RegisterVariable( &vr_melee_speed );
	Cvar_RegisterVariable( &vr_haptics );
	Cvar_RegisterVariable( &vr_aim_from_weapon );
	Cvar_RegisterVariable( &vr_weapon_origin );
	Cvar_RegisterVariable( &vr_touch_use );
	Cvar_RegisterVariable( &vr_touch_reach );
	Cvar_RegisterVariable( &vr_touch_los );
	Cvar_RegisterVariable( &vr_touch_backoff );
	Cvar_RegisterVariable( &vr_touch_hold );
	Cvar_RegisterVariable( &vr_grip_snap );
	Cvar_RegisterVariable( &vr_grip_pose );
	Cvar_RegisterVariable( &vr_grip_offset_up );
	Cvar_RegisterVariable( &vr_grip_offset_fwd );
	Cvar_RegisterVariable( &vr_grip_offset_side );
	Cvar_RegisterVariable( &vr_grip_pose_pitch );
	Cvar_RegisterVariable( &vr_grip_pose_yaw );
	Cvar_RegisterVariable( &vr_grip_pose_roll );
	Cvar_RegisterVariable( &vr_step_smooth );
	Cvar_RegisterVariable( &vr_roomscale );
	Cvar_RegisterVariable( &vr_roomscale_gain );
	Cvar_RegisterVariable( &vr_roomscale_max );
	Cvar_RegisterVariable( &vr_neck_model );
	Cvar_RegisterVariable( &vr_neck_up );
	Cvar_RegisterVariable( &vr_neck_fwd );
	Cvar_RegisterVariable( &vr_roomscale_noise );
	Cvar_RegisterVariable( &vr_lefthand );
	Cvar_RegisterVariable( &vr_height );
	Cvar_RegisterVariable( &vr_height_offset );
	Cvar_RegisterVariable( &vr_seated );
	Cvar_RegisterVariable( &vr_seated_lift );
	Cvar_RegisterVariable( &vr_crouch );
	Cvar_RegisterVariable( &vr_crouch_ratio );
	Cvar_RegisterVariable( &vr_ladder );
	Cvar_RegisterVariable( &vr_walkdirection );
	Cvar_RegisterVariable( &vr_ladder_hands );
	Cvar_RegisterVariable( &vr_ladder_speed );
	Cvar_RegisterVariable( &vr_ladder_hands_only );
	Cvar_RegisterVariable( &vr_ladder_max );
	Cvar_RegisterVariable( &vr_vignette );
	Cvar_RegisterVariable( &vr_vignette_size );
	Cvar_RegisterVariable( &vr_vignette_fade );
	Cmd_AddCommand( "vr_calibrate", VR_CalibrateHeight_f, "set standing height from your current headset height" );
	Cvar_RegisterVariable( &vr_dual_wield );
	Cvar_RegisterVariable( &vr_offhand_muzzle );
	Cvar_RegisterVariable( &vr_diag_aim );
	Cvar_RegisterVariable( &vr_model_align );
	Cvar_RegisterVariable( &vr_aim_attachment );
	Cvar_RegisterVariable( &vr_aim_pitch_offset );
	Cvar_RegisterVariable( &vr_aim_yaw_offset );
	Cvar_RegisterVariable( &vr_flashlight_hand );
	Cvar_RegisterVariable( &vr_weapon_pitch_offset );
	Cvar_RegisterVariable( &vr_weapon_yaw_offset );
	Cvar_RegisterVariable( &vr_weapon_roll_offset );
	Cvar_RegisterVariable( &vr_hand_pivot_fwd );
	Cvar_RegisterVariable( &vr_hand_pivot_left );
	Cvar_RegisterVariable( &vr_hand_pivot_up );
	Cvar_RegisterVariable( &vr_hands );
	Cvar_RegisterVariable( &vr_hud );
	Cmd_AddCommand( "vr_status", VR_Status_f, "report OpenXR VR state" );

	VR_DiagOpen();
	vrdiag.session_start = host.realtime;

	if( !vr_enable.value || Sys_CheckParm( "-novr" ))
	{
		Con_Printf( "VR: disabled\n" );
		return false;
	}

	// --- required extensions ---
	if( XR_FAILED( xrEnumerateInstanceExtensionProperties( NULL, 0, &count, NULL )) || !count )
	{
		Con_Printf( "VR: no OpenXR runtime available, running flatscreen\n" );
		return false;
	}

	exts = Mem_Calloc( host.mempool, sizeof( *exts ) * count );
	for( i = 0; i < count; i++ )
		exts[i].type = XR_TYPE_EXTENSION_PROPERTIES;

	if( XR_FAILED( xrEnumerateInstanceExtensionProperties( NULL, count, &count, exts )))
	{
		Mem_Free( exts );
		return false;
	}

	for( i = 0; i < count; i++ )
	{
		if( !Q_strcmp( exts[i].extensionName, XR_KHR_OPENGL_ENABLE_EXTENSION_NAME ))
			have_gl = true;
		if( !Q_strcmp( exts[i].extensionName, XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME ))
			have_depth = true;
		if( vr_debug.value )
			Con_Printf( "VR: ext %s v%u\n", exts[i].extensionName, exts[i].extensionVersion );
	}
	Mem_Free( exts );

	if( !have_gl )
	{
		Con_Printf( S_ERROR "VR: runtime lacks %s - cannot submit OpenGL frames\n",
			XR_KHR_OPENGL_ENABLE_EXTENSION_NAME );
		return false;
	}

	enabled[n_enabled++] = XR_KHR_OPENGL_ENABLE_EXTENSION_NAME;

	// SUBMIT DEPTH ALONGSIDE COLOUR.
	//
	// The compositor reprojects every frame it shows to wherever the head
	// actually ended up, and with colour alone it has to assume the whole
	// image sits on one flat plane. Everything at the wrong distance then
	// shears against head motion - worst on near geometry, which in this
	// game means the weapon and the hands, the two things always in view.
	//
	// Given depth it can reproject per pixel instead, and the shear goes.
	// Optional because not every runtime offers it, and losing it costs
	// only accuracy in reprojection, never a frame.
	if( have_depth && vr_depth_submit.value != 0.0f )
	{
		enabled[n_enabled++] = XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME;
		vr.have_depth_ext = true;
	}
	else if( vr_depth_submit.value != 0.0f )
		Con_Printf( "VR: runtime has no %s, reprojection will be flat-plane\n",
			XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME );

	// --- instance ---
	// IMPORTANT: request 1.0, not XR_CURRENT_API_VERSION. Shipping runtimes
	// (e.g. VirtualDesktopXR) reject 1.1 with XR_ERROR_API_VERSION_UNSUPPORTED.
	Q_strncpy( ci.applicationInfo.applicationName, "Xash3D FWGS VR",
		sizeof( ci.applicationInfo.applicationName ));
	Q_strncpy( ci.applicationInfo.engineName, "Xash3D FWGS",
		sizeof( ci.applicationInfo.engineName ));
	ci.applicationInfo.applicationVersion = 1;
	ci.applicationInfo.engineVersion      = 1;
	ci.applicationInfo.apiVersion         = XR_API_VERSION_1_0;
	ci.enabledExtensionCount              = n_enabled;
	ci.enabledExtensionNames              = enabled;

	if( XR_FAILED( xrCreateInstance( &ci, &vr.instance )))
	{
		Con_Printf( "VR: xrCreateInstance failed (is the runtime/headset running?)\n" );
		vr.instance = XR_NULL_HANDLE;
		return false;
	}

	if( XR_SUCCEEDED( xrGetInstanceProperties( vr.instance, &ip )))
	{
		Con_Printf( "VR: runtime %s %u.%u.%u\n", ip.runtimeName,
			(unsigned)XR_VERSION_MAJOR( ip.runtimeVersion ),
			(unsigned)XR_VERSION_MINOR( ip.runtimeVersion ),
			(unsigned)XR_VERSION_PATCH( ip.runtimeVersion ));
	}

	vr.available = true;

	// --- system ---
	// Do NOT tear down on failure: no HMD right now is a normal transient state
	// (streamer not started, headset asleep). Keep the instance and retry.
	if( !VR_AcquireSystem( ))
	{
		Con_Printf( "VR: instance created, waiting for an HMD (retrying in background)\n" );
		Con_Printf( "VR: type 'vr_status' in console for details\n" );
	}

	return true;
}

/*
================
VR_CreateSwapchain
================
*/
/*
================
VR_DestroySwapchain

Release one eye's render targets. Split out of session teardown because the
swapchains are also rebuilt mid-session when the image quality settings
change, and that path needs to let go of the old ones first.
================
*/
static void VR_DestroySwapchain( int eye )
{
	vr_swapchain_t *sc = &vr.swapchains[eye];

	if( sc->fbos && vrgl.loaded )
		vrgl.DeleteFramebuffers( sc->image_count, sc->fbos );
	if( sc->depth_rb && vrgl.loaded )
		vrgl.DeleteRenderbuffers( 1, &sc->depth_rb );
	if( sc->msaa_fbo && vrgl.loaded )
		vrgl.DeleteFramebuffers( 1, &sc->msaa_fbo );
	if( sc->msaa_color && vrgl.loaded )
		vrgl.DeleteRenderbuffers( 1, &sc->msaa_color );
	if( sc->msaa_depth && vrgl.loaded )
		vrgl.DeleteRenderbuffers( 1, &sc->msaa_depth );
	if( sc->depth_handle )
		xrDestroySwapchain( sc->depth_handle );
	if( sc->depth_images ) Mem_Free( sc->depth_images );
	if( sc->handle )
		xrDestroySwapchain( sc->handle );
	if( sc->images ) Mem_Free( sc->images );
	if( sc->fbos )   Mem_Free( sc->fbos );
	memset( sc, 0, sizeof( *sc ));
}

static qboolean VR_CreateSwapchain( int eye )
{
	vr_swapchain_t *sc = &vr.swapchains[eye];
	XrSwapchainCreateInfo ci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
	uint32_t n = 0, i;

	// SUPERSAMPLING.
	//
	// The runtime recommends a size that lands roughly one rendered pixel
	// per display pixel at the centre of the lens. That is a floor, not a
	// target: the optics stretch the image unevenly, so a 1:1 render is
	// already undersampled over most of the view, and the distortion pass
	// resamples it again on the way out. Rendering larger and letting the
	// compositor downsample is the single biggest quality dial in VR.
	//
	// Cost scales with the square, so 1.5 is over twice the pixels of 1.0.
	// Clamped rather than trusted - a stray 4 here would ask for sixteen
	// times the work and simply stop the headset.
	{
		float ss = vr_supersample.value;

		if( ss < 0.5f ) ss = 0.5f;
		if( ss > 2.0f ) ss = 2.0f;

		sc->width  = (uint32_t)( vr.view_configs[eye].recommendedImageRectWidth  * ss + 0.5f );
		sc->height = (uint32_t)( vr.view_configs[eye].recommendedImageRectHeight * ss + 0.5f );

		vr.applied_ss = ss;
	}

	ci.arraySize   = 1;
	ci.mipCount    = 1;
	ci.faceCount   = 1;
	ci.format      = GL_RGBA8_T;
	ci.width       = sc->width;
	ci.height      = sc->height;
	ci.sampleCount = 1;
	ci.usageFlags  = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;

	XR_CHECK( xrCreateSwapchain( vr.session, &ci, &sc->handle ), "xrCreateSwapchain" );
	XR_CHECK( xrEnumerateSwapchainImages( sc->handle, 0, &n, NULL ), "xrEnumerateSwapchainImages" );

	sc->image_count = n;
	sc->images = Mem_Calloc( host.mempool, sizeof( XrSwapchainImageOpenGLKHR ) * n );
	sc->fbos   = Mem_Calloc( host.mempool, sizeof( GLuint_t ) * n );

	for( i = 0; i < n; i++ )
		sc->images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR;

	XR_CHECK( xrEnumerateSwapchainImages( sc->handle, n, &n,
		(XrSwapchainImageBaseHeader *)sc->images ), "xrEnumerateSwapchainImages(fill)" );

	// DEPTH THE COMPOSITOR CAN READ.
	//
	// A second swapchain, owned by the runtime the same way the colour one
	// is, acquired and released alongside it every frame. Only created when
	// the runtime advertised the extension; otherwise depth stays private
	// below and nothing downstream changes.
	if( vr.have_depth_ext )
	{
		XrSwapchainCreateInfo dci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
		uint32_t dn = 0, k;

		// PICK A DEPTH FORMAT THE RUNTIME WILL ACTUALLY TAKE.
		//
		// Hardcoding DEPTH_COMPONENT24 was simply refused. That a runtime
		// advertises the depth extension says nothing about which formats it
		// accepts, and the accepted set differs between them - so ask, and
		// choose from what comes back rather than assuming.
		//
		// Depth-only formats are preferred over packed depth-stencil purely so
		// the FBO attachment below stays a plain GL_DEPTH_ATTACHMENT; a packed
		// format properly wants GL_DEPTH_STENCIL_ATTACHMENT and is only taken
		// when nothing simpler is on offer.
		{
			static const int64_t want[] = {
				GL_DEPTH_COMPONENT32F_T,
				GL_DEPTH_COMPONENT24_EXT,
				GL_DEPTH_COMPONENT16_T,
				GL_DEPTH24_STENCIL8_T,
			};
			int64_t *fmts = NULL;
			uint32_t nf = 0, fi, wi;
			int64_t chosen = 0;

			if( XR_SUCCEEDED( xrEnumerateSwapchainFormats( vr.session, 0, &nf, NULL )) && nf )
			{
				fmts = Mem_Calloc( host.mempool, sizeof( int64_t ) * nf );

				if( XR_FAILED( xrEnumerateSwapchainFormats( vr.session, nf, &nf, fmts )))
					nf = 0;
			}

			for( wi = 0; wi < (uint32_t)( sizeof( want ) / sizeof( want[0] )) && !chosen; wi++ )
			{
				for( fi = 0; fi < nf; fi++ )
				{
					if( fmts[fi] == want[wi] )
					{
						chosen = want[wi];
						break;
					}
				}
			}

			if( fmts ) Mem_Free( fmts );

			if( !chosen )
			{
				Con_Printf( "VR: eye %d - runtime offers no usable depth format\n", eye );
				vr.have_depth_ext = false;
			}
			else
				dci.format = chosen;
		}

		if( !vr.have_depth_ext )
			goto no_depth;

		dci.arraySize   = 1;
		dci.mipCount    = 1;
		dci.faceCount   = 1;
		dci.width       = sc->width;
		dci.height      = sc->height;
		dci.sampleCount = 1;
		dci.usageFlags  = XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;

		// Not fatal. A runtime may advertise the extension and still refuse
		// this format, and a frame without depth is only a less accurate
		// reprojection - never a missing frame.
		if( XR_SUCCEEDED( xrCreateSwapchain( vr.session, &dci, &sc->depth_handle ))
			&& XR_SUCCEEDED( xrEnumerateSwapchainImages( sc->depth_handle, 0, &dn, NULL ))
			&& dn > 0 )
		{
			sc->depth_image_count = dn;
			sc->depth_images = Mem_Calloc( host.mempool,
				sizeof( XrSwapchainImageOpenGLKHR ) * dn );

			for( k = 0; k < dn; k++ )
				sc->depth_images[k].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR;

			if( XR_FAILED( xrEnumerateSwapchainImages( sc->depth_handle, dn, &dn,
				(XrSwapchainImageBaseHeader *)sc->depth_images )))
			{
				Mem_Free( sc->depth_images );
				sc->depth_images = NULL;
				xrDestroySwapchain( sc->depth_handle );
				sc->depth_handle = 0;
			}
		}
		else
		{
			if( sc->depth_handle ) xrDestroySwapchain( sc->depth_handle );
			sc->depth_handle = 0;
			Con_Printf( "VR: eye %d depth swapchain unavailable, colour only\n", eye );
		}
no_depth:
		;
	}

	// Private depth, ONLY when the runtime is not taking it. With a depth
	// swapchain the attachment changes every frame to whichever image was
	// acquired, so a fixed renderbuffer here would only be overwritten.
	if( !sc->depth_handle )
	{
		vrgl.GenRenderbuffers( 1, &sc->depth_rb );
		vrgl.BindRenderbuffer( GL_RENDERBUFFER_EXT, sc->depth_rb );
		vrgl.RenderbufferStorage( GL_RENDERBUFFER_EXT, GL_DEPTH_COMPONENT24_EXT,
			sc->width, sc->height );
		vrgl.BindRenderbuffer( GL_RENDERBUFFER_EXT, 0 );
	}

	// MULTISAMPLED RENDER TARGET.
	//
	// Half-Life is built out of railings, grates, pipes and ladder rungs -
	// thin high-contrast edges everywhere - and in stereo each eye aliases
	// them differently. The result does not read as jagged so much as
	// unstable, because the two eyes disagree about where the edge is and
	// the disagreement shifts with every small movement of the head.
	//
	// Runtimes hand out single-sampled swapchain images, so the eye is drawn
	// into this instead and resolved across in VR_EndEye.
	sc->samples = (int)vr_msaa.value;
	if( sc->samples != 2 && sc->samples != 4 && sc->samples != 8 )
		sc->samples = 0;

	vr.applied_msaa = sc->samples;

	if( sc->samples && !vrgl.RenderbufferStorageMultisample )
	{
		Con_Printf( "VR: no glRenderbufferStorageMultisample, MSAA off\n" );
		sc->samples = 0;
	}

	if( sc->samples )
	{
		GLenum_t st;

		vrgl.GenRenderbuffers( 1, &sc->msaa_color );
		vrgl.BindRenderbuffer( GL_RENDERBUFFER_EXT, sc->msaa_color );
		vrgl.RenderbufferStorageMultisample( GL_RENDERBUFFER_EXT, sc->samples,
			GL_RGBA8_T, sc->width, sc->height );

		vrgl.GenRenderbuffers( 1, &sc->msaa_depth );
		vrgl.BindRenderbuffer( GL_RENDERBUFFER_EXT, sc->msaa_depth );
		vrgl.RenderbufferStorageMultisample( GL_RENDERBUFFER_EXT, sc->samples,
			GL_DEPTH_COMPONENT24_EXT, sc->width, sc->height );
		vrgl.BindRenderbuffer( GL_RENDERBUFFER_EXT, 0 );

		vrgl.GenFramebuffers( 1, &sc->msaa_fbo );
		vrgl.BindFramebuffer( GL_FRAMEBUFFER_EXT, sc->msaa_fbo );
		vrgl.FramebufferRenderbuffer( GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT,
			GL_RENDERBUFFER_EXT, sc->msaa_color );
		vrgl.FramebufferRenderbuffer( GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT,
			GL_RENDERBUFFER_EXT, sc->msaa_depth );
		st = vrgl.CheckFramebufferStatus( GL_FRAMEBUFFER_EXT );
		vrgl.BindFramebuffer( GL_FRAMEBUFFER_EXT, 0 );

		// Fall back to no MSAA rather than refuse to render. An unsupported
		// sample count is a setting to ignore, not a reason to lose the game.
		if( st != GL_FRAMEBUFFER_COMPLETE_EXT )
		{
			Con_Printf( S_ERROR "VR: eye %d %dx MSAA unavailable (0x%x), falling back\n",
				eye, sc->samples, st );
			vrgl.DeleteFramebuffers( 1, &sc->msaa_fbo );
			vrgl.DeleteRenderbuffers( 1, &sc->msaa_color );
			vrgl.DeleteRenderbuffers( 1, &sc->msaa_depth );
			sc->msaa_fbo = sc->msaa_color = sc->msaa_depth = 0;
			sc->samples = 0;
		}
	}

	// wrap each swapchain texture in an FBO once, up front
	vrgl.GenFramebuffers( n, sc->fbos );
	for( i = 0; i < n; i++ )
	{
		GLenum_t status;

		vrgl.BindFramebuffer( GL_FRAMEBUFFER_EXT, sc->fbos[i] );
		vrgl.FramebufferTexture2D( GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT,
			GL_TEXTURE_2D_T, sc->images[i].image, 0 );
		if( sc->depth_rb )
			vrgl.FramebufferRenderbuffer( GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT,
				GL_RENDERBUFFER_EXT, sc->depth_rb );

		status = vrgl.CheckFramebufferStatus( GL_FRAMEBUFFER_EXT );
		if( status != GL_FRAMEBUFFER_COMPLETE_EXT )
		{
			Con_Printf( S_ERROR "VR: eye %d FBO %u incomplete (0x%x)\n", eye, i, status );
			vrgl.BindFramebuffer( GL_FRAMEBUFFER_EXT, 0 );
			return false;
		}
	}
	vrgl.BindFramebuffer( GL_FRAMEBUFFER_EXT, 0 );

	Con_Printf( "VR: eye %d swapchain %ux%u, %u images, %dx MSAA, depth %s\n",
		eye, sc->width, sc->height, n, sc->samples ? sc->samples : 1,
		sc->depth_handle ? "submitted" : "private" );
	return true;
}

/*
================
VR_DestroySession

Release everything owned by a session, so a failed attempt can be retried
without leaking. Safe to call on a partially constructed session.
================
*/
static void VR_DestroySession( void )
{
	int i;

	for( i = 0; i < VR_MAX_EYES; i++ )
		VR_DestroySwapchain( i );

	for( i = 0; i < 2; i++ )
	{
		if( vr.hand_space[i] )
			xrDestroySpace( vr.hand_space[i] );
		vr.hand_space[i] = 0;

		if( vr.grip_space[i] )
			xrDestroySpace( vr.grip_space[i] );
		vr.grip_space[i] = 0;
	}

	if( vr.view_space )  xrDestroySpace( vr.view_space );
	if( vr.stage_space ) xrDestroySpace( vr.stage_space );
	vr.view_space = vr.stage_space = 0;

	if( vr.session != XR_NULL_HANDLE )
		xrDestroySession( vr.session );
	vr.session = XR_NULL_HANDLE;

	vr.session_ready = false;
	vr.running       = false;
	vr.frame_started = false;
	vr.input_ready   = false;
	vr.action_set    = 0;
}

/*
================
VR_InitSession

Requires a current GL context.
================
*/
qboolean VR_InitSession( void )
{
	XrGraphicsBindingOpenGLWin32KHR binding = { XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR };
	XrSessionCreateInfo ci = { XR_TYPE_SESSION_CREATE_INFO };
	XrReferenceSpaceCreateInfo rsci = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
	int i;

	if( !vr.available || vr.session_ready )
		return vr.session_ready;

	if( !VR_LoadGLFuncs( ))
	{
		Con_Printf( S_ERROR "VR: could not resolve GL entry points\n" );
		return false;
	}

	binding.hDC   = wglGetCurrentDC();
	binding.hGLRC = wglGetCurrentContext();

	if( !binding.hDC || !binding.hGLRC )
	{
		Con_Printf( S_ERROR "VR: no current WGL context (hDC=%p hGLRC=%p)\n",
			binding.hDC, binding.hGLRC );
		return false;
	}

	ci.next     = &binding;
	ci.systemId = vr.system;

	XR_CHECK( xrCreateSession( vr.instance, &ci, &vr.session ), "xrCreateSession" );

	// Room-scale space. STAGE gives a floor-level origin, which is what we want
	// for roomscale reconciliation later; fall back to LOCAL if unsupported.
	rsci.poseInReferenceSpace.orientation.w = 1.0f;
	rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
	if( XR_FAILED( xrCreateReferenceSpace( vr.session, &rsci, &vr.stage_space )))
	{
		Con_Printf( "VR: STAGE space unavailable, falling back to LOCAL\n" );
		rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
		XR_CHECK( xrCreateReferenceSpace( vr.session, &rsci, &vr.stage_space ),
			"xrCreateReferenceSpace(LOCAL)" );
	}

	rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
	XR_CHECK( xrCreateReferenceSpace( vr.session, &rsci, &vr.view_space ),
		"xrCreateReferenceSpace(VIEW)" );

	for( i = 0; i < vr.eye_count; i++ )
	{
		if( !VR_CreateSwapchain( i ))
		{
			// CRITICAL: the session already exists at this point. Returning
			// without destroying it leaks one session per attempt, and since
			// VR_BeginFrame retries every frame that reaches
			// XR_ERROR_LIMIT_REACHED within seconds. Tear down and back off.
			Con_Printf( S_ERROR "VR: swapchain setup failed, releasing session\n" );
			VR_DestroySession();
			vr.session_retries++;
			if( vr.session_retries >= 3 )
			{
				Con_Printf( S_ERROR "VR: giving up on session creation, running flatscreen\n" );
				vr.available = false;
			}
			return false;
		}
		vr.views[i].type = XR_TYPE_VIEW;
	}

	vr.session_ready = true;

	// The headset, not the monitor, paces a VR frame - xrWaitFrame blocks to the
	// HMD's refresh. Leaving desktop vsync on additionally clamps us to the
	// monitor (60Hz here), starving a 72/90Hz headset and causing judder.
	Cvar_Set( "gl_vsync", "0" );

	// gl_clear defaults to 0 because in flatscreen the 3D scene overwrites
	// every pixel, making a clear wasted work. In VR nothing draws to the
	// window at all (both eyes go to OpenXR FBOs), so without this the window
	// swaps an unwritten buffer full of garbage. Needed even with the mirror
	// blit enabled, so the letterbox bars are black instead of stale memory.
	Cvar_Set( "gl_clear", "1" );

	// REVERTED: forcing r_studio_builtin_renderer globally did make the
	// mirror fire (confirmed), but it routes EVERY entity through the
	// engine's own studio path instead of Half-Life's client.dll-provided
	// one - and broke weapon pickup, which evidently depends on something
	// the client.dll's own path does. That regression is worse than an
	// unmirrored hand. The surgical fix - targeting only the VR hand
	// entities by model name - lives in R_StudioDrawModelInternal
	// (gl_studio.c), not here.
	Con_Printf( "VR: disabled desktop vsync (headset paces frames)\n" );

	Con_Printf( "VR: session created\n" );

	// action sets must be attached before the session starts running
	VR_InitInput();

	return true;
}

/*
=================================================================
	input - OpenXR action sets

OpenXR does not expose raw buttons. Actions are declared semantically ("move",
"jump") and bound per controller type, so one set of code drives Touch, Index,
Vive wands, WMR, and anything else the runtime can map.
=================================================================
*/
static XrPath VR_Path( const char *s )
{
	XrPath p = XR_NULL_PATH;

	xrStringToPath( vr.instance, s, &p );
	return p;
}

// Binding table: one row per interaction profile, one path per action.
// NULL leaves an action unbound on that device.
//
// Two separate pose bindings per profile, not one - OpenXR defines these for
// different purposes and conflating them is what caused the hand mesh to
// point at the floor:
//   aim_*  - "aim/pose": where the controller is pointed. Drives weapon aim.
//   grip_* - "grip/pose": how a held object naturally sits in the hand. This
//            is what OpenXR's own spec says to use for rendering a held
//            object's mesh - including the hand model itself.
typedef struct
{
	const char *profile;
	const char *path[VRA_COUNT];
	const char *aim_l, *aim_r;
	const char *grip_l, *grip_r;
} vr_profile_t;

static const vr_profile_t vr_profiles[] =
{
	{
		"/interaction_profiles/oculus/touch_controller",
		{
			"/user/hand/left/input/thumbstick",		// MOVE
			"/user/hand/right/input/thumbstick",		// TURN
			"/user/hand/right/input/a/click",		// JUMP
			"/user/hand/right/input/b/click",		// CROUCH
			"/user/hand/right/input/trigger/value",		// ATTACK
			"/user/hand/right/input/squeeze/value",		// ATTACK2
			// USE moved off the left grip so the grip can be a dedicated
			// GRAB. Left trigger was unused in every profile, and
			// trigger-to-interact / grip-to-grab is the conventional VR
			// mapping anyway.
			"/user/hand/left/input/trigger/value",		// USE
			"/user/hand/left/input/x/click",		// RELOAD
			"/user/hand/left/input/y/click",		// FLASHLIGHT
			"/user/hand/right/input/thumbstick/click",	// NEXTWEAP
			"/user/hand/left/input/thumbstick/click",	// PREVWEAP
			"/user/hand/left/input/menu/click",		// MENU
			"/user/hand/left/input/squeeze/value",		// OFFGRIP
		},
		"/user/hand/left/input/aim/pose",  "/user/hand/right/input/aim/pose",
		"/user/hand/left/input/grip/pose", "/user/hand/right/input/grip/pose"
	},
	{
		"/interaction_profiles/valve/index_controller",
		{
			"/user/hand/left/input/thumbstick",
			"/user/hand/right/input/thumbstick",
			"/user/hand/right/input/a/click",
			"/user/hand/right/input/b/click",
			"/user/hand/right/input/trigger/value",
			"/user/hand/right/input/squeeze/value",
			"/user/hand/left/input/trigger/value",		// USE
			"/user/hand/left/input/a/click",
			"/user/hand/left/input/b/click",
			"/user/hand/right/input/thumbstick/click",
			"/user/hand/left/input/thumbstick/click",
			"/user/hand/left/input/system/click",
			"/user/hand/left/input/squeeze/value",		// OFFGRIP
		},
		"/user/hand/left/input/aim/pose",  "/user/hand/right/input/aim/pose",
		"/user/hand/left/input/grip/pose", "/user/hand/right/input/grip/pose"
	},
	{
		"/interaction_profiles/microsoft/motion_controller",
		{
			"/user/hand/left/input/thumbstick",
			"/user/hand/right/input/thumbstick",
			"/user/hand/right/input/trackpad/click",
			"/user/hand/left/input/trackpad/click",
			"/user/hand/right/input/trigger",
			"/user/hand/right/input/squeeze/click",
			"/user/hand/left/input/trigger",		// USE
			NULL,
			NULL,
			"/user/hand/right/input/thumbstick/click",
			"/user/hand/left/input/thumbstick/click",
			"/user/hand/left/input/menu/click",
			"/user/hand/left/input/squeeze/click",		// OFFGRIP
		},
		"/user/hand/left/input/aim/pose",  "/user/hand/right/input/aim/pose",
		"/user/hand/left/input/grip/pose", "/user/hand/right/input/grip/pose"
	},
	{
		"/interaction_profiles/htc/vive_controller",
		{
			"/user/hand/left/input/trackpad",
			"/user/hand/right/input/trackpad",
			"/user/hand/right/input/trackpad/click",
			"/user/hand/left/input/trackpad/click",
			"/user/hand/right/input/trigger/value",
			"/user/hand/right/input/squeeze/click",
			"/user/hand/left/input/trigger/value",		// USE
			NULL,
			NULL,
			NULL,
			NULL,
			"/user/hand/left/input/menu/click",
			"/user/hand/left/input/squeeze/click",		// OFFGRIP
		},
		"/user/hand/left/input/aim/pose",  "/user/hand/right/input/aim/pose",
		"/user/hand/left/input/grip/pose", "/user/hand/right/input/grip/pose"
	},
};

static qboolean VR_SuggestProfile( const vr_profile_t *p )
{
	XrInteractionProfileSuggestedBinding sb = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
	XrActionSuggestedBinding b[VRA_COUNT + 6];	// +6: aim_l/r, grip_l/r, haptic_l/r
	uint32_t n = 0;
	int i;
	XrResult res;

	for( i = 0; i < VRA_COUNT; i++ )
	{
		if( !p->path[i] )
			continue;
		b[n].action  = vr.actions[i];
		b[n].binding = VR_Path( p->path[i] );
		n++;
	}

	if( p->aim_l )  { b[n].action = vr.act_hand_pose; b[n].binding = VR_Path( p->aim_l );  n++; }
	if( p->aim_r )  { b[n].action = vr.act_hand_pose; b[n].binding = VR_Path( p->aim_r );  n++; }
	if( p->grip_l ) { b[n].action = vr.act_hand_grip; b[n].binding = VR_Path( p->grip_l ); n++; }
	if( p->grip_r ) { b[n].action = vr.act_hand_grip; b[n].binding = VR_Path( p->grip_r ); n++; }

	// Haptic output. The standard path is the same across every profile, so
	// it is not part of the per-profile table.
	b[n].action = vr.act_haptic[0]; b[n].binding = VR_Path( "/user/hand/left/output/haptic" );  n++;
	b[n].action = vr.act_haptic[1]; b[n].binding = VR_Path( "/user/hand/right/output/haptic" ); n++;

	sb.interactionProfile     = VR_Path( p->profile );
	sb.suggestedBindings      = b;
	sb.countSuggestedBindings = n;

	res = xrSuggestInteractionProfileBindings( vr.instance, &sb );
	if( XR_FAILED( res ))
	{
		// not fatal - a runtime rejects device profiles it does not know
		if( vr_debug.value )
			Con_Printf( "VR: bindings for %s rejected: %s\n", p->profile, VR_ResultString( res ));
		VR_DiagPrintf( "bindings FAILED: %s\n", p->profile );
		return false;
	}

	VR_DiagPrintf( "bindings ok    : %s (%u paths)\n", p->profile, n );
	return true;
}

/*
================
VR_InitInput

Actions are instance-level, but the action SET is attached to the session and
attachment is permanent - so this runs once, right after session creation and
before the session starts running.
================
*/
static qboolean VR_InitInput( void )
{
	XrActionSetCreateInfo asci = { XR_TYPE_ACTION_SET_CREATE_INFO };
	XrSessionActionSetsAttachInfo ai = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
	XrActionCreateInfo aci;
	int i, profiles = 0;

	if( vr.input_ready )
		return true;

	Q_strncpy( asci.actionSetName, "gameplay", sizeof( asci.actionSetName ));
	Q_strncpy( asci.localizedActionSetName, "Gameplay", sizeof( asci.localizedActionSetName ));
	asci.priority = 0;
	XR_CHECK( xrCreateActionSet( vr.instance, &asci, &vr.action_set ), "xrCreateActionSet" );

	vr.hand_path[0] = VR_Path( "/user/hand/left" );
	vr.hand_path[1] = VR_Path( "/user/hand/right" );

	// NOTE: deliberately NO subaction paths on the gameplay actions. Declaring
	// them forces every query to name a hand, and an action bound only to one
	// controller then reads inactive when queried against the other - which is
	// exactly what silently killed turning. Without them, a query using
	// XR_NULL_PATH aggregates across all bindings.
	for( i = 0; i < VRA_COUNT; i++ )
	{
		memset( &aci, 0, sizeof( aci ));
		aci.type       = XR_TYPE_ACTION_CREATE_INFO;
		aci.actionType = vr_action_defs[i].type;
		Q_strncpy( aci.actionName, vr_action_defs[i].name, sizeof( aci.actionName ));
		Q_strncpy( aci.localizedActionName, vr_action_defs[i].label, sizeof( aci.localizedActionName ));
		XR_CHECK( xrCreateAction( vr.action_set, &aci, &vr.actions[i] ), "xrCreateAction" );
	}

	// hand poses DO want subaction paths - we need them told apart.
	// Two separate pose actions, not one: aim/pose (pointing direction, drives
	// weapon aim) and grip/pose (how a held object sits in the hand - what
	// OpenXR's spec says to use for rendering the hand mesh itself). Using one
	// pose for both is what caused the hand mesh to render pointed at the
	// floor after the weapon-aim fix.
	memset( &aci, 0, sizeof( aci ));
	aci.type       = XR_TYPE_ACTION_CREATE_INFO;
	aci.actionType = XR_ACTION_TYPE_POSE_INPUT;
	Q_strncpy( aci.actionName, "hand", sizeof( aci.actionName ));
	Q_strncpy( aci.localizedActionName, "Hand Aim", sizeof( aci.localizedActionName ));
	aci.countSubactionPaths = 2;
	aci.subactionPaths      = vr.hand_path;
	XR_CHECK( xrCreateAction( vr.action_set, &aci, &vr.act_hand_pose ), "xrCreateAction hand" );

	memset( &aci, 0, sizeof( aci ));
	aci.type       = XR_TYPE_ACTION_CREATE_INFO;
	aci.actionType = XR_ACTION_TYPE_POSE_INPUT;
	Q_strncpy( aci.actionName, "handgrip", sizeof( aci.actionName ));
	Q_strncpy( aci.localizedActionName, "Hand Grip", sizeof( aci.localizedActionName ));
	aci.countSubactionPaths = 2;
	aci.subactionPaths      = vr.hand_path;
	XR_CHECK( xrCreateAction( vr.action_set, &aci, &vr.act_hand_grip ), "xrCreateAction handgrip" );

	// Haptic output, one action per hand. Separate actions rather than one
	// with subaction paths, so a buzz can be sent to a specific controller
	// without having to plumb subaction paths through every call site.
	memset( &aci, 0, sizeof( aci ));
	aci.type       = XR_TYPE_ACTION_CREATE_INFO;
	aci.actionType = XR_ACTION_TYPE_VIBRATION_OUTPUT;
	Q_strncpy( aci.actionName, "haptic_left", sizeof( aci.actionName ));
	Q_strncpy( aci.localizedActionName, "Haptic Left", sizeof( aci.localizedActionName ));
	XR_CHECK( xrCreateAction( vr.action_set, &aci, &vr.act_haptic[0] ), "xrCreateAction haptic_left" );

	memset( &aci, 0, sizeof( aci ));
	aci.type       = XR_TYPE_ACTION_CREATE_INFO;
	aci.actionType = XR_ACTION_TYPE_VIBRATION_OUTPUT;
	Q_strncpy( aci.actionName, "haptic_right", sizeof( aci.actionName ));
	Q_strncpy( aci.localizedActionName, "Haptic Right", sizeof( aci.localizedActionName ));
	XR_CHECK( xrCreateAction( vr.action_set, &aci, &vr.act_haptic[1] ), "xrCreateAction haptic_right" );

	for( i = 0; i < (int)( sizeof( vr_profiles ) / sizeof( vr_profiles[0] )); i++ )
	{
		if( VR_SuggestProfile( &vr_profiles[i] ))
			profiles++;
	}

	if( !profiles )
		Con_Printf( S_WARN "VR: no interaction profile bindings accepted\n" );

	for( i = 0; i < 2; i++ )
	{
		XrActionSpaceCreateInfo asp = { XR_TYPE_ACTION_SPACE_CREATE_INFO };

		asp.action        = vr.act_hand_pose;
		asp.subactionPath = vr.hand_path[i];
		asp.poseInActionSpace.orientation.w = 1.0f;

		if( XR_FAILED( xrCreateActionSpace( vr.session, &asp, &vr.hand_space[i] )))
			Con_Printf( S_WARN "VR: could not create hand space %d\n", i );

		asp.action = vr.act_hand_grip;

		if( XR_FAILED( xrCreateActionSpace( vr.session, &asp, &vr.grip_space[i] )))
			Con_Printf( S_WARN "VR: could not create hand grip space %d\n", i );
	}

	ai.countActionSets = 1;
	ai.actionSets      = &vr.action_set;
	XR_CHECK( xrAttachSessionActionSets( vr.session, &ai ), "xrAttachSessionActionSets" );

	vr.input_ready = true;
	Con_Printf( "VR: input ready (%d interaction profiles)\n", profiles );
	VR_DiagPrintf( "input          : ready, %d profiles\n\n", profiles );
	return true;
}

/*
================
VR_SyncInput
================
*/
static void VR_SyncInput( void )
{
	XrActiveActionSet aas = { 0 };
	XrActionsSyncInfo si = { XR_TYPE_ACTIONS_SYNC_INFO };
	XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
	XrActionStateVector2f v2 = { XR_TYPE_ACTION_STATE_VECTOR2F };
	XrActionStateBoolean bl = { XR_TYPE_ACTION_STATE_BOOLEAN };
	int i;

	if( !vr.input_ready )
		return;

	aas.actionSet     = vr.action_set;
	aas.subactionPath = XR_NULL_PATH;
	si.countActiveActionSets = 1;
	si.activeActionSets      = &aas;

	if( XR_FAILED( xrSyncActions( vr.session, &si )))
		return;

	// Report which interaction profile the runtime actually bound per hand. This
	// is the definitive answer to "are my bindings live?" - suggestion succeeding
	// only means the runtime understood the profile, not that it selected it.
	if( !vr.profiles_logged && vr.frames_submitted > 30 )
	{
		int h;

		vr.profiles_logged = true;
		for( h = 0; h < 2; h++ )
		{
			XrInteractionProfileState ips = { XR_TYPE_INTERACTION_PROFILE_STATE };
			char buf[XR_MAX_PATH_LENGTH];
			uint32_t len = 0;

			if( XR_FAILED( xrGetCurrentInteractionProfile( vr.session, vr.hand_path[h], &ips )))
				continue;

			if( ips.interactionProfile == XR_NULL_PATH )
			{
				VR_DiagPrintf( "active profile %s: NONE (controller asleep or absent)\n",
					h ? "right" : "left" );
				continue;
			}

			if( XR_SUCCEEDED( xrPathToString( vr.instance, ips.interactionProfile,
				sizeof( buf ), &len, buf )))
				VR_DiagPrintf( "active profile %s: %s\n", h ? "right" : "left", buf );
		}
	}

	memcpy( vr.btn_prev, vr.btn, sizeof( vr.btn ));
	memset( vr.btn, 0, sizeof( vr.btn ));
	vr.move_x = vr.move_y = vr.turn_x = vr.turn_y = 0.0f;

	gi.subactionPath = XR_NULL_PATH;	// aggregate across every binding

	gi.action = vr.actions[VRA_MOVE];
	if( XR_SUCCEEDED( xrGetActionStateVector2f( vr.session, &gi, &v2 )) && v2.isActive )
	{
		vr.move_x = v2.currentState.x;
		vr.move_y = v2.currentState.y;
	}

	gi.action = vr.actions[VRA_TURN];
	vr.turn_active = false;
	if( XR_SUCCEEDED( xrGetActionStateVector2f( vr.session, &gi, &v2 )) && v2.isActive )
	{
		vr.turn_active = true;
		vr.turn_x = v2.currentState.x;
		vr.turn_y = v2.currentState.y;
	}

	for( i = VRA_JUMP; i < VRA_COUNT; i++ )
	{
		gi.action = vr.actions[i];
		if( XR_SUCCEEDED( xrGetActionStateBoolean( vr.session, &gi, &bl )) && bl.isActive )
			vr.btn[i] = bl.currentState ? true : false;
	}

	// One-shot actions are console commands, dispatched on the press edge only.
	if( vr.btn[VRA_FLASHLIGHT] && !vr.btn_prev[VRA_FLASHLIGHT] )
		Cbuf_AddText( "impulse 100\n" );
	/*
	MAIN-HAND GRIP IS A MODIFIER LAYER.
	
	Held, it re-tasks the main hand's own trigger and stick rather than doing
	anything itself:
	
	    grip + trigger      secondary fire
	    grip + stick right  next weapon
	    grip + stick left   previous weapon
	    grip + stick click  toggle the weapon select
	    (select open) fire  take the highlighted weapon
	
	This costs nothing and doubles every control on that hand: the grip was
	previously secondary fire and nothing else, which a modifier absorbs for
	free.
	
	The stick cannot turn and cycle at once, so turning is suppressed while the
	grip is down - see VR_UpdateTurn.
	
	"Fire selects" needs no code: with hud_fastswitch off the mod's own client
	DLL consumes +attack as the select confirm instead of firing. That is stock
	GoldSrc behaviour, so it is true in every mod.
	*/
	{
		static qboolean cyc_prev = false, click_prev = false;
		// The grip is the rung on a ladder, not a modifier. Both hands are
		// climbing, the weapon is stowed, and there is nothing to cycle - so
		// letting it double as the weapon-select layer meant every pull on a
		// rung also armed the modifier. That overload is what made the whole
		// ladder feature unusable the first time.
		qboolean grip  = vr.btn[VRA_ATTACK2] && !VR_LadderHands();
		// The toggle is the thumbstick CLICK, and the same thumbstick is being
		// flicked left and right to cycle. Pressing a stick slightly inward while
		// pushing it sideways is close to unavoidable, so a bare click test made
		// cycling summon the menu constantly - measured, 20 of 22 cycles in one
		// session happened with a menu the player never asked for. Only count a
		// click made with the stick actually centred.
		qboolean click = ( vr.btn[VRA_NEXTWEAP]
			&& fabs( vr.turn_x ) < 0.4f && fabs( vr.turn_y ) < 0.4f
			&& fabs( vr.move_x ) < 0.4f && fabs( vr.move_y ) < 0.4f );
		
		// EITHER stick drives the cycle.
		//
		// "main-hand stick" and "left stick" were both used to describe this and
		// they are different sticks, so reading only one of them meant watching
		// an input that was never touched. Accepting both removes the ambiguity
		// entirely and costs nothing: the grip modifier already reserves both
		// sticks for this while it is held.
		float stick_x = ( fabs( vr.turn_x ) >= fabs( vr.move_x )) ? vr.turn_x : vr.move_x;
		qboolean cyc  = ( grip && fabs( stick_x ) > 0.6f );
	
		if( grip )
		{
			// Edge-triggered, so one flick is one weapon rather than a burst.
			//
			// The fast-switch state is carried IN the command string rather than
			// set around it, because Cbuf_AddText only queues - a cvar set and
			// restored either side would both have happened before the queued
			// command ever ran.
			//
			// With the select CLOSED this must switch weapons outright and not
			// summon the menu, so fast-switch is forced on for the command. With
			// it OPEN the same command walks the highlight instead, which is what
			// invnext already does in that state.
			if( cyc && !cyc_prev )
			{
				const char *dir = ( stick_x > 0.0f ) ? "invnext" : "invprev";
				
				VR_DiagPrintf( "SELCYC dir=%s open=%d grip=%d turn_x=%.2f move_x=%.2f\n",
					dir, vr.select_open ? 1 : 0, grip ? 1 : 0, vr.turn_x, vr.move_x );
				
				if( vr.select_open )
					Cbuf_AddText( va( "%s\n", dir ));
				else
					Cbuf_AddText( va( "hud_fastswitch 1; %s\n", dir ));
			}
	
			if( click && !click_prev )
			{
				if( !vr.select_open )
				{
					// The select HUD only appears with fast-switch OFF;
					// with it on invnext just switches silently.
					vr.select_fastswitch = Cvar_VariableValue( "hud_fastswitch" );
					Cvar_SetValue( "hud_fastswitch", 0.0f );
					Cbuf_AddText( "invnext\n" );
					vr.select_open = true;
				}
				else
				{
					Cbuf_AddText( "cancelselect\n" );
					Cvar_SetValue( "hud_fastswitch", vr.select_fastswitch );
					vr.select_open = false;
				}
				VR_Haptic( VR_DominantHand(), 0.04f, 0.0f, 0.5f );
			}
		}
	
		// The mod closes the select itself once fire confirms, so put the
		// player's own fast-switch setting back rather than leaving it off for
		// the rest of the session.
		// CONFIRMING A WEAPON NEEDS THE CLIENT DLL'S OWN BUTTON STATE, NOT OURS.
		//
		// ammo.cpp takes a weapon when gHUD.m_iKeyBits & IN_ATTACK, and
		// hud_update.cpp fills m_iKeyBits from CL_ButtonBits() - the client
		// DLL's own accumulator, driven by the "+attack" COMMAND. Setting
		// cmd->buttons never touches it, which is why firing at an open select
		// did nothing however the bit was routed.
		//
		// So issue the command. This is also what stops the gun going off:
		// ammo.cpp clears the bit on confirm and CL_ResetButtonBits pushes that
		// back into the client's input state, which is exactly how a desktop
		// player confirms with attack without firing.
		if( vr.select_open )
		{
			// Either trigger press takes the weapon. VRA_ATTACK covers fire and
			// grip+trigger alike, since alt-fire is the same physical trigger;
			// VRA_ATTACK2 is added on its own EDGE so a grip that is merely being
			// held to cycle cannot confirm, but a deliberate squeeze can.
			if(( vr.btn[VRA_ATTACK] && !vr.btn_prev[VRA_ATTACK] )
				|| ( vr.btn[VRA_ATTACK2] && !vr.btn_prev[VRA_ATTACK2] ))
			{
				Cbuf_AddText( "+attack\n" );
				VR_DiagPrintf( "SELTAKE +attack issued\n" );
			}
			else if( !vr.btn[VRA_ATTACK] && vr.btn_prev[VRA_ATTACK] )
			{
				Cbuf_AddText( "-attack\n" );
				Cvar_SetValue( "hud_fastswitch", vr.select_fastswitch );
				vr.select_open = false;
				VR_DiagPrintf( "SELTAKE closed\n" );
			}
		}
	
		cyc_prev = cyc;
		click_prev = click;
	}
	if( vr.btn[VRA_MENU] && !vr.btn_prev[VRA_MENU] )
		Cbuf_AddText( "escape\n" );

	for( i = 0; i < 2; i++ )
	{
		XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };

		if( !vr.hand_space[i] )
			continue;

		if( XR_SUCCEEDED( xrLocateSpace( vr.hand_space[i], vr.stage_space,
			vr.frame_state.predictedDisplayTime, &loc )))
		{
			if(( loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT ) &&
			   ( loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT ))
				VR_ConvertPose( &loc.pose, &vr.hand_pose[i] );
			else
				vr.hand_pose[i].valid = false;
		}
	}

	// same, for the separate grip pose used to render held meshes
	for( i = 0; i < 2; i++ )
	{
		XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };

		if( !vr.grip_space[i] )
			continue;

		if( XR_SUCCEEDED( xrLocateSpace( vr.grip_space[i], vr.stage_space,
			vr.frame_state.predictedDisplayTime, &loc )))
		{
			if(( loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT ) &&
			   ( loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT ))
				VR_ConvertPose( &loc.pose, &vr.hand_grip_pose[i] );
			else
				vr.hand_grip_pose[i].valid = false;
		}
	}
}

/*
================
VR_GetMovement - stick -> HL move values
================
*/
void VR_GetMovement( float *forward, float *side )
{
	float dead = Q_max( 0.0f, vr_deadzone.value );
	float fx = vr.move_x, fy = vr.move_y;
	float mag = sqrt( fx * fx + fy * fy );

	if( forward ) *forward = 0.0f;
	if( side )    *side = 0.0f;

	if( !VR_IsActive() || !vr.input_ready )
		return;

	// While the teleport arc is up, the stick is aiming it rather than walking.
	// Leaving smooth movement live underneath would walk the player during
	// every aim, which is both wrong and the exact sensation teleport users
	// turned it on to avoid.
	if( VR_TeleportAiming( ))
		return;

	if( mag < dead )
		return;

	// rescale past the deadzone so control stays smooth from the edge of it
	if( mag > 0.0f && dead < 1.0f )
	{
		float s = ( mag - dead ) / ( 1.0f - dead ) / mag;
		fx *= s;
		fy *= s;
	}

	// OFF-HAND DIRECTED MOVEMENT, optional.
	//
	// By default the stick walks you relative to where you are LOOKING, which
	// is the obvious mapping and stays the default here. The alternative is to
	// walk relative to where your off hand points, which lets you keep moving
	// in one direction while looking somewhere else - useful for backing away
	// from something while watching it. Lambda1VR offers the same choice as
	// vr_walkdirection (L1VR_SurfaceView.c:374).
	//
	// Implemented as a rotation of the stick vector by the yaw difference
	// between the off hand and the head, so everything downstream - including
	// the weapon-aim counter-rotation in CL_CreateCmd - keeps working
	// unchanged.
	if( vr_walkdirection.value != 0.0f )
	{
		vec3_t hand_org, hand_ang;

		if( VR_GetHandWorld( VR_OffHand(), hand_org, hand_ang ))
		{
			float d = DEG2RAD( hand_ang[YAW] - ( vr.body_yaw + vr.hmd_pose.angles[YAW] ));
			float c = cos( d ), s = sin( d );
			float nx = fx * c - fy * s;
			float ny = fx * s + fy * c;

			fx = nx;
			fy = ny;
		}
	}

	if( forward ) *forward = bound( -1.0f, fy, 1.0f ) * vr_movespeed.value;
	if( side )    *side    = bound( -1.0f, fx, 1.0f ) * vr_movespeed.value;
}

/*
================
VR_UpdateTurn - right stick rotates the play space
================
*/
void VR_UpdateTurn( float frametime )
{
	float dead = Q_max( 0.05f, vr_deadzone.value );

	if( !VR_IsActive() || !vr.input_ready )
		return;

	// NOTE: this accumulates a DELTA rather than writing body_yaw directly.
	// VR_OverrideViewAngles recomputes body_yaw from the mod's yaw every frame
	// (to strip the head yaw we injected), so a direct write here would be
	// discarded one call later - which is exactly why turning did nothing.
	if( vr_snap_turn.value )
	{
		// discrete steps: much more comfortable for most people than smooth yaw
		// The grip modifier re-tasks this stick to weapon cycling, and it cannot
	// do both. Suppressed rather than blended: a stick flick meant as "next
	// weapon" that also spun the player would be worse than either.
	if( vr.btn[VRA_ATTACK2] )
		return;
	
	if( fabs( vr.turn_x ) < 0.6f )
		{
			vr.snap_pending = false;
			return;
		}
		if( vr.snap_pending )
			return;

		vr.snap_pending = true;
		vr.turn_delta -= ( vr.turn_x > 0.0f ) ? vr_snap_angle.value : -vr_snap_angle.value;
	}
	else
	{
		if( fabs( vr.turn_x ) < dead )
			return;
		vr.turn_delta -= vr.turn_x * vr_turnspeed.value * frametime;
	}
}

qboolean VR_GetButton( int btn )
{
	if( !VR_IsActive() || !vr.input_ready || btn < 0 || btn >= VRA_COUNT )
		return false;
	return vr.btn[btn];
}

/*
================
VR_PollEvents
================
*/
static void VR_PollEvents( void )
{
	XrEventDataBuffer ev = { XR_TYPE_EVENT_DATA_BUFFER };

	while( xrPollEvent( vr.instance, &ev ) == XR_SUCCESS )
	{
		if( ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED )
		{
			const XrEventDataSessionStateChanged *ss = (const XrEventDataSessionStateChanged *)&ev;

			vr.session_state = ss->state;
			Con_Printf( "VR: session state -> %s\n", VR_SessionStateName( ss->state ));
			VR_DiagPrintf( "t=%7.2f  SESSION STATE -> %s\n",
				host.realtime - vrdiag.session_start, VR_SessionStateName( ss->state ));

			switch( ss->state )
			{
			case XR_SESSION_STATE_READY:
			{
				XrSessionBeginInfo bi = { XR_TYPE_SESSION_BEGIN_INFO };

				bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
				if( XR_SUCCEEDED( xrBeginSession( vr.session, &bi )))
				{
					vr.running = true;
					Con_Printf( "VR: session running\n" );
				}
				break;
			}
			case XR_SESSION_STATE_STOPPING:
				vr.running = false;
				xrEndSession( vr.session );
				Con_Printf( "VR: session stopped\n" );
				break;
			case XR_SESSION_STATE_EXITING:
			case XR_SESSION_STATE_LOSS_PENDING:
				vr.running = false;
				Con_Printf( "VR: session exiting\n" );
				break;
			default:
				break;
			}
		}

		memset( &ev, 0, sizeof( ev ));
		ev.type = XR_TYPE_EVENT_DATA_BUFFER;
	}
}

/*
================
VR_BeginFrame
================
*/
/*
================
VR_CheckQuality

Rebuild the eye targets when the image quality settings change.

They are read when a swapchain is built, which happens once as the headset
session comes up - and that is BEFORE the config is executed, so a supersample
set in config.cfg or vrbinds.cfg was read as its compiled default and the
setting appeared to do nothing at all. Rebuilding on change fixes that no
matter when the value lands, and makes both settings adjustable in place
rather than only across a restart.

Called before the frame is waited on, which is the only point where no image
is acquired and the targets can safely be let go of.
================
*/
static void VR_CheckQuality( void )
{
	float ss = vr_supersample.value;
	int msaa = (int)vr_msaa.value;
	int i;

	// Clamped the same way VR_CreateSwapchain clamps them, or an
	// out-of-range value would never compare equal and rebuild every frame.
	if( ss < 0.5f ) ss = 0.5f;
	if( ss > 2.0f ) ss = 2.0f;
	if( msaa != 2 && msaa != 4 && msaa != 8 ) msaa = 0;

	if( ss == vr.applied_ss && msaa == vr.applied_msaa )
		return;

	Con_Printf( "VR: image quality changed, rebuilding eye targets\n" );

	for( i = 0; i < vr.eye_count; i++ )
	{
		VR_DestroySwapchain( i );

		if( !VR_CreateSwapchain( i ))
		{
			// A driver can refuse a size or sample count that looked fine.
			// Leaving half-built targets would black the headset out, so put
			// the settings back and rebuild what is known to have worked.
			int j;

			Con_Printf( S_ERROR "VR: %.2fx / %dx MSAA refused, reverting\n", ss, msaa );
			Cvar_SetValue( "vr_supersample", vr.applied_ss );
			Cvar_SetValue( "vr_msaa", (float)vr.applied_msaa );

			for( j = 0; j <= i; j++ )
			{
				VR_DestroySwapchain( j );
				VR_CreateSwapchain( j );
			}
			return;
		}
	}
}

qboolean VR_BeginFrame( void )
{
	XrFrameWaitInfo wi = { XR_TYPE_FRAME_WAIT_INFO };
	XrFrameBeginInfo bi = { XR_TYPE_FRAME_BEGIN_INFO };
	XrViewLocateInfo vli = { XR_TYPE_VIEW_LOCATE_INFO };
	XrViewState vs = { XR_TYPE_VIEW_STATE };
	XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
	uint32_t n = 0;

	vr.eyes_submitted = false;
	vr.eyes_this_frame = 0;

	if( !vr.available )
		return false;

	// The HMD may appear after startup; pick it up and create the session lazily.
	if( !vr.have_system )
	{
		if( !VR_AcquireSystem( ))
			return false;
	}

	if( !vr.session_ready )
	{
		if( !VR_InitSession( ))
			return false;
	}

	// NOTE: this must run unconditionally, NOT behind VR_IsActive(). vr.running is
	// set only by this poll, so gating the poll on vr.running would deadlock the
	// session in IDLE forever - it can never reach READY.
	VR_PollEvents();

	if( !vr.running )
		return false;

	VR_CheckQuality();

	memset( &vr.frame_state, 0, sizeof( vr.frame_state ));
	vr.frame_state.type = XR_TYPE_FRAME_STATE;

	if( XR_FAILED( xrWaitFrame( vr.session, &wi, &vr.frame_state )))
	{
		vrdiag.err_wait++;
		return false;
	}

	if( XR_FAILED( xrBeginFrame( vr.session, &bi )))
		return false;

	// From here on an XR frame is OPEN. Every path out of this function must be
	// matched by VR_EndFrame(), which the caller now always invokes - otherwise
	// xrWaitFrame blocks or errors on the following frame.
	vr.frame_started = true;

	if( !vr.frame_state.shouldRender )
	{
		vrdiag.frames_no_render++;
		return false;
	}

	// locate the eyes
	vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	vli.displayTime           = vr.frame_state.predictedDisplayTime;
	vli.space                 = vr.stage_space;

	if( XR_FAILED( xrLocateViews( vr.session, &vli, &vs, vr.eye_count, &n, vr.views )))
	{
		vrdiag.err_locate++;
		return false;
	}

	if( !( vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT ) ||
	    !( vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT ))
	{
		vrdiag.err_locate++;
		return false;
	}

	// HMD pose (centre of the two eyes) for gameplay use
	if( XR_SUCCEEDED( xrLocateSpace( vr.view_space, vr.stage_space,
		vr.frame_state.predictedDisplayTime, &loc )))
	{
		if(( loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT ) &&
		   ( loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT ))
			VR_ConvertPose( &loc.pose, &vr.hmd_pose );
	}

	// controller state, sampled against this frame's predicted display time
	VR_SyncInput();

	// Menu anchor and pointer. Here rather than in CL_CreateCmd because this
	// is the one per-frame point common to the world path and the menu-only
	// path - the main menu never reaches CL_CreateCmd at all.
	VR_UpdateDeath();
	VR_UpdateShoulderMelee();
	VR_UpdateReload();
	VR_UpdateAction();
	VR_UpdateMenu2D();

	VR_DiagSample();

	return true;
}

/*
================
VR_BeginEye
================
*/
qboolean VR_BeginEye( int eye, ref_viewpass_t *rvp )
{
	vr_swapchain_t *sc;
	XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
	XrSwapchainImageWaitInfo wi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };

	if( !VR_IsActive() || eye < 0 || eye >= vr.eye_count )
		return false;

	// Which eye the 2D pass is about to composite into - see VR_Begin2D.
	vr.cur_eye = eye;

	sc = &vr.swapchains[eye];

	if( XR_FAILED( xrAcquireSwapchainImage( sc->handle, &ai, &sc->acquired_index )))
		return false;

	wi.timeout = XR_INFINITE_DURATION;
	if( XR_FAILED( xrWaitSwapchainImage( sc->handle, &wi )))
	{
		// RELEASE what was just acquired. The image is only ever released in
		// VR_EndEye, and the caller skips that entirely when this returns
		// false - so every failure here permanently consumed one image from
		// the runtime's pool. A handful of them and the pool is empty and the
		// eye stops rendering for the rest of the session, with no error to
		// show for it.
		XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };

		xrReleaseSwapchainImage( sc->handle, &ri );
		return false;
	}

	// Take a depth image too, when the compositor is going to read one.
	//
	// Its own pool, its own index - there is no promise the colour and depth
	// images line up, so the pairing is made here each frame rather than
	// baked into the FBOs at creation.
	//
	// Every failure below leaves depth_ready false and the frame renders
	// exactly as it would without the extension. Losing depth costs
	// reprojection accuracy; refusing the frame would cost the frame.
	sc->depth_ready = false;

	if( sc->depth_handle )
	{
		XrSwapchainImageAcquireInfo dai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
		XrSwapchainImageWaitInfo dwi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };

		if( XR_SUCCEEDED( xrAcquireSwapchainImage( sc->depth_handle, &dai,
			&sc->depth_acquired_index )))
		{
			dwi.timeout = XR_INFINITE_DURATION;

			if( XR_SUCCEEDED( xrWaitSwapchainImage( sc->depth_handle, &dwi )))
			{
				sc->depth_ready = true;
			}
			else
			{
				// Hand it straight back. An image acquired and never released is
				// gone from the pool for good, and a few of those empty it.
				XrSwapchainImageReleaseInfo dri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };

				xrReleaseSwapchainImage( sc->depth_handle, &dri );
			}
		}
	}

	// Point this frame's colour FBO at this frame's depth image.
	if( sc->depth_ready )
	{
		vrgl.BindFramebuffer( GL_FRAMEBUFFER_EXT, sc->fbos[sc->acquired_index] );
		vrgl.FramebufferTexture2D( GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT,
			GL_TEXTURE_2D_T, sc->depth_images[sc->depth_acquired_index].image, 0 );
	}

	// ref_gl renders into whatever framebuffer is bound and never rebinds one.
	// With MSAA that is the multisampled target, resolved across in VR_EndEye.
	if( sc->samples )
		vrgl.BindFramebuffer( GL_FRAMEBUFFER_EXT, sc->msaa_fbo );
	else
		vrgl.BindFramebuffer( GL_FRAMEBUFFER_EXT, sc->fbos[sc->acquired_index] );

	// fill the viewpass from the OpenXR eye pose
	if( rvp )
	{
		const XrView *v = &vr.views[eye];
		vr_pose_t pose;

		VR_ConvertPose( &v->pose, &pose );

		rvp->viewport[0] = 0;
		rvp->viewport[1] = 0;
		rvp->viewport[2] = sc->width;
		rvp->viewport[3] = sc->height;

		// OpenXR poses live in the play space, which knows nothing about where the
		// player stands in the map or which way their body faces. Compose the two.
		//
		// The eye offset from head centre (essentially the IPD half-offset) is
		// expressed in play-space axes, so it must be rotated by the game's yaw
		// before being added to the world anchor - otherwise the eyes separate
		// along the wrong axis as soon as the player faces a different direction.
		{
			vec3_t rel;
			float yaw_off = vr_compose_yaw.value ? vr.world_yaw : 0.0f;
			float s, c;
			const float *hmd_ref = vr.hmd_origin_at_sync_valid ? vr.hmd_origin_at_sync : vr.hmd_pose.origin;

			VectorSubtract( pose.origin, hmd_ref, rel );
			SinCos( DEG2RAD( yaw_off ), &s, &c );

			rvp->vieworigin[0] = vr.world_origin[0] + ( rel[0] * c - rel[1] * s );
			rvp->vieworigin[1] = vr.world_origin[1] + ( rel[0] * s + rel[1] * c );
			// vr_height_offset shifts the whole play space vertically. Rooms
			// with a raised floor, a seated player, or a tracking origin that
			// simply reads low all show up as standing in the ground or
			// floating above it, and none of that is fixable from the game
			// side. Applied to the EYE only, so shot origins and hand poses,
			// which are anchored to the same reference, move with it.
			// Seated play sits the real head well below standing eye height, and the
			// view follows it down. vr_seated_lift puts the eye back where a standing
			// player would have it, so the world is the right size from a chair.
			rvp->vieworigin[2] = ( vr.smooth_z_valid ? vr.smooth_z : vr.world_origin[2] ) + rel[2]
				+ vr_height_offset.value
				+ (( vr_seated.value != 0.0f ) ? vr_seated_lift.value : 0.0f );

			// Head owns pitch and roll outright - letting the game pitch or roll a
			// VR player's view is a reliable way to make them sick. Yaw is additive
			// so scripted turns (the intro tram, trigger_camera, vehicles) still
			// carry the player around while they remain free to look about.
			rvp->viewangles[PITCH] = pose.angles[PITCH];
			rvp->viewangles[YAW]   = anglemod( pose.angles[YAW] + yaw_off );
			rvp->viewangles[ROLL]  = pose.angles[ROLL];
		}

		// fov_x/fov_y still feed the CULLING frustum (R_SetupFrustum ->
		// GL_FrustumInitProj), which is symmetric and separate from the
		// projection matrix. A symmetric frustum built from the true asymmetric
		// angles could clip geometry off the wider side, so publish a
		// CONSERVATIVE symmetric FOV: twice the larger half-angle, which is
		// guaranteed to contain the real frustum. Slightly over-inclusive (draws
		// a little extra) is safe; under-inclusive would pop geometry at edges.
		{
			float half_x = Q_max( fabs( v->fov.angleLeft ), fabs( v->fov.angleRight ));
			float half_y = Q_max( fabs( v->fov.angleUp   ), fabs( v->fov.angleDown ));

			rvp->fov_x = RAD2DEG( 2.0f * half_x );
			rvp->fov_y = RAD2DEG( 2.0f * half_y );
		}

		// True asymmetric frustum - what the renderer should actually use.
		rvp->vr_active     = true;
		rvp->vr_eye        = eye;
		rvp->vr_tan_left   = tanf( v->fov.angleLeft );
		rvp->vr_tan_right  = tanf( v->fov.angleRight );
		rvp->vr_tan_up     = tanf( v->fov.angleUp );
		rvp->vr_tan_down   = tanf( v->fov.angleDown );
	}

	return true;
}

/*
================
VR_EndEye
================
*/
void VR_EndEye( int eye )
{
	vr_swapchain_t *sc;
	XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };

	if( !VR_IsActive() || eye < 0 || eye >= vr.eye_count )
		return;

	sc = &vr.swapchains[eye];

	// RESOLVE THE MULTISAMPLED EYE into the swapchain image.
	//
	// Colour is filtered because that is the whole point of drawing it
	// multisampled. Depth is not: a depth value is a position, and averaging
	// two of them invents a surface that was never there. Nearest keeps a
	// real sample, which is what the compositor needs to reproject against.
	//
	// Done before the mirror blit so the desktop shows the resolved image
	// rather than a raw multisample buffer it cannot read.
	if( sc->samples )
	{
		vrgl.BindFramebuffer( GL_READ_FRAMEBUFFER_T, sc->msaa_fbo );
		vrgl.BindFramebuffer( GL_DRAW_FRAMEBUFFER_T, sc->fbos[sc->acquired_index] );

		vrgl.BlitFramebuffer( 0, 0, (GLint_t)sc->width, (GLint_t)sc->height,
			0, 0, (GLint_t)sc->width, (GLint_t)sc->height,
			GL_COLOR_BUFFER_BIT_T, GL_LINEAR_T );

		if( sc->depth_ready )
			vrgl.BlitFramebuffer( 0, 0, (GLint_t)sc->width, (GLint_t)sc->height,
				0, 0, (GLint_t)sc->width, (GLint_t)sc->height,
				GL_DEPTH_BUFFER_BIT_T, GL_NEAREST_T );
	}

	// Desktop mirror: copy the left eye into the window before releasing the
	// swapchain image back to the runtime.
	//
	// Why this is needed at all: in VR both eyes render into OpenXR swapchain
	// FBOs, so NOTHING is ever drawn to the window's back buffer - but
	// R_EndFrame still swaps it every frame. Combined with gl_clear
	// defaulting to "0" (the flatscreen scene normally overwrites every
	// pixel, so clearing is wasted work there), the window ends up swapping
	// in a buffer that was never written: undefined GPU memory, which shows
	// as scrambled garbage rather than a black screen.
	//
	// Blitting one eye also makes the game visible to anyone not wearing the
	// headset, and lets rendering be verified without putting it on.
	// Not on a menu-only frame. There the desktop window already has the real
	// menu drawn into it by V_PostRender, so blitting a cleared eye over the top
	// just fights it - the two alternate every frame and the window strobes grey
	// at framerate. The headset still gets the eye; only the mirror is skipped.
	if( eye == 0 && vr_mirror.value && vrgl.BlitFramebuffer && !vr.menu_frame )
	{
		int ww = refState.width;
		int wh = refState.height;

		if( ww > 0 && wh > 0 )
		{
			// Fit preserving aspect (letterbox) rather than stretching - an eye
			// is roughly square (2496x2688 here) while the window is
			// widescreen, so a straight stretch distorts badly.
			float src_aspect = (float)sc->width / (float)sc->height;
			int dw = ww, dh = (int)( ww / src_aspect );
			int dx, dy;

			if( dh > wh )
			{
				dh = wh;
				dw = (int)( wh * src_aspect );
			}
			dx = ( ww - dw ) / 2;
			dy = ( wh - dh ) / 2;

			vrgl.BindFramebuffer( GL_READ_FRAMEBUFFER_T, sc->fbos[sc->acquired_index] );
			vrgl.BindFramebuffer( GL_DRAW_FRAMEBUFFER_T, 0 );
			vrgl.BlitFramebuffer( 0, 0, (GLint_t)sc->width, (GLint_t)sc->height,
				dx, dy, dx + dw, dy + dh,
				GL_COLOR_BUFFER_BIT_T, GL_LINEAR_T );
		}
	}

	vrgl.BindFramebuffer( GL_FRAMEBUFFER_EXT, 0 );
	xrReleaseSwapchainImage( sc->handle, &ri );

	if( sc->depth_ready )
	{
		XrSwapchainImageReleaseInfo dri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };

		xrReleaseSwapchainImage( sc->depth_handle, &dri );
	}

	// record the layer view for submission
	vr.proj_views[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
	vr.proj_views[eye].pose = vr.views[eye].pose;
	vr.proj_views[eye].fov  = vr.views[eye].fov;
	vr.proj_views[eye].subImage.swapchain = sc->handle;
	vr.proj_views[eye].subImage.imageRect.offset.x = 0;
	vr.proj_views[eye].subImage.imageRect.offset.y = 0;
	vr.proj_views[eye].subImage.imageRect.extent.width  = sc->width;
	vr.proj_views[eye].subImage.imageRect.extent.height = sc->height;
	vr.proj_views[eye].subImage.imageArrayIndex = 0;
	vr.proj_views[eye].next = NULL;

	// TELL THE COMPOSITOR WHAT THE DEPTH MEANS.
	//
	// The buffer alone is unreadable - the values are whatever the
	// projection encoded, so the range that produced them has to travel
	// with it. The renderer publishes that range rather than this file
	// guessing, because it is the renderer that builds the projection and a
	// guess here would silently drift the moment it changed there.
	//
	// Skipped rather than faked if the range looks wrong - a menu frame that
	// never drew a 3D view leaves it unset, and handing the runtime a bad
	// range is worse than handing it none.
	if( sc->depth_ready && refState.zNear > 0.0f && refState.zFar > refState.zNear )
	{
		XrCompositionLayerDepthInfoKHR *di = &vr.depth_info[eye];

		di->type = XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR;
		di->next = NULL;
		di->subImage.swapchain = sc->depth_handle;
		di->subImage.imageRect = vr.proj_views[eye].subImage.imageRect;
		di->subImage.imageArrayIndex = 0;

		// The GL depth range, untouched.
		di->minDepth = 0.0f;
		di->maxDepth = 1.0f;

		// OpenXR works in metres and the game does not.
		di->nearZ = refState.zNear / VR_UNITS_PER_METER;
		di->farZ  = refState.zFar / VR_UNITS_PER_METER;

		vr.proj_views[eye].next = di;
	}

	vr.eyes_submitted = true;
	vr.eyes_this_frame++;
}

/*
================
VR_EndFrame
================
*/
void VR_EndFrame( void )
{
	XrCompositionLayerProjection layer = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
	const XrCompositionLayerBaseHeader *layers[1];
	XrFrameEndInfo ei = { XR_TYPE_FRAME_END_INFO };

	// Safe to call unconditionally - no-ops unless an XR frame is actually open.
	if( !vr.available || !vr.session_ready || !vr.frame_started )
		return;

	layer.space     = vr.stage_space;
	layer.viewCount = vr.eye_count;
	layer.views     = vr.proj_views;
	layers[0] = (const XrCompositionLayerBaseHeader *)&layer;

	ei.displayTime          = vr.frame_state.predictedDisplayTime;
	ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;

	// Only submit a layer if we really rendered the eyes. Submitting a projection
	// layer referencing swapchain images that were never written is a protocol
	// error and some runtimes will drop the session over it.
	//
	// ALL eyes, not merely one. layer.viewCount is always vr.eye_count, but
	// proj_views[] is filled per eye in VR_EndEye - so a frame where eye 0
	// succeeded and eye 1 failed submitted eye 1's entry holding either last
	// frame's swapchain or, on the very first frame, XR_NULL_HANDLE. That is
	// precisely the protocol error this check exists to prevent, and the
	// boolean could not see it.
	if( vr.frame_state.shouldRender && vr.eyes_this_frame >= vr.eye_count )
	{
		ei.layerCount = 1;
		ei.layers     = layers;
		vr.frames_submitted++;
	}
	else
	{
		ei.layerCount = 0;
		ei.layers     = NULL;
	}

	xrEndFrame( vr.session, &ei );
	vr.frame_started = false;
	vr.eyes_submitted = false;
	vr.eyes_this_frame = 0;
}

/*
================
VR_Shutdown
================
*/
void VR_Shutdown( void )
{
	int i;

	// atexit registers this too, so it can run twice - make that harmless.
	if( !vr.available && vr.instance == XR_NULL_HANDLE && !vr.instance_mutex )
		return;

	for( i = 0; i < vr.eye_count; i++ )
	{
		vr_swapchain_t *sc = &vr.swapchains[i];

		if( sc->fbos && vrgl.loaded )
			vrgl.DeleteFramebuffers( sc->image_count, sc->fbos );
		if( sc->depth_rb && vrgl.loaded )
			vrgl.DeleteRenderbuffers( 1, &sc->depth_rb );
		if( sc->handle )
			xrDestroySwapchain( sc->handle );
		if( sc->images ) Mem_Free( sc->images );
		if( sc->fbos )   Mem_Free( sc->fbos );
	}

	if( vr.view_space )  xrDestroySpace( vr.view_space );
	if( vr.stage_space ) xrDestroySpace( vr.stage_space );
	if( vr.session != XR_NULL_HANDLE )  xrDestroySession( vr.session );
	if( vr.instance != XR_NULL_HANDLE ) xrDestroyInstance( vr.instance );

	memset( &vr, 0, sizeof( vr ));
	Con_Printf( "VR: shut down\n" );
}

/*
================
VR_SetMenuFrame

Marks the current frame as the menu-only path (no world rendered). Used to
suppress the desktop mirror blit, which would otherwise fight V_PostRender for
the window - see VR_EndEye.
================
*/
void VR_SetMenuFrame( qboolean on )
{
	vr.menu_frame = on;
}

#else // !XASH_WIN32 || XASH_DEDICATED || !XASH_OPENXR

// Stubs so callers need no #ifdef. VR is Win32-client-only for now.
CVAR_DEFINE_AUTO( vr_enable, "0", FCVAR_ARCHIVE, "enable OpenXR VR rendering" );

qboolean VR_Init( void )        { return false; }
qboolean VR_InitSession( void ) { return false; }
void     VR_Shutdown( void )    { }
qboolean VR_IsActive( void )    { return false; }
qboolean VR_IsAvailable( void ) { return false; }
qboolean VR_BeginFrame( void )  { return false; }
int      VR_GetEyeCount( void ) { return 0; }
qboolean VR_BeginEye( int eye, ref_viewpass_t *rvp ) { return false; }
void     VR_EndEye( int eye )   { }
void     VR_EndFrame( void )    { }
void     VR_GetRequiredGLVersion( int *major, int *minor ) { if( major ) *major = 0; if( minor ) *minor = 0; }

static const vr_pose_t vr_null_pose;
const vr_pose_t *VR_GetHMDPose( void )      { return &vr_null_pose; }
const vr_pose_t *VR_GetHandPose( int hand ) { return &vr_null_pose; }


// Everything the header declares must have a definition here too, or a build
// without OpenXR links against nothing. The list above was written when the VR
// surface was thirteen functions; it is now closer to fifty, and every one
// added since was missing - so this branch had quietly stopped compiling long
// ago and nobody noticed, because the only builds anyone made had OpenXR.
void     VR_SetWorldReference( const vec3_t origin ) { }
void     VR_OverrideViewAngles( vec3_t angles ) { }
float    VR_GetBodyYaw( void ) { return 0.0f; }
qboolean VR_SelectOpen( void ) { return false; }
void     VR_GetMovement( float *forward, float *side ) { if( forward ) *forward = 0.0f; if( side ) *side = 0.0f; }
void     VR_UpdateTurn( float frametime ) { }
qboolean VR_GetButton( int btn ) { return false; }
qboolean VR_GetHandWorld( int hand, vec3_t out_org, vec3_t out_ang ) { return false; }
qboolean VR_GetListener( vec3_t out_org, vec3_t out_ang ) { return false; }
void     VR_ObserveUserMessage( const char *name, int size, const void *buf ) { }
void     VR_UpdateTeleport( void ) { }
qboolean VR_TeleportAiming( void ) { return false; }
qboolean VR_ConsumeTeleport( vec3_t out_dest ) { return false; }
void     VR_DrawHands( qboolean draw_right ) { }
qboolean VR_AlignModelToFireRay( vec3_t ang ) { return false; }
void     VR_ResetModelAlign( void ) { }
void     VR_CalibrateWeaponAngles( vec3_t ang ) { }
qboolean VR_ApplyTwoHandedAim( const vec3_t dom_org, vec3_t ang ) { return false; }
qboolean VR_HoldingMelee( void ) { return false; }
void     VR_UpdateFireRay( void ) { }
qboolean VR_GetFireRay( vec3_t out_org, vec3_t out_ang ) { return false; }
qboolean VR_WeaponOriginActive( void ) { return false; }
int      VR_DominantHand( void ) { return 1; }
int      VR_OffHand( void ) { return 0; }
qboolean VR_GetPhysicalCrouch( void ) { return false; }
qboolean VR_OnLadder( void ) { return false; }
qboolean VR_LadderHands( void ) { return false; }
float    VR_GetLadderClimb( void ) { return 0.0f; }
float    VR_GetLadderMove( void ) { return 0.0f; }
qboolean VR_GetLadderDir( vec3_t out ) { return false; }
qboolean VR_LadderHandsOnly( void ) { return false; }
void     VR_GetRoomScaleMove( float view_yaw, float *forward, float *side ) { if( forward ) *forward = 0.0f; if( side ) *side = 0.0f; }
qboolean VR_GetTouchContact( void ) { return false; }
qboolean VR_GetUseSource( vec3_t out_org, vec3_t out_ang ) { return false; }
void     VR_DiagModelAngles( const vec3_t raw, const vec3_t after_cal, const vec3_t after_align, const vec3_t final ) { }
void     VR_SetMenuFrame( qboolean on ) { }
void     VR_SetFirePhase( int phase, const float *eye, int buttons ) { }
void     VR_CheckTraceOutsideWindow( const float *start ) { }
qboolean VR_AimFromWeapon( void ) { return false; }
qboolean VR_DualWieldActive( void ) { return false; }
qboolean VR_GetOffhandFire( vec3_t out_org, vec3_t out_dir ) { return false; }
void     VR_DrawOffhandWeapon( void ) { }
void     VR_DrawHeldRound( void ) { }
qboolean VR_GetAimAngles( vec3_t out_ang ) { return false; }
qboolean VR_GetWeaponAim( vec3_t out_org, vec3_t out_ang ) { return false; }
void     VR_DrawOverlays( void ) { }
void     VR_Haptic( int hand, float duration, float frequency, float amplitude ) { }
qboolean VR_GetMeleeAttack( void ) { return false; }
qboolean VR_GetReloadCmd( void ) { return false; }
qboolean VR_ActionBlocked( void ) { return false; }
int      VR_GetActionImpulse( void ) { return 0; }
void     VR_HoldViewModel( void ) { }
qboolean VR_GetFlashlightSource( vec3_t out_org, vec3_t out_fwd ) { return false; }
void     VR_Begin2D( void ) { }
void     VR_End2D( void ) { }

#endif
