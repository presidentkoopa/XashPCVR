/*
cl_view.c - player rendering positioning
Copyright (C) 2009 Uncle Mike

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

#include "common.h"
#include "client.h"
#include "const.h"
#include "entity_types.h"
#include "vgui_draw.h"
#include "sound.h"
#include "input.h" // touch
#include "platform/platform.h" // GL_UpdateSwapInterval
#include "vr/vr_openxr.h"

/*
===============
V_CalcViewRect

calc frame rectangle (Quake1 style)
===============
*/
static void V_CalcViewRect( void )
{
	qboolean	full = false;
	int	sb_lines;
	float	size;

	if( Host_IsQuakeCompatible( ))
	{
		// intermission is always full screen
		if( cl.intermission ) size = 120.0f;
		else size = scr_viewsize.value;

		if( size >= 120.0f )
			sb_lines = 0;		// no status bar at all
		else if( size >= 110.0f )
			sb_lines = 24;		// no inventory
		else sb_lines = 48;

		if( scr_viewsize.value >= 100.0f )
		{
			full = true;
			size = 100.0f;
		}
		else size = scr_viewsize.value;

		if( cl.intermission )
		{
			size = 100.0f;
			sb_lines = 0;
			full = true;
		}
		size /= 100.0f;
	}
	else
	{
		full = true;
		sb_lines = 0;
		size = 1.0f;
	}

	clgame.viewport[2] = refState.width * size;
	clgame.viewport[3] = refState.height * size;

	if( clgame.viewport[3] > refState.height - sb_lines )
		clgame.viewport[3] = refState.height - sb_lines;
	if( clgame.viewport[3] > refState.height )
		clgame.viewport[3] = refState.height;

	clgame.viewport[0] = ( refState.width - clgame.viewport[2] ) / 2;
	if( full ) clgame.viewport[1] = 0;
	else clgame.viewport[1] = ( refState.height - sb_lines - clgame.viewport[3] ) / 2;

}

/*
===============
V_SetupViewModel
===============
*/
static void V_SetupViewModel( void )
{
	cl_entity_t	*view = &clgame.viewent;
	player_info_t	*info = &cl.players[cl.playernum];

	if( !cl.local.weaponstarttime )
		cl.local.weaponstarttime = cl.time;

	// setup the viewent variables
	view->curstate.colormap = (info->topcolor & 0xFFFF)|((info->bottomcolor << 8) & 0xFFFF);
	view->curstate.number = cl.playernum + 1;
	view->index = cl.playernum + 1;
	view->model = CL_ModelHandle( cl.local.viewmodel );
	view->curstate.modelindex = cl.local.viewmodel;
	view->curstate.sequence = cl.local.weaponsequence;
	view->curstate.rendermode = kRenderNormal;

	// alias models has another animation methods
	if( view->model && view->model->type == mod_studio )
	{
		view->curstate.animtime = cl.local.weaponstarttime;
		view->curstate.frame = 0.0f;
	}
}

/*
===============
V_SetRefParams
===============
*/
static void V_SetRefParams( ref_params_t *fd )
{
	memset( fd, 0, sizeof( ref_params_t ));

	// probably this is not needs
	VectorCopy( refState.vieworg, fd->vieworg );
	VectorCopy( refState.viewangles, fd->viewangles );

	fd->frametime = host.frametime;
	fd->time = cl.time;

	fd->intermission = cl.intermission;
	fd->paused = (cl.paused != 0);
	fd->spectator = (cls.spectator != 0);
	fd->onground = (cl.local.onground != -1);
	fd->waterlevel = cl.local.waterlevel;

	VectorCopy( cl.simvel, fd->simvel );
	VectorCopy( cl.simorg, fd->simorg );

	VectorCopy( cl.viewheight, fd->viewheight );
	fd->idealpitch = cl.local.idealpitch;

	VectorCopy( cl.viewangles, fd->cl_viewangles );
	fd->health = cl.local.health;
	VectorCopy( cl.crosshairangle, fd->crosshairangle );
	if( Host_IsQuakeCompatible( ))
		fd->viewsize = scr_viewsize.value;
	else fd->viewsize = 120.0f;

	VectorCopy( cl.punchangle, fd->punchangle );
	fd->maxclients = cl.maxclients;
	fd->viewentity = cl.viewentity;
	fd->playernum = cl.playernum;
	fd->max_entities = clgame.maxEntities;
	fd->demoplayback = cls.demoplayback;
	fd->hardware = 1; // OpenGL

	if( cl.first_frame || cl.skip_interp )
	{
		cl.first_frame = false;		// now can be unlocked
		fd->smoothing = true;		// NOTE: currently this used to prevent ugly un-duck effect while level is changed
	}
	else fd->smoothing = cl.local.pushmsec;		// enable smoothing in multiplayer by server request (AMX uses)

	// get pointers to movement vars and user cmd
	fd->movevars = &clgame.movevars;
	fd->cmd = &cl.cmd;

	// setup viewport
	fd->viewport[0] = clgame.viewport[0];
	fd->viewport[1] = clgame.viewport[1];
	fd->viewport[2] = clgame.viewport[2];
	fd->viewport[3] = clgame.viewport[3];

	fd->onlyClientDraw = 0;	// reset clientdraw
	fd->nextView = 0;		// reset nextview
}

