/*
vr_openxr.h - OpenXR VR support for Xash3D FWGS (PCVR fork)
Copyright (C) 2026

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

--------------------------------------------------------------------------
DESIGN NOTES

Where this lives and why:

  The VR layer sits in the ENGINE (xash.dll), not in the renderer module and
  not in any mod's client.dll. That placement is deliberate and is what makes
  VR work across arbitrary GoldSrc mods:

    engine/client/cl_view.c : V_RenderView() runs a multi-view loop and calls
      clgame.dllFuncs.pfnCalcRefdef( &rp )   <-- THE MOD decides where the camera is
      V_GetRefParams( &rp, &rvp )
      GL_RenderFrame( &rvp )                 <-- engine hands one view to ref_gl

  We do not touch pfnCalcRefdef. We let the mod compute its normal view, then
  override the resulting ref_viewpass_t with the per-eye pose from OpenXR before
  handing it to the renderer. Because that override is engine code, it applies
  to Half-Life, TFC, Ricochet, They Hunger, ... with no per-mod work.

  ref_gl never binds a framebuffer object anywhere in its .c files (it renders to
  the default framebuffer), so the engine can bind an FBO wrapping the OpenXR
  swapchain image and ref_gl will render into it without knowing.

Runtime facts verified on this machine (see PCVR_LOG.md FINDING 007-009):
  - Runtime VirtualDesktopXR 1.0.10, XR_KHR_opengl_enable v10 present.
  - Must request apiVersion XR_API_VERSION_1_0; 1.1 is rejected with -4.
  - Runtime requires OpenGL >= 4.0, so the GL context must be a 4.x
    COMPATIBILITY profile (legacy GoldSrc GL idioms must remain valid).
*/

#ifndef VR_OPENXR_H
#define VR_OPENXR_H

#include "xash3d_types.h"
#include "ref_params.h"

#define VR_EYE_LEFT   0
#define VR_EYE_RIGHT  1
#define VR_MAX_EYES   2

/*
 * GoldSrc uses ~1 unit == 1 inch, Z up, X forward, Y left.
 * OpenXR uses meters, Y up, -Z forward, X right.
 */
#define VR_UNITS_PER_METER  39.37f

typedef struct vr_pose_s
{
	vec3_t   origin;   // in HL units, HL axes, relative to the tracking space origin
	vec3_t   angles;   // HL euler degrees (pitch, yaw, roll)
	qboolean valid;
} vr_pose_t;

//
// lifecycle
//

// Create the OpenXR instance/system. Called early, before the GL context exists,
// so that GL context creation can honour the runtime's requirements.
// Returns false if VR is unavailable or disabled; the engine then runs flatscreen.
qboolean VR_Init( void );

// Create the OpenXR session. Must be called AFTER the GL context exists,
// because the session needs the Win32 HDC/HGLRC graphics binding.
qboolean VR_InitSession( void );

void     VR_Shutdown( void );

// True once a session is running and frames may be submitted.
qboolean VR_IsActive( void );

// True if VR_Init succeeded, even if the session isn't running yet.
qboolean VR_IsAvailable( void );

//
// per-frame
//

// xrWaitFrame + xrBeginFrame + xrLocateViews.
// Returns false if this frame should not be rendered (session idle, etc).
qboolean VR_BeginFrame( void );

// Number of views to render this frame (2 for stereo).
int      VR_GetEyeCount( void );

// Anchor the play space at this world position for the current frame.
// (The rotation used is the body yaw captured by VR_OverrideViewAngles.)
void     VR_SetWorldReference( const vec3_t origin );

// Replace the view angles the mod derived from mouse input with the VR ones.
//
// This is what makes the player actually able to MOVE sensibly. cl.viewangles
// drives movement direction and weapon aim, so if it stays mouse-controlled
// while only the rendered view follows the head, WASD sends the player wherever
// the mouse happens to point rather than where they are looking.
//
// The yaw the mod computed is treated as BODY facing (so mouse/stick turning
// still works and can rotate the play space); head yaw is added on top:
//   yaw   = body yaw + HMD yaw
//   pitch = HMD only
//   roll  = HMD only
// Call after the mod's CL_CreateMove, passing the usercmd's viewangles.
void     VR_OverrideViewAngles( vec3_t angles );

