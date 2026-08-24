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
#include "vr_openxr.h"

#if XASH_WIN32 && !XASH_DEDICATED

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

#define GL_FRAMEBUFFER_EXT          0x8D40
#define GL_COLOR_ATTACHMENT0_EXT    0x8CE0
#define GL_DEPTH_ATTACHMENT_EXT     0x8D00
#define GL_RENDERBUFFER_EXT         0x8D41
#define GL_DEPTH_COMPONENT24_EXT    0x81A6
#define GL_FRAMEBUFFER_COMPLETE_EXT 0x8CD5
#define GL_TEXTURE_2D_T             0x0DE1

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
#undef GETPROC

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
};

typedef struct
{
	XrSwapchain            handle;
	uint32_t               width, height;
	uint32_t               image_count;
	XrSwapchainImageOpenGLKHR *images;   // GL texture names
	GLuint_t              *fbos;         // one FBO per swapchain image
	GLuint_t               depth_rb;     // shared depth renderbuffer
	uint32_t               acquired_index;
} vr_swapchain_t;

static struct
{
	qboolean      available;      // instance exists
	qboolean      have_system;    // xrGetSystem succeeded (an HMD is present)
	qboolean      session_ready;  // session created
	qboolean      running;        // session state is running; frames may be submitted
	qboolean      frame_started;  // between xrBeginFrame and xrEndFrame
	qboolean      eyes_submitted; // at least one eye was rendered this frame
	double        next_system_retry;
	int           frames_submitted;

	// where the play space sits in the game world, refreshed each frame
	vec3_t        world_origin;
	float         world_yaw;
	float         body_yaw;       // play-space rotation in world (mouse/stick turn)
	float         injected_yaw;   // head yaw written into cl.viewangles last frame

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
	vr_swapchain_t swapchains[VR_MAX_EYES];

	XrFrameState  frame_state;

	vr_pose_t     hmd_pose;
	vr_pose_t     hand_pose[2];

	int           gl_major, gl_minor;

	// ---- input ----
	qboolean      input_ready;
	XrActionSet   action_set;
	XrAction      actions[VRA_COUNT];
	XrAction      act_hand_pose;
	XrPath        hand_path[2];  // /user/hand/left, /user/hand/right
	XrSpace       hand_space[2];

	float         move_x, move_y;   // -1..1 locomotion stick
	float         turn_x, turn_y;   // -1..1 turn stick
	qboolean      btn[VRA_COUNT];
	qboolean      btn_prev[VRA_COUNT];
	qboolean      snap_pending;

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

		VR_DiagPrintf( "t=%7.2f  hmd pos=(%7.1f %7.1f %7.1f)  ang=(p%7.2f y%7.2f r%7.2f)"
			"  world=(%7.1f %7.1f %7.1f) yaw=%6.1f  fps=%5.1f  hz=%5.1f"
			"  stick=(%5.2f %5.2f) turn=%5.2f  sub=%d%s\n",
			host.realtime - vrdiag.session_start,
			vr.hmd_pose.origin[0], vr.hmd_pose.origin[1], vr.hmd_pose.origin[2],
			vr.hmd_pose.angles[PITCH], vr.hmd_pose.angles[YAW], vr.hmd_pose.angles[ROLL],
			vr.world_origin[0], vr.world_origin[1], vr.world_origin[2], vr.world_yaw,
			vrdiag.fps_frames ? vrdiag.fps_accum / vrdiag.fps_frames : 0.0f,
			hmd_hz,
			vr.move_x, vr.move_y, vr.turn_x,
			vr.frames_submitted,
			frozen ? "  [FROZEN]" : "" );
	}

	vrdiag.fps_accum = 0.0f;
	vrdiag.fps_frames = 0;
	vrdiag.samples++;
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
	// so atan2( right[2], up[2] ) == -roll, hence the negation below.
	angles[YAW]   = RAD2DEG( atan2f( fwd[1], fwd[0] ));
	angles[PITCH] = RAD2DEG( -asinf( bound( -1.0f, fwd[2], 1.0f )));
	angles[ROLL]  = RAD2DEG( -atan2f( right[2], up[2] ));

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
	VectorCopy( origin, vr.world_origin );
	vr.world_yaw = vr.body_yaw;
}