/*
===============
V_MergeOverviewRefdef

merge refdef with overview settings
===============
*/
static void V_RefApplyOverview( ref_viewpass_t *rvp )
{
	ref_overview_t	*ov = &clgame.overView;

	if( !CL_IsDevOverviewMode( ))
		return;

	// NOTE: Xash3D may use 16:9 or 16:10 aspects
	float aspect = (float)refState.width / (float)refState.height;

	float size_x = fabs( 8192.0f / ov->flZoom );
	float size_y = fabs( 8192.0f / (ov->flZoom * aspect ));

	// compute rectangle
	ov->xLeft = -(size_x / 2);
	ov->xRight = (size_x / 2);
	ov->yTop = -(size_y / 2);
	ov->yBottom = (size_y / 2);

	if( CL_IsDevOverviewMode() == 1 )
	{
		Con_NPrintf( 0, " Overview: Zoom %.2f, Map Origin (%.2f, %.2f, %.2f), Z Min %.2f, Z Max %.2f, Rotated %i\n",
		ov->flZoom, ov->origin[0], ov->origin[1], ov->origin[2], ov->zNear, ov->zFar, ov->rotated );
	}

	VectorCopy( ov->origin, rvp->vieworigin );
	rvp->vieworigin[2] = ov->zFar + ov->zNear;
	vec2_t mins = Vec2( rvp->vieworigin );
	vec2_t maxs = Vec2( rvp->vieworigin );

	mins[!ov->rotated] += ov->xLeft;
	maxs[!ov->rotated] += ov->xRight;
	mins[ov->rotated] += ov->yTop;
	maxs[ov->rotated] += ov->yBottom;

	rvp->viewangles[0] = 90.0f;
	rvp->viewangles[1] = 90.0f;
	rvp->viewangles[2] = (ov->rotated) ? (ov->flZoom < 0.0f) ? 180.0f : 0.0f : (ov->flZoom < 0.0f) ? -90.0f : 90.0f;

	SetBits( rvp->flags, RF_DRAW_OVERVIEW );

	ref.dllFuncs.GL_OrthoBounds( mins, maxs );
}

/*
====================
V_CalcFov
====================
*/
static float V_CalcFov( float *fov_x, float width, float height )
{
	if( *fov_x < 1.0f || *fov_x > 179.0f )
		*fov_x = 90.0f; // default value

	float x = width / tan( DEG2RAD( *fov_x ) * 0.5f );
	float half_fov_y = atan( height / x );

	return RAD2DEG( half_fov_y ) * 2;
}

/*
====================
V_AdjustFov
====================
*/
static void V_AdjustFov( float *fov_x, float *fov_y, float width, float height, qboolean lock_x )
{
	float x, y;

	if( width * 3 == 4 * height || width * 4 == height * 5 )
	{
		// 4:3 or 5:4 ratio
		return;
	}

	if( lock_x )
	{
		*fov_y = 2 * atan((width * 3) / (height * 4) * tan( *fov_y * M_PI_F / 360.0f * 0.5f )) * 360 / M_PI_F;
		return;
	}

	y = V_CalcFov( fov_x, 640, 480 );
	x = *fov_x;

	*fov_x = V_CalcFov( &y, height, width );
	if( *fov_x < x ) *fov_x = x;
	else *fov_y = y;
}