// Body/play-space yaw in world space, as last captured.
float    VR_GetBodyYaw( void );

// True while the weapon select HUD is up. Melee stands down while it is: fire
// is the select confirm and swing-to-hit raises the same IN_ATTACK bit.
qboolean VR_SelectOpen( void );

//
// controller input
//

// Must stay in step with vr_action_id_t in vr_openxr.c.
#define VR_BTN_JUMP        2
#define VR_BTN_CROUCH      3
#define VR_BTN_ATTACK      4
#define VR_BTN_ATTACK2     5
#define VR_BTN_USE         6
#define VR_BTN_RELOAD      7
#define VR_BTN_FLASHLIGHT  8
#define VR_BTN_NEXTWEAP    9
#define VR_BTN_PREVWEAP    10
#define VR_BTN_MENU        11
#define VR_BTN_OFFGRIP     12	// off-hand grip: grab / two-hand a weapon

// Thumbstick locomotion, already deadzoned and scaled to HL move units.
// Values are relative to the current view direction, matching how the engine
// interprets usercmd forwardmove/sidemove.
void     VR_GetMovement( float *forward, float *side );

// Apply this frame's turn input to the play-space yaw (snap or smooth).
void     VR_UpdateTurn( float frametime );

qboolean VR_GetButton( int btn );

// Controller pose mapped into game world space, using the same anchor and
// rotation the eyes use so hands and view agree. hand: 0 = left, 1 = right.
// Returns false if that controller is not currently tracked.
qboolean VR_GetHandWorld( int hand, vec3_t out_org, vec3_t out_ang );

// World-space head pose for audio. The stereo loop must feed this to
// S_UpdateFrame rather than the mod's flat camera, or head rotation,
// room-scale walking and physical crouch are all inaudible.
qboolean VR_GetListener( vec3_t out_org, vec3_t out_ang );

// Draws a bare-hand model at each tracked controller, client-side only (never
// networked, works identically regardless of which mod is loaded). Call once
// per frame, not per eye. draw_right should be false when the weapon
// viewmodel is already being drawn for the right hand this frame, so the gun
// and the bare hand are never both shown at once.
void     VR_DrawHands( qboolean draw_right );

// Rotate the drawn weapon so its barrel lies on the actual firing line.
// Call AFTER VR_CalibrateWeaponAngles, before pre-negating pitch.
// Returns true if it applied a MEASURED correction. When false the mesh had no
// usable attachment, and the caller should fall back to the tuned rest-pose
// constant (VR_CalibrateWeaponAngles) instead.
qboolean VR_AlignModelToFireRay( vec3_t ang );

// Drop the accumulated model-alignment correction. Call when the two-handed
// hold takes over, since that path sets angles absolutely and leaves no
// residual for the integrator to close.
void     VR_ResetModelAlign( void );

// Applies the weapon viewmodel's rest-pose correction (vr_weapon_*_offset).
// Call on the PHYSICAL tracked angles, BEFORE pre-negating pitch.
void     VR_CalibrateWeaponAngles( vec3_t ang );

// Two-handed stabilisation: aim along the hand-to-hand vector when the off
// hand is up at the weapon. Call BEFORE VR_CalibrateWeaponAngles.
// Returns true if it engaged this frame.
qboolean VR_ApplyTwoHandedAim( const vec3_t dom_org, vec3_t ang );

// True when the equipped viewmodel is a melee weapon (crowbar and friends).
qboolean VR_HoldingMelee( void );

// Compute the frame's fire ray once (call early in CL_CreateCmd). Every
// consumer then reads it via VR_GetFireRay/VR_GetWeaponAim so the laser and
// the actual shot can never disagree.
void     VR_UpdateFireRay( void );
qboolean VR_GetFireRay( vec3_t out_org, vec3_t out_ang );