float VR_GetBodyYaw( void )
{
	return vr.body_yaw;
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
	vr.body_yaw = anglemod( angles[YAW] - vr.injected_yaw );

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
	XrInstanceCreateInfo ci = { XR_TYPE_INSTANCE_CREATE_INFO };
	const char *enabled[4];
	uint32_t n_enabled = 0;
	XrSystemGetInfo sgi = { XR_TYPE_SYSTEM_GET_INFO };
	XrInstanceProperties ip = { XR_TYPE_INSTANCE_PROPERTIES };

	memset( &vr, 0, sizeof( vr ));
	vr.instance = XR_NULL_HANDLE;
	vr.session  = XR_NULL_HANDLE;
	vr.system   = XR_NULL_SYSTEM_ID;

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
static qboolean VR_CreateSwapchain( int eye )
{
	vr_swapchain_t *sc = &vr.swapchains[eye];
	XrSwapchainCreateInfo ci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
	uint32_t n = 0, i;

	sc->width  = vr.view_configs[eye].recommendedImageRectWidth;
	sc->height = vr.view_configs[eye].recommendedImageRectHeight;

	ci.arraySize   = 1;
	ci.mipCount    = 1;
	ci.faceCount   = 1;
	ci.format      = 0x8058; // GL_RGBA8
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

	// one shared depth buffer per eye
	vrgl.GenRenderbuffers( 1, &sc->depth_rb );
	vrgl.BindRenderbuffer( GL_RENDERBUFFER_EXT, sc->depth_rb );
	vrgl.RenderbufferStorage( GL_RENDERBUFFER_EXT, GL_DEPTH_COMPONENT24_EXT, sc->width, sc->height );
	vrgl.BindRenderbuffer( GL_RENDERBUFFER_EXT, 0 );

	// wrap each swapchain texture in an FBO once, up front
	vrgl.GenFramebuffers( n, sc->fbos );
	for( i = 0; i < n; i++ )
	{
		GLenum_t status;

		vrgl.BindFramebuffer( GL_FRAMEBUFFER_EXT, sc->fbos[i] );
		vrgl.FramebufferTexture2D( GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT,
			GL_TEXTURE_2D_T, sc->images[i].image, 0 );
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

	Con_Printf( "VR: eye %d swapchain %ux%u, %u images\n", eye, sc->width, sc->height, n );
	return true;
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
			return false;
		vr.views[i].type = XR_TYPE_VIEW;
	}

	vr.session_ready = true;

	// The headset, not the monitor, paces a VR frame - xrWaitFrame blocks to the
	// HMD's refresh. Leaving desktop vsync on additionally clamps us to the
	// monitor (60Hz here), starving a 72/90Hz headset and causing judder.
	Cvar_Set( "gl_vsync", "0" );
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
typedef struct
{
	const char *profile;
	const char *path[VRA_COUNT];
	const char *pose_l, *pose_r;
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
			"/user/hand/left/input/squeeze/value",		// USE
			"/user/hand/left/input/x/click",		// RELOAD
			"/user/hand/left/input/y/click",		// FLASHLIGHT
			"/user/hand/right/input/thumbstick/click",	// NEXTWEAP
			"/user/hand/left/input/thumbstick/click",	// PREVWEAP
			"/user/hand/left/input/menu/click",		// MENU
		},
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
			"/user/hand/left/input/squeeze/value",
			"/user/hand/left/input/a/click",
			"/user/hand/left/input/b/click",
			"/user/hand/right/input/thumbstick/click",
			"/user/hand/left/input/thumbstick/click",
			"/user/hand/left/input/system/click",
		},
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
			"/user/hand/left/input/squeeze/click",
			NULL,
			NULL,
			"/user/hand/right/input/thumbstick/click",
			"/user/hand/left/input/thumbstick/click",
			"/user/hand/left/input/menu/click",
		},
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
			"/user/hand/left/input/squeeze/click",
			NULL,
			NULL,
			NULL,
			NULL,
			"/user/hand/left/input/menu/click",
		},
		"/user/hand/left/input/grip/pose", "/user/hand/right/input/grip/pose"
	},
};