/*
=============
V_GetRefParams
=============
*/
static void V_GetRefParams( ref_params_t *fd, ref_viewpass_t *rvp )
{
	// part1: deniable updates
	VectorCopy( fd->simvel, cl.simvel );
	VectorCopy( fd->simorg, cl.simorg );
	VectorCopy( fd->punchangle, cl.punchangle );
	VectorCopy( fd->viewheight, cl.viewheight );

	// part2: really used updates
	VectorCopy( fd->crosshairangle, cl.crosshairangle );
	VectorCopy( fd->cl_viewangles, cl.viewangles );

	// setup ref_viewpass
	rvp->viewport[0] = fd->viewport[0];
	rvp->viewport[1] = fd->viewport[1];
	rvp->viewport[2] = fd->viewport[2];
	rvp->viewport[3] = fd->viewport[3];

	VectorCopy( fd->vieworg, rvp->vieworigin );
	VectorCopy( fd->viewangles, rvp->viewangles );

	rvp->viewentity = fd->viewentity;

	// calc FOV
	rvp->fov_x = bound( 10.0f, cl.local.scr_fov, 150.0f ); // this is a final fov value

	// first we need to compute FOV and other things that needs for frustum properly work
	rvp->fov_y = V_CalcFov( &rvp->fov_x, clgame.viewport[2], clgame.viewport[3] );

	// adjust FOV for widescreen
	if( refState.wideScreen && r_adjust_fov.value )
		V_AdjustFov( &rvp->fov_x, &rvp->fov_y, clgame.viewport[2], clgame.viewport[3], false );

	rvp->flags = 0;

	if( fd->onlyClientDraw )
		SetBits( rvp->flags, RF_ONLY_CLIENTDRAW );
	SetBits( rvp->flags, RF_DRAW_WORLD );
}

/*
==================
V_PreRender

==================
*/
qboolean V_PreRender( void )
{
	// too early
	if( !ref.initialized )
		return false;

	if( host.status == HOST_SLEEP )
		return false;

	// if the screen is disabled (loading plaque is up)
	if( cls.disable_screen )
	{
		if(( host.realtime - cls.disable_screen ) > cl_timeout.value )
		{
			Con_Reportf( "%s: loading plaque timed out\n", __func__ );
			cls.disable_screen = 0.0f;
		}
		return false;
	}

	V_CheckGamma();

	ref.dllFuncs.R_BeginFrame( !cl.paused && ( cls.state == ca_active ));

	GL_UpdateSwapInterval( );

	return true;
}

//============================================================================

/*
====================
V_ApplyRefUnderwater

apply underwater fov effect
====================
*/
static void V_ApplyRefUnderwater( ref_viewpass_t *rvp )
{
	if( !FBitSet( rvp->flags, RF_DRAW_CUBEMAP|RF_DRAW_OVERVIEW ))
		return;

	if( !FBitSet( host.features, ENGINE_QUAKE_COMPATIBLE ))
		return;

	if( cl.local.waterlevel < 3 )
		return;

	rvp->fov_x = atan( tan( DEG2RAD( rvp->fov_x ) / 2 ) * ( 0.97f + sin( cl.time * 1.5f ) * 0.03f )) * 2 / (M_PI_F / 180.0f);
	rvp->fov_y = atan( tan( DEG2RAD( rvp->fov_y ) / 2 ) * ( 1.03f - sin( cl.time * 1.5f ) * 0.03f )) * 2 / (M_PI_F / 180.0f);

}