// True when the shot ORIGIN should be moved to the muzzle.
qboolean VR_WeaponOriginActive( void );

// Which controller holds the weapon, and which is free. 0 = left, 1 = right.
// Follows vr_lefthand, so left-handed play is a cvar rather than a fork.
int      VR_DominantHand( void );
int      VR_OffHand( void );

// Room-scale. True while the player is physically ducked below
// vr_crouch_ratio of their calibrated standing height (vr_height).
qboolean VR_GetPhysicalCrouch( void );

// Touch to interact. Returns the off hand's pose so the mod's own PlayerUse()
// search can be run from your fingertips instead of your face - see
// VR_GetUseSource in vr_openxr.c for why no per-mod knowledge is needed.
// True while the off hand is physically in contact with something. Raise
// IN_USE from this and the mod's own PlayerUse() decides whether anything is
// actually usable there - no button press, no per-mod knowledge.
// Traces the drawn weapon angles through each stage into vr_diag.log, sampled.
void     VR_DiagModelAngles( const vec3_t raw, const vec3_t after_cal, const vec3_t after_align, const vec3_t final );
qboolean VR_GetTouchContact( void );
qboolean VR_GetUseSource( vec3_t out_org, vec3_t out_ang );

// Room-scale. Walking in your room walks in the game: returns the movement
// needed for the player entity to chase your real physical position, so the
// body collides with the world instead of the view sliding through it.
// view_yaw must be the yaw the usercmd will actually carry, since
// forwardmove/sidemove are interpreted along it.
void     VR_GetRoomScaleMove( float view_yaw, float *forward, float *side );

// Teleport locomotion, off by default (vr_teleport). Built as a MODE on the
// existing movement stick rather than a new binding, so it needs no changes to
// any interaction profile: push the stick to raise an arc from the off hand,
// release to commit.
//
// VR_UpdateTeleport runs once per client frame from CL_CreateCmd.
// VR_TeleportAiming lets locomotion suppress itself while the arc is up.
// VR_ConsumeTeleport is the SERVER side - it returns true exactly once per
// committed teleport, and is read directly out of client state the same way
// VR_GetWeaponAim already is, so nothing is added to the network protocol.
// Observe a mod user message. Non-destructive - the mod still receives it.
// Used to learn weapon names from WeaponList/CurWeapon, so a weapon can be
// switched to directly by name instead of cycled to.
void     VR_ObserveUserMessage( const char *name, int size, const void *buf );

void     VR_UpdateTeleport( void );
qboolean VR_TeleportAiming( void );
qboolean VR_ConsumeTeleport( vec3_t out_dest );

// Hand-over-hand ladder climbing, returned as a usercmd upmove contribution.
// 0 when not climbing. Ladder movement lives in pm_shared (engine code) and is
// driven by upmove, so this needs nothing from the mod.
// True while standing on/at a ladder brush (CONTENTS_LADDER). Engine-side, so
// no mod involvement - see VR_GetLadderMove for why that matters.
qboolean VR_OnLadder( void );
float    VR_GetLadderMove( void );

// True while climbing a ladder by hand: the weapon is stowed, firing is
// suppressed and the stick cannot climb, so the hands are what move you.
qboolean VR_LadderHands( void );

// Climb rate for this frame in units/sec, already computed. Read by the
// server; recomputing it would consume the same hand movement twice.
float    VR_GetLadderClimb( void );
qboolean VR_GetLadderDir( vec3_t out );
qboolean VR_LadderHandsOnly( void );

// VR akimbo. The off hand fires the equipped weapon a second time on its own
// cooldown. The engine only supplies WHERE that shot comes from, via
// pev->vuser1/vuser2; the second shot itself is the game DLL's
// (CBasePlayer::DualWieldPostFrame). Needs our hl.dll - the one VR feature
// that is not mod-agnostic, so it stays off by default.
qboolean VR_DualWieldActive( void );
qboolean VR_GetOffhandFire( vec3_t out_org, vec3_t out_dir );