static qboolean VR_SuggestProfile( const vr_profile_t *p )
{
	XrInteractionProfileSuggestedBinding sb = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
	XrActionSuggestedBinding b[VRA_COUNT + 2];
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

	if( p->pose_l ) { b[n].action = vr.act_hand_pose; b[n].binding = VR_Path( p->pose_l ); n++; }
	if( p->pose_r ) { b[n].action = vr.act_hand_pose; b[n].binding = VR_Path( p->pose_r ); n++; }

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

	// hand poses DO want subaction paths - we need them told apart
	memset( &aci, 0, sizeof( aci ));
	aci.type       = XR_TYPE_ACTION_CREATE_INFO;
	aci.actionType = XR_ACTION_TYPE_POSE_INPUT;
	Q_strncpy( aci.actionName, "hand", sizeof( aci.actionName ));
	Q_strncpy( aci.localizedActionName, "Hand", sizeof( aci.localizedActionName ));
	aci.countSubactionPaths = 2;
	aci.subactionPaths      = vr.hand_path;
	XR_CHECK( xrCreateAction( vr.action_set, &aci, &vr.act_hand_pose ), "xrCreateAction hand" );

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
	if( XR_SUCCEEDED( xrGetActionStateVector2f( vr.session, &gi, &v2 )) && v2.isActive )
	{
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
	if( vr.btn[VRA_NEXTWEAP] && !vr.btn_prev[VRA_NEXTWEAP] )
		Cbuf_AddText( "invnext\n" );
	if( vr.btn[VRA_PREVWEAP] && !vr.btn_prev[VRA_PREVWEAP] )
		Cbuf_AddText( "invprev\n" );
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

	if( mag < dead )
		return;

	// rescale past the deadzone so control stays smooth from the edge of it
	if( mag > 0.0f && dead < 1.0f )
	{
		float s = ( mag - dead ) / ( 1.0f - dead ) / mag;
		fx *= s;
		fy *= s;
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

	if( vr_snap_turn.value )
	{
		// discrete steps: much more comfortable for most people than smooth yaw
		if( fabs( vr.turn_x ) < 0.6f )
		{
			vr.snap_pending = false;
			return;
		}
		if( vr.snap_pending )
			return;

		vr.snap_pending = true;
		vr.body_yaw = anglemod( vr.body_yaw -
			(( vr.turn_x > 0.0f ) ? vr_snap_angle.value : -vr_snap_angle.value ));
	}
	else
	{
		if( fabs( vr.turn_x ) < dead )
			return;
		vr.body_yaw = anglemod( vr.body_yaw - vr.turn_x * vr_turnspeed.value * frametime );
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
qboolean VR_BeginFrame( void )
{
	XrFrameWaitInfo wi = { XR_TYPE_FRAME_WAIT_INFO };
	XrFrameBeginInfo bi = { XR_TYPE_FRAME_BEGIN_INFO };
	XrViewLocateInfo vli = { XR_TYPE_VIEW_LOCATE_INFO };
	XrViewState vs = { XR_TYPE_VIEW_STATE };
	XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
	uint32_t n = 0;

	vr.eyes_submitted = false;

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

	sc = &vr.swapchains[eye];

	if( XR_FAILED( xrAcquireSwapchainImage( sc->handle, &ai, &sc->acquired_index )))
		return false;

	wi.timeout = XR_INFINITE_DURATION;
	if( XR_FAILED( xrWaitSwapchainImage( sc->handle, &wi )))
		return false;

	// ref_gl renders into whatever framebuffer is bound and never rebinds one.
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

			VectorSubtract( pose.origin, vr.hmd_pose.origin, rel );
			SinCos( DEG2RAD( yaw_off ), &s, &c );

			rvp->vieworigin[0] = vr.world_origin[0] + ( rel[0] * c - rel[1] * s );
			rvp->vieworigin[1] = vr.world_origin[1] + ( rel[0] * s + rel[1] * c );
			rvp->vieworigin[2] = vr.world_origin[2] + rel[2];

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

	vrgl.BindFramebuffer( GL_FRAMEBUFFER_EXT, 0 );
	xrReleaseSwapchainImage( sc->handle, &ri );

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

	vr.eyes_submitted = true;
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
	if( vr.frame_state.shouldRender && vr.eyes_submitted )
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
}

/*
================
VR_Shutdown
================
*/
void VR_Shutdown( void )
{
	int i;

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

#else // !XASH_WIN32 || XASH_DEDICATED

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

#endif