/*
==================
V_RenderView

==================
*/
void V_RenderView( void )
{
	// HACKHACK: make ref params static
	// not really critical but allows client.dll to take address of refdef and don't trigger ASan
	static ref_params_t	rp;
	ref_viewpass_t	rvp;
	int		viewnum = 0;

	if( !cl.video_prepped || ( !ui_renderworld.value && UI_IsVisible() && !cl.background ))
		return; // still loading

	V_CalcViewRect ();	// compute viewport rectangle
	V_SetRefParams( &rp );
	V_SetupViewModel ();
	ref.dllFuncs.R_Set2DMode( false );
	SCR_DirtyScreen();
	ref.dllFuncs.GL_BackendStartFrame ();

	// PCVR fork: stereo path.
	//
	// The mod's pfnCalcRefdef still computes the view exactly as it always does -
	// we never touch it, which is what keeps this working for any GoldSrc mod.
	// We then render the SAME scene once per eye, overriding only the viewpass.
	// VR_BeginFrame drives the whole OpenXR lifecycle (HMD acquisition, lazy
	// session creation, event polling). It must NOT be gated on VR_IsActive():
	// vr.running is set only by the poll inside it, so gating would deadlock the
	// session in IDLE and it could never reach READY.
	if( VR_BeginFrame( ))
	{
		vec3_t base_origin;
		int eye;

		clgame.dllFuncs.pfnCalcRefdef( &rp );
		V_GetRefParams( &rp, &rvp );
		V_RefApplyOverview( &rvp );
		V_ApplyRefUnderwater( &rvp );

		// Anchor the play space at the mod's view origin. The rotation used is the
		// body yaw captured in VR_OverrideViewAngles, NOT rvp.viewangles - that
		// already includes head yaw, so reusing it here would apply head rotation
		// twice.
		VectorCopy( rvp.vieworigin, base_origin );
		VR_SetWorldReference( base_origin );

		if( FBitSet( rvp.flags, RF_ONLY_CLIENTDRAW ))
			ref.dllFuncs.R_ClearScreen();

		// Pin the weapon to the right controller. The mod positioned the
		// viewmodel relative to a mouse-look camera; in VR the hand owns it.
		{
			qboolean weapon_equipped = ( cl.local.viewmodel != 0 );
			cl_entity_t *view = &clgame.viewent;
			qboolean pinned = false;

			if( vr_hands.value && weapon_equipped )
			{
				vec3_t hand_org, hand_ang;

				if( VR_GetHandWorld( 1, hand_org, hand_ang ))
				{
					// Two-handed stabilisation, before any mesh correction so
					// it operates on physical tracked angles like everything
					// else. No-op unless the off hand is up at the weapon.
					VR_ApplyTwoHandedAim( hand_org, hand_ang );

					// Weapon meshes rest at a different angle than the bare-hand
					// mesh, so they get their own correction (vr_weapon_*_offset).
					// Live: with the hands correct, the gun still hung ~45 deg
					// below horizontal. Applied on the PHYSICAL angles, before
					// the pitch pre-negation below, exactly as VR_DrawHands does.
					// Skipped while braced. The two-handed angles are already a
					// finished world direction; applying the mesh rest-pose
					// correction on top of them is what made the weapon fly off.
					// Lambda1VR skips its equivalent correction identically
					// (VrInputAlt2.c:183-204).
					// ALWAYS applied, including while braced - this is the MODEL, not
					// the aim. The correction is what rotates the mesh so its barrel
					// lies along whatever angles it is handed; skipping it leaves the
					// weapon in its uncorrected rest pose, drawn ~45 degrees below the
					// firing line even though the shot itself was correct.
					//
					// The fire ray in VR_UpdateFireRay still skips it while braced,
					// because there the hand-to-hand vector is already a finished world
					// direction. Aim and model need opposite treatment here.
					VR_CalibrateWeaponAngles( hand_ang );

					// Close the residual: rotate the drawn weapon so its barrel sits
					// exactly on the firing line. The rest-pose correction above gets
					// it close, but every mesh carries its own leftover angle, which
					// is why the gun was still drawn just under its own laser.
					VR_AlignModelToFireRay( hand_ang );

					// PRE-NEGATE PITCH, same as VR_DrawHands does for the bare
					// hands. Studio rendering negates pitch (the old Quake
					// inverse-pitch bug) unless the mod sets
					// ENGINE_COMPENSATE_QUAKE_BUG - and stock Half-Life cannot
					// set it, because valve/hl.dll does not export
					// Server_GetPhysicsInterface, so host.features stays 0.
					// Both reference VR ports negate a second time on the way
					// in so the two cancel (HLVR VRHelper.cpp "360-x" +
					// StudioModelRenderer.cpp; Lambda1VR VrInputDefault.c +
					// its gl_studio.c).
					//
					// The bare hands got this fix; THIS path was missed, so the
					// equipped weapon stayed net inverted - raising the hand
					// pitched the gun down and eventually behind the player.
					hand_ang[PITCH] = -hand_ang[PITCH];

					view->curstate.movetype = MOVETYPE_NONE; // UNVERIFIED hypothesis: avoid stale interpolation blend in R_StudioSetUpTransform if viewent's movetype happens to be MOVETYPE_STEP
					VectorCopy( hand_org, view->origin );
					VectorCopy( hand_org, view->curstate.origin );
					VectorCopy( hand_org, view->latched.prevorigin );
					VectorCopy( hand_ang, view->angles );
					VectorCopy( hand_ang, view->curstate.angles );
					VectorCopy( hand_ang, view->latched.prevangles );
					pinned = true;
				}
			}

			// NOTE: previously routed this entity around the mod's pStudioDraw
			// hook (EF_VR_PINNED_VIEWMODEL, checked in gl_studio.c) to fix a
			// pivot/inversion bug - reverted, live-tested to break weapon
			// pickup, same regression this exact bypass-pStudioDraw mechanism
			// caused before when applied globally. Pickup takes priority;
			// the pivot bug fix needs a different mechanism.
			ClearBits( view->curstate.effects, EF_VR_PINNED_VIEWMODEL );

			// Left hand always; right hand only when no weapon is filling that
			// slot this frame.
			//
			// Tried drawing the tracked hand on the weapon once the welded arms
			// were stripped. It looks wrong: the hand model is posed open and
			// sits beside the grip rather than around it, because nothing lines
			// the hand up with the weapon or plays its grip sequence. A floating
			// gun reads better than a flat hand next to one. Revisit if the hand
			// is ever actually posed onto the grip (the model carries
			// fullgrab/halfgrab sequences that are currently unused).
			VR_DrawHands( !weapon_equipped );
		}

		for( eye = 0; eye < VR_GetEyeCount(); eye++ )
		{
			ref_viewpass_t eye_rvp = rvp;	// inherit flags/viewentity from the mod

			if( !VR_BeginEye( eye, &eye_rvp ))
				continue;

			GL_RenderFrame( &eye_rvp );

			// Composite the 2D layer into this eye while its FBO is still bound.
			// Without this the HUD, console and menu only ever reach the flat
			// window, leaving the headset with no health, ammo or menu at all.
			if( vr_hud.value )
			{
				ref.dllFuncs.R_Set2DMode( true );
				VR_Begin2D();
				CL_DrawHUD( CL_ACTIVE );
				UI_UpdateMenu( host.realtime );
				Con_DrawConsole();
				VR_End2D();
				ref.dllFuncs.R_Set2DMode( false );
			}

			VR_EndEye( eye );
		}

		// audio is updated once per frame, not once per eye
		S_UpdateFrame( &rvp );

		VR_EndFrame();
	}
	else
	{
		do
		{
			clgame.dllFuncs.pfnCalcRefdef( &rp );
			V_GetRefParams( &rp, &rvp );
			V_RefApplyOverview( &rvp );
			V_ApplyRefUnderwater( &rvp );

			if( viewnum == 0 && FBitSet( rvp.flags, RF_ONLY_CLIENTDRAW ))
			{
				ref.dllFuncs.R_ClearScreen();
			}

			GL_RenderFrame( &rvp );
			S_UpdateFrame( &rvp );
			viewnum++;

		} while( rp.nextView );

		// VR_BeginFrame may have opened an XR frame and then bailed (e.g. the
		// runtime said shouldRender=false). An opened frame MUST still be ended
		// or xrWaitFrame stalls next tick. This no-ops if nothing was opened.
		VR_EndFrame();
	}

	// draw debug triangles on a server
	SV_DrawDebugTriangles ();
	ref.dllFuncs.GL_BackendEndFrame ();
}