// Draws the second gun (a mirrored copy of the viewmodel) at the off hand.
// Client-side only, nothing networked. Call alongside VR_DrawHands.
void     VR_DrawOffhandWeapon( void );

// Diagnostic only, no behaviour change. SV_RunCmd tags which game-DLL call it is
// currently inside; VR_CheckTraceOutsideWindow, called from pfnTraceLine, logs a
// shot leaving the player's eye during PreThink - the one place a mod could fire
// that the PostThink view_ofs substitution does not cover. Gated on vr_debug.
#define VR_FIRE_PHASE_NONE      0	// not inside the game DLL's per-command calls
#define VR_FIRE_PHASE_PRETHINK  1	// inside pfnPlayerPreThink - NOT covered
#define VR_FIRE_PHASE_POSTTHINK 2	// inside the substituted window - covered

// Marks a menu-only frame, so the desktop mirror does not fight V_PostRender.
void     VR_SetMenuFrame( qboolean on );
void     VR_SetFirePhase( int phase, const float *eye, int buttons );
void     VR_CheckTraceOutsideWindow( const float *start );

// True when firing should follow the weapon rather than the head.
qboolean VR_AimFromWeapon( void );

// Angles for the usercmd so the shot LANDS where the weapon points, given
// the mod fires from the player's eye. See the implementation for why the
// origin itself cannot be moved from engine code.
qboolean VR_GetAimAngles( vec3_t out_ang );

// Where the weapon actually points (raw aim pose + two-handed stabilisation,
// WITHOUT the cosmetic mesh correction). Used for firing and the laser.
qboolean VR_GetWeaponAim( vec3_t out_org, vec3_t out_ang );

// World-space VR overlays (laser sight, grenade arc). Called from
// pfnDrawNormalTriangles during the 3D pass.
void     VR_DrawOverlays( void );

// Buzz a controller. hand: 0 = left, 1 = right. duration seconds,
// frequency Hz (0 = runtime default), amplitude 0..1.
void     VR_Haptic( int hand, float duration, float frequency, float amplitude );

// True on the frame a melee swing should count as an attack. Only fires
// while a melee weapon is equipped.
qboolean VR_GetMeleeAttack( void );
qboolean VR_GetReloadCmd( void );
qboolean VR_ActionBlocked( void );

// Off-hand flashlight source. Returns false to use the stock head mount.
qboolean VR_GetFlashlightSource( vec3_t out_org, vec3_t out_fwd );

// Constrain 2D drawing to a readable rect inside the current eye texture.
// The engine's 2D pass sets up an ortho for the WINDOW, which would otherwise
// splatter the HUD across the full eye at the wrong aspect. Call after
// R_Set2DMode(true) while an eye FBO is bound; VR_End2D restores the viewport.
void     VR_Begin2D( void );
void     VR_End2D( void );

// Acquire this eye's swapchain image and bind an FBO around it, then fill in
// rvp (viewport, vieworigin, viewangles, fov, asymmetric frustum tangents).
// The caller then invokes the normal GL_RenderFrame( rvp ).
qboolean VR_BeginEye( int eye, ref_viewpass_t *rvp );

// Release this eye's swapchain image and unbind the FBO.
void     VR_EndEye( int eye );

// xrEndFrame - submit the projection layer.
void     VR_EndFrame( void );

//
// tracking queries (used later by input / gameplay layers)
//
const vr_pose_t *VR_GetHMDPose( void );
const vr_pose_t *VR_GetHandPose( int hand );  // 0 = left, 1 = right

//
// GL context requirements - queried by the platform layer before creating the
// context so we can request a profile the runtime will accept.
//
void     VR_GetRequiredGLVersion( int *major, int *minor );

extern convar_t vr_enable;
extern convar_t vr_hands;
extern convar_t vr_hud;

#endif // VR_OPENXR_H
