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

//
// controller input
//

#define VR_BTN_JUMP    0
#define VR_BTN_ATTACK  1
#define VR_BTN_USE     2

// Thumbstick locomotion, already deadzoned and scaled to HL move units.
// Values are relative to the current view direction, matching how the engine
// interprets usercmd forwardmove/sidemove.
void     VR_GetMovement( float *forward, float *side );

// Apply this frame's turn input to the play-space yaw (snap or smooth).
void     VR_UpdateTurn( float frametime );

qboolean VR_GetButton( int btn );

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

#endif // VR_OPENXR_H