#define POINT_SIZE		16.0f
#define NODE_INTERVAL_X(x)	(x * 16.0f)
#define NODE_INTERVAL_Y(x)	(x * 16.0f)

static void R_DrawLeafNode( float x, float y, float scale )
{
	float downScale = scale * 0.25f;// * POINT_SIZE;

	ref.dllFuncs.R_DrawStretchPic( x - downScale * 0.5f, y - downScale * 0.5f, downScale, downScale, 0, 0, 1, 1, R_GetBuiltinTexture( REF_PARTICLE_TEXTURE ) );
}

static void R_DrawNodeConnection( float x, float y, float x2, float y2 )
{
	ref.dllFuncs.Begin( TRI_LINES );
		ref.dllFuncs.Vertex3f( x, y, 0 );
		ref.dllFuncs.Vertex3f( x2, y2, 0 );
	ref.dllFuncs.End( );
}

static void R_ShowTree_r( mnode_t *node, float x, float y, float scale, int shownodes, mleaf_t *viewleaf )
{
	float	downScale = scale * 0.8f;

	downScale = Q_max( downScale, 1.0f );

	if( !node ) return;

	world.recursion_level++;

	if( node->contents < 0 )
	{
		mleaf_t	*leaf = (mleaf_t *)node;

		if( world.recursion_level > world.max_recursion )
			world.max_recursion = world.recursion_level;

		if( shownodes == 1 )
		{
			if( cl.worldmodel->leafs == leaf )
				ref.dllFuncs.Color4f( 1.0f, 1.0f, 1.0f, 1.0f );
			else if( viewleaf && viewleaf == leaf )
				ref.dllFuncs.Color4f( 1.0f, 0.0f, 0.0f, 1.0f );
			else ref.dllFuncs.Color4f( 0.0f, 1.0f, 0.0f, 1.0f );
			R_DrawLeafNode( x, y, scale );
		}
		world.recursion_level--;
		return;
	}

	if( shownodes == 1 )
	{
		ref.dllFuncs.Color4f( 0.0f, 0.0f, 1.0f, 1.0f );
		R_DrawLeafNode( x, y, scale );
	}
	else if( shownodes == 2 )
	{
		R_DrawNodeConnection( x, y, x - scale, y + scale );
		R_DrawNodeConnection( x, y, x + scale, y + scale );
	}

	R_ShowTree_r( node_child( node, 1, cl.worldmodel ), x - scale, y + scale, downScale, shownodes, viewleaf );
	R_ShowTree_r( node_child( node, 0, cl.worldmodel ), x + scale, y + scale, downScale, shownodes, viewleaf );

	world.recursion_level--;
}

static void R_ShowTree( void )
{
	float	x = (float)((refState.width - (int)POINT_SIZE) >> 1);
	float	y = NODE_INTERVAL_Y(1.0f);

	if( !cl.worldmodel || !r_showtree.value )
		return;

	world.recursion_level = 0;
	mleaf_t *viewleaf = Mod_PointInLeaf( refState.vieworg, cl.worldmodel->nodes, cl.worldmodel );

	ref.dllFuncs.TriRenderMode( kRenderTransTexture );

	//pglLineWidth( 2.0f );
	ref.dllFuncs.Color4f( 1, 0.7f, 0, 1.0f );
	//pglDisable( GL_TEXTURE_2D );
	R_ShowTree_r( cl.worldmodel->nodes, x, y, world.max_recursion * 3.5f, 2, viewleaf );
	//pglEnable( GL_TEXTURE_2D );
	//pglLineWidth( 1.0f );

	R_ShowTree_r( cl.worldmodel->nodes, x, y, world.max_recursion * 3.5f, 1, viewleaf );

	Con_NPrintf( 0, "max recursion %d\n", world.max_recursion );
}

/*
==================
V_PostRender

==================
*/
void V_PostRender( void )
{
	qboolean		draw_2d = false;

	ref.dllFuncs.R_AllowFog( false );
	ref.dllFuncs.R_Set2DMode( true );

	if( cls.state == ca_active && cls.signon == SIGNONS && cls.scrshot_action != scrshot_mapshot )
	{
		SCR_TileClear();
		CL_DrawHUD( CL_ACTIVE );
		VGui_Paint();
	}

	switch( cls.scrshot_action )
	{
	case scrshot_inactive:
	case scrshot_normal:
	case scrshot_snapshot:
		draw_2d = true;
		break;
	}

	if( draw_2d )
	{
		SCR_RSpeeds();
		SCR_NetSpeeds();
		SCR_DrawPos();
		SCR_DrawEnts();
		SCR_DrawNetGraph();
		SCR_DrawUserCmd();
		Joy_DrawDebug();
		IN_GyroDrawDebug();
		SV_DrawOrthoTriangles();
		CL_DrawDemoRecording();
		CL_DrawHUD( CL_CHANGELEVEL );
		ref.dllFuncs.R_ShowTextures();
		R_ShowTree();
		UI_UpdateMenu( host.realtime );
		Con_DrawVersion();
		Con_DrawConsole();
		Con_DrawDebug(); // must be last
		Touch_Draw();
		OSK_Draw();

		S_ExtraUpdate();
	}

	SCR_MakeScreenShot();
	ref.dllFuncs.R_AllowFog( true );
	Platform_SetTimer( 0.0f );
	ref.dllFuncs.R_EndFrame();

	V_CheckGammaEnd();
}
