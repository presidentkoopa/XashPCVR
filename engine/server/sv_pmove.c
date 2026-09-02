/*
sv_pmove.c - server-side player physic
Copyright (C) 2010 Uncle Mike

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
#include "server.h"
#include "const.h"
#include "pm_local.h"
#include "event_flags.h"
#include "studio.h"
#if !XASH_DEDICATED
#include "client/vr/vr_openxr.h"	// PCVR fork: fire from the controller
#endif

static qboolean has_update = false;
static void SV_GetTrueOrigin( sv_client_t *cl, int edictnum, vec3_t origin );

void SV_ClipPMoveToEntity( physent_t *pe, const vec3_t start, vec3_t mins, vec3_t maxs, const vec3_t end, pmtrace_t *tr )
{
	Assert( tr != NULL );

	if( svgame.physFuncs.ClipPMoveToEntity != NULL )
	{
		// do custom sweep test
		svgame.physFuncs.ClipPMoveToEntity( pe, start, mins, maxs, end, tr );
	}
	else
	{
		// function is missed, so we didn't hit anything
		tr->allsolid = false;
	}
}

static qboolean SV_CopyEdictToPhysEnt( physent_t *pe, edict_t *ed )
{
	model_t	*mod = SV_ModelHandle( ed->v.modelindex );

	if( !mod ) return false;
	pe->player = false;

	pe->info = NUM_FOR_EDICT( ed );
	VectorCopy( ed->v.origin, pe->origin );
	VectorCopy( ed->v.angles, pe->angles );

	if( FBitSet( ed->v.flags, FL_CLIENT ))
	{
		// client
		SV_GetTrueOrigin( sv.current_client, pe->info, pe->origin );
		if( FBitSet( ed->v.flags, FL_FAKECLIENT )) // fakeclients have client flag too
		{
			// bot
			Q_strncpy( pe->name, "bot", sizeof( pe->name ));
		}
		else
		{
			Q_strncpy( pe->name, "player", sizeof( pe->name ));
		}
		pe->player = pe->info;
	}
	else
	{
		// otherwise copy the modelname
		Q_strncpy( pe->name, mod->name, sizeof( pe->name ));
	}

	pe->model = pe->studiomodel = NULL;

	switch( ed->v.solid )
	{
	case SOLID_NOT:
	case SOLID_BSP:
		pe->model = mod;
		VectorClear( pe->mins );
		VectorClear( pe->maxs );
		break;
	case SOLID_BBOX:
		if( mod && mod->type == mod_studio && mod->flags & STUDIO_TRACE_HITBOX )
			pe->studiomodel = mod;
		VectorCopy( ed->v.mins, pe->mins );
		VectorCopy( ed->v.maxs, pe->maxs );
		break;
	case SOLID_CUSTOM:
		pe->model = (mod->type == mod_brush) ? mod : NULL;
		pe->studiomodel = (mod->type == mod_studio) ? mod : NULL;
		VectorCopy( ed->v.mins, pe->mins );
		VectorCopy( ed->v.maxs, pe->maxs );
		break;
	default:
		pe->studiomodel = (mod->type == mod_studio) ? mod : NULL;
		VectorCopy( ed->v.mins, pe->mins );
		VectorCopy( ed->v.maxs, pe->maxs );
		break;
	}

	pe->solid = ed->v.solid;
	pe->rendermode = ed->v.rendermode;
	pe->skin = ed->v.skin;
	pe->frame = ed->v.frame;
	pe->sequence = ed->v.sequence;

	memcpy( &pe->controller[0], &ed->v.controller[0], 4 * sizeof( byte ));
	memcpy( &pe->blending[0], &ed->v.blending[0], 2 * sizeof( byte ));

	pe->movetype = ed->v.movetype;
	pe->takedamage = ed->v.takedamage;
	pe->team = ed->v.team;
	pe->classnumber = ed->v.playerclass;
	pe->blooddecal = 0;	// unused in GoldSrc

	// for mods
	pe->iuser1 = ed->v.iuser1;
	pe->iuser2 = ed->v.iuser2;
	pe->iuser3 = ed->v.iuser3;
	pe->iuser4 = ed->v.iuser4;
	pe->fuser1 = ed->v.fuser1;
	pe->fuser2 = ed->v.fuser2;
	pe->fuser3 = ed->v.fuser3;
	pe->fuser4 = ed->v.fuser4;

	VectorCopy( ed->v.vuser1, pe->vuser1 );
	VectorCopy( ed->v.vuser2, pe->vuser2 );
	VectorCopy( ed->v.vuser3, pe->vuser3 );
	VectorCopy( ed->v.vuser4, pe->vuser4 );

	return true;
}

static qboolean SV_ShouldUnlagForPlayer( sv_client_t *cl )
{
	// can't unlag in singleplayer
	if( svs.maxclients <= 1 )
		return false;

	// unlag disabled globally
	if( !svgame.dllFuncs.pfnAllowLagCompensation() || !sv_unlag.value )
		return false;

	if( !FBitSet( cl->flags, FCL_LAG_COMPENSATION ))
		return false;

	// player not ready
	if( cl->state != cs_spawned )
		return false;

	return true;
}

static void SV_GetTrueOrigin( sv_client_t *cl, int edictnum, vec3_t origin )
{
	if( !SV_ShouldUnlagForPlayer( cl ))
		return;

	if( edictnum < 1 || edictnum > svs.maxclients )
		return;

	if( svgame.interp[edictnum-1].active && svgame.interp[edictnum-1].moving )
		VectorCopy( svgame.interp[edictnum-1].oldpos, origin );
}

static void SV_GetTrueMinMax( sv_client_t *cl, int edictnum, vec3_t mins, vec3_t maxs )
{
	if( !SV_ShouldUnlagForPlayer( cl ))
		return;

	if( edictnum < 1 || edictnum > svs.maxclients )
		return;

	if( svgame.interp[edictnum-1].active && svgame.interp[edictnum-1].moving )
	{
		VectorCopy( svgame.interp[edictnum-1].mins, mins );
		VectorCopy( svgame.interp[edictnum-1].maxs, maxs );
	}
}

/*
====================
SV_AddLinksToPmove

collect solid entities
====================
*/
static void SV_AddLinksToPmove( areanode_t *node, const vec3_t pmove_mins, const vec3_t pmove_maxs )
{
	link_t	*l, *next;
	edict_t	*check, *pl;
	vec3_t	mins, maxs;
	physent_t	*pe;

	pl = SV_EdictNum( svgame.pmove->player_index + 1 );
	Assert( SV_IsValidEdict( pl ));

	// touch linked edicts
	for( l = node->solid_edicts.next; l != &node->solid_edicts; l = next )
	{
		next = l->next;
		check = EDICT_FROM_AREA( l );

		if( check->v.groupinfo != 0 )
		{
			if( !SV_CheckGroupOp( svs.groupop, check->v.groupinfo, pl->v.groupinfo ))
				continue;
		}

		if( check->v.owner == pl || check->v.solid == SOLID_TRIGGER )
			continue; // player or player's own missile

		if( svgame.pmove->numvisent < MAX_PHYSENTS )
		{
			pe = &svgame.pmove->visents[svgame.pmove->numvisent];
			if( SV_CopyEdictToPhysEnt( pe, check ))
				svgame.pmove->numvisent++;
		}

		if( check->v.solid == SOLID_NOT && ( check->v.skin == CONTENTS_NONE || check->v.modelindex == 0 ))
			continue;

		// ignore monsterclip brushes
		if( FBitSet( check->v.flags, FL_MONSTERCLIP ) && check->v.solid == SOLID_BSP )
			continue;

		if( check == pl ) continue;	// himself

		// nehahra collision flags
		if( check->v.movetype != MOVETYPE_PUSH )
		{
			if(( FBitSet( check->v.flags, FL_CLIENT|FL_FAKECLIENT ) && check->v.health <= 0.0f ) || check->v.deadflag == DEAD_DEAD )
				continue;	// dead body
		}

		if( VectorIsNull( check->v.size ))
			continue;

		VectorCopy( check->v.absmin, mins );
		VectorCopy( check->v.absmax, maxs );

		if( FBitSet( check->v.flags, FL_CLIENT ) && !FBitSet( check->v.flags, FL_FAKECLIENT ))
		{
			if( sv.current_client )
			{
				// trying to get interpolated values
				SV_GetTrueMinMax( sv.current_client, NUM_FOR_EDICT( check ), mins, maxs );
			}
		}

		if( !BoundsIntersect( pmove_mins, pmove_maxs, mins, maxs ))
			continue;

		if( svgame.pmove->numphysent < MAX_PHYSENTS )
		{
			pe = &svgame.pmove->physents[svgame.pmove->numphysent];

			if( SV_CopyEdictToPhysEnt( pe, check ))
				svgame.pmove->numphysent++;
		}
	}

	// recurse down both sides
	if( node->axis == -1 ) return;

	if( pmove_maxs[node->axis] > node->dist )
		SV_AddLinksToPmove( node->children[0], pmove_mins, pmove_maxs );
	if( pmove_mins[node->axis] < node->dist )
		SV_AddLinksToPmove( node->children[1], pmove_mins, pmove_maxs );
}

/*
====================
SV_AddLaddersToPmove
====================
*/
static void SV_AddLaddersToPmove( areanode_t *node, const vec3_t pmove_mins, const vec3_t pmove_maxs )
{
	link_t	*l, *next;
	edict_t	*check;

	// get ladder edicts
	for( l = node->solid_edicts.next; l != &node->solid_edicts; l = next )
	{
		model_t	*mod;
		physent_t	*pe;

		next = l->next;
		check = EDICT_FROM_AREA( l );

		if( check->v.solid != SOLID_NOT || check->v.skin != CONTENTS_LADDER )
			continue;

		mod = SV_ModelHandle( check->v.modelindex );

		// only brushes can have special contents
		if( !mod || mod->type != mod_brush )
			continue;

		if( !BoundsIntersect( pmove_mins, pmove_maxs, check->v.absmin, check->v.absmax ))
			continue;

		if( svgame.pmove->nummoveent == MAX_MOVEENTS )
			return;

		pe = &svgame.pmove->moveents[svgame.pmove->nummoveent];
		if( SV_CopyEdictToPhysEnt( pe, check ))
			svgame.pmove->nummoveent++;
	}

	// recurse down both sides
	if( node->axis == -1 ) return;

	if( pmove_maxs[node->axis] > node->dist )
		SV_AddLaddersToPmove( node->children[0], pmove_mins, pmove_maxs );
	if( pmove_mins[node->axis] < node->dist )
		SV_AddLaddersToPmove( node->children[1], pmove_mins, pmove_maxs );
}

static void GAME_EXPORT pfnParticle( const float *origin, int color, float life, int zpos, int zvel )
{
	if( !origin )
	{
		Con_Reportf( S_ERROR "%s: NULL origin. Ignored\n", __func__ );
		return;
	}

	MSG_BeginServerCmd( &sv.reliable_datagram, svc_particle );
	MSG_WriteVec3Coord( &sv.reliable_datagram, origin );
	MSG_WriteChar( &sv.reliable_datagram, 0 ); // no x-vel
	MSG_WriteChar( &sv.reliable_datagram, 0 ); // no y-vel
	int	v = bound( -128, (zpos * zvel) * 16.0f, 127 );
	MSG_WriteChar( &sv.reliable_datagram, v ); // write z-vel
	MSG_WriteByte( &sv.reliable_datagram, 1 );
	MSG_WriteByte( &sv.reliable_datagram, color );
	MSG_WriteByte( &sv.reliable_datagram, bound( 0, life * 8, 255 ));
}

static int GAME_EXPORT pfnTestPlayerPosition( float *pos, pmtrace_t *ptrace )
{
	return PM_TestPlayerPosition( svgame.pmove, pos, ptrace, NULL );
}

static void GAME_EXPORT pfnStuckTouch( int hitent, pmtrace_t *tr )
{
	PM_StuckTouch( svgame.pmove, hitent, tr );
}

static int GAME_EXPORT pfnPointContents( float *p, int *truecontents )
{
	return PM_PointContentsPmove( svgame.pmove, p, truecontents );
}

static int GAME_EXPORT pfnTruePointContents( float *p )
{
	return PM_TruePointContents( svgame.pmove, p );
}

static pmtrace_t GAME_EXPORT pfnPlayerTrace( float *start, float *end, int traceFlags, int ignore_pe )
{
	return PM_PlayerTraceExt( svgame.pmove, start, end, traceFlags, svgame.pmove->numphysent, svgame.pmove->physents, ignore_pe, NULL );
}

static pmtrace_t *GAME_EXPORT pfnTraceLine( float *start, float *end, int flags, int usehull, int ignore_pe )
{
	return PM_TraceLine( svgame.pmove, start, end, flags, usehull, ignore_pe );
}

static hull_t *GAME_EXPORT pfnHullForBsp( physent_t *pe, float *offset )
{
	return PM_HullForBsp( pe, svgame.pmove, offset );
}

static float GAME_EXPORT pfnTraceModel( physent_t *pe, float *start, float *end, trace_t *trace )
{
	return PM_TraceModel( svgame.pmove, pe, start, end, trace );
}

static const char *GAME_EXPORT pfnTraceTexture( int ground, float *vstart, float *vend )
{
	return PM_TraceTexture( svgame.pmove, ground, vstart, vend );
}

static void GAME_EXPORT pfnPlaySound( int channel, const char *sample, float volume, float attenuation, int fFlags, int pitch )
{
	edict_t	*ent;

	ent = SV_EdictNum( svgame.pmove->player_index + 1 );
	if( !SV_IsValidEdict( ent )) return;

	SV_StartSound( ent, channel, sample, volume, attenuation, fFlags|SND_FILTER_CLIENT, pitch );
}

static void GAME_EXPORT pfnPlaybackEventFull( int flags, int clientindex, word eventindex, float delay, float *origin,
	float *angles, float fparam1, float fparam2, int iparam1, int iparam2, int bparam1, int bparam2 )
{
	edict_t	*ent;

	ent = SV_EdictNum( clientindex + 1 );
	if( !SV_IsValidEdict( ent )) return;

	// GoldSrc always sets FEV_NOTHOST in PMove version of this function
	SV_PlaybackEventFull( flags | FEV_NOTHOST, ent, eventindex,
		delay, origin, angles,
		fparam1, fparam2,
		iparam1, iparam2,
		bparam1, bparam2 );
}

static pmtrace_t GAME_EXPORT pfnPlayerTraceEx( float *start, float *end, int traceFlags, pfnIgnore pmFilter )
{
	return PM_PlayerTraceExt( svgame.pmove, start, end, traceFlags, svgame.pmove->numphysent, svgame.pmove->physents, -1, pmFilter );
}

static int GAME_EXPORT pfnTestPlayerPositionEx( float *pos, pmtrace_t *ptrace, pfnIgnore pmFilter )
{
	return PM_TestPlayerPosition( svgame.pmove, pos, ptrace, pmFilter );
}

static pmtrace_t *GAME_EXPORT pfnTraceLineEx( float *start, float *end, int flags, int usehull, pfnIgnore pmFilter )
{
	return PM_TraceLineEx( svgame.pmove, start, end, flags, usehull, pmFilter );
}

static struct msurface_s *GAME_EXPORT pfnTraceSurface( int ground, float *vstart, float *vend )
{
	return PM_TraceSurfacePmove( svgame.pmove, ground, vstart, vend );
}

/*
===============
SV_InitClientMove

===============
*/
// Server-side ladder tracing. Separate from vr_diag because the question it
// answers - whether pm_shared agreed to run its ladder code - is invisible
// from the client and needed only while ladder work is in flight.
static CVAR_DEFINE_AUTO( vr_diag_ladder, "0", 0, "log whether pm_shared ran its ladder code" );

void SV_InitClientMove( void )
{
	Cvar_RegisterVariable( &vr_diag_ladder );

	Pmove_Init ();

	svgame.pmove->server = true;
	svgame.pmove->movevars = &svgame.movevars;
	svgame.pmove->runfuncs = false;

	// enumerate client hulls
	for( int i = 0; i < MAX_MAP_HULLS; i++ )
	{
		if( svgame.dllFuncs.pfnGetHullBounds( i, host.player_mins[i], host.player_maxs[i] ))
			Con_Reportf( "SV: hull%i, player_mins: %g %g %g, player_maxs: %g %g %g\n", i,
			host.player_mins[i][0], host.player_mins[i][1], host.player_mins[i][2],
			host.player_maxs[i][0], host.player_maxs[i][1], host.player_maxs[i][2] );
	}

	memcpy( svgame.pmove->player_mins, host.player_mins, sizeof( host.player_mins ));
	memcpy( svgame.pmove->player_maxs, host.player_maxs, sizeof( host.player_maxs ));

	// common utilities
	svgame.pmove->PM_Info_ValueForKey = Info_ValueForKey;
	svgame.pmove->PM_Particle = pfnParticle;
	svgame.pmove->PM_TestPlayerPosition = pfnTestPlayerPosition;
	svgame.pmove->Con_NPrintf = Con_NPrintf;
	svgame.pmove->Con_DPrintf = Con_DPrintf;
	svgame.pmove->Con_Printf = Con_Printf;
	svgame.pmove->Sys_FloatTime = Sys_DoubleTime;
	svgame.pmove->PM_StuckTouch = pfnStuckTouch;
	svgame.pmove->PM_PointContents = pfnPointContents;
	svgame.pmove->PM_TruePointContents = pfnTruePointContents;
	svgame.pmove->PM_HullPointContents = (void*)PM_HullPointContents;
	svgame.pmove->PM_PlayerTrace = pfnPlayerTrace;
	svgame.pmove->PM_TraceLine = pfnTraceLine;
	svgame.pmove->RandomLong = COM_RandomLong;
	svgame.pmove->RandomFloat = COM_RandomFloat;
	svgame.pmove->PM_GetModelType = pfnGetModelType;
	svgame.pmove->PM_GetModelBounds = pfnGetModelBounds;
	svgame.pmove->PM_HullForBsp = (void*)pfnHullForBsp;
	svgame.pmove->PM_TraceModel = pfnTraceModel;
	svgame.pmove->COM_FileSize = COM_FileSize;
	svgame.pmove->COM_LoadFile = COM_LoadFile;
	svgame.pmove->COM_FreeFile = COM_FreeFile;
	svgame.pmove->memfgets = Q_memfgets;
	svgame.pmove->PM_PlaySound = pfnPlaySound;
	svgame.pmove->PM_TraceTexture = pfnTraceTexture;
	svgame.pmove->PM_PlaybackEventFull = pfnPlaybackEventFull;
	svgame.pmove->PM_PlayerTraceEx = pfnPlayerTraceEx;
	svgame.pmove->PM_TestPlayerPositionEx = pfnTestPlayerPositionEx;
	svgame.pmove->PM_TraceLineEx = pfnTraceLineEx;
	svgame.pmove->PM_TraceSurface = pfnTraceSurface;

	// initalize pmove
	svgame.dllFuncs.pfnPM_Init( svgame.pmove );
}

static void PM_CheckMovingGround( edict_t *ent, float frametime )
{
	if( svgame.physFuncs.SV_UpdatePlayerBaseVelocity != NULL )
	{
		svgame.physFuncs.SV_UpdatePlayerBaseVelocity( ent );
	}
	else
	{
		SV_UpdateBaseVelocity( ent );
	}

	if( !FBitSet( ent->v.flags, FL_BASEVELOCITY ))
	{
		// apply momentum (add in half of the previous frame of velocity first)
		VectorMA( ent->v.velocity, 1.0f + (frametime * 0.5f), ent->v.basevelocity, ent->v.velocity );
		VectorClear( ent->v.basevelocity );
	}

	ClearBits( ent->v.flags, FL_BASEVELOCITY );
}

static void SV_SetupPMove( playermove_t *pmove, sv_client_t *cl, usercmd_t *ucmd, const char *physinfo )
{
	vec3_t	absmin, absmax;
	edict_t	*clent = cl->edict;

	pmove->frametime = ucmd->msec * 0.001f;
	pmove->player_index = NUM_FOR_EDICT( clent ) - 1;
	pmove->multiplayer = (svs.maxclients > 1) ? true : false;
	pmove->time = (float)(cl->timebase * 1000.0);
	VectorCopy( clent->v.origin, pmove->origin );
	VectorCopy( clent->v.v_angle, pmove->angles );
	VectorCopy( clent->v.v_angle, pmove->oldangles );
	VectorCopy( clent->v.velocity, pmove->velocity );
	VectorCopy( clent->v.basevelocity, pmove->basevelocity );
	VectorCopy( clent->v.view_ofs, pmove->view_ofs );
	VectorCopy( clent->v.movedir, pmove->movedir );
	pmove->flDuckTime = clent->v.flDuckTime;
	pmove->bInDuck = clent->v.bInDuck;
	pmove->usehull = (clent->v.flags & FL_DUCKING) ? 1 : 0; // reset hull
	pmove->flTimeStepSound = clent->v.flTimeStepSound;
	pmove->iStepLeft = clent->v.iStepLeft;
	pmove->flFallVelocity = clent->v.flFallVelocity;
	pmove->flSwimTime = clent->v.flSwimTime;
	VectorCopy( clent->v.punchangle, pmove->punchangle );
	pmove->effects = clent->v.effects;
	pmove->flags = clent->v.flags;
	pmove->gravity = clent->v.gravity;
	pmove->friction = clent->v.friction;
	pmove->oldbuttons = clent->v.oldbuttons;
	pmove->waterjumptime = clent->v.teleport_time;
	pmove->dead = (clent->v.health <= 0.0f ) ? true : false;
	pmove->deadflag = clent->v.deadflag;
	pmove->spectator = 0; // spectator physic all execute on client
	pmove->movetype = clent->v.movetype;
	if( pmove->multiplayer ) pmove->onground = -1;
	pmove->waterlevel = clent->v.waterlevel;
	pmove->watertype = clent->v.watertype;
	pmove->maxspeed = svgame.movevars.maxspeed; // GoldSrc uses sv_maxspeed here?
	pmove->clientmaxspeed = clent->v.maxspeed;
	pmove->iuser1 = clent->v.iuser1;
	pmove->iuser2 = clent->v.iuser2;
	pmove->iuser3 = clent->v.iuser3;
	pmove->iuser4 = clent->v.iuser4;
	pmove->fuser1 = clent->v.fuser1;
	pmove->fuser2 = clent->v.fuser2;
	pmove->fuser3 = clent->v.fuser3;
	pmove->fuser4 = clent->v.fuser4;
	VectorCopy( clent->v.vuser1, pmove->vuser1 );
	VectorCopy( clent->v.vuser2, pmove->vuser2 );
	VectorCopy( clent->v.vuser3, pmove->vuser3 );
	VectorCopy( clent->v.vuser4, pmove->vuser4 );
	pmove->cmd = *ucmd;	// setup current cmds
	pmove->runfuncs = true;

	Q_strncpy( pmove->physinfo, physinfo, sizeof( pmove->physinfo ));

	// setup physents
	pmove->numvisent = 0;
	pmove->numphysent = 0;
	pmove->nummoveent = 0;

	for( int i = 0; i < 3; i++ )
	{
		absmin[i] = clent->v.origin[i] - 256.0f;
		absmax[i] = clent->v.origin[i] + 256.0f;
	}

	SV_CopyEdictToPhysEnt( &svgame.pmove->physents[0], &svgame.edicts[0] );
	svgame.pmove->visents[0] = svgame.pmove->physents[0];
	svgame.pmove->numphysent = 1;	// always have world
	svgame.pmove->numvisent = 1;

	SV_AddLinksToPmove( sv_areanodes, absmin, absmax );
	SV_AddLaddersToPmove( sv_areanodes, absmin, absmax );
}

static void SV_FinishPMove( playermove_t *pmove, sv_client_t *cl )
{
	edict_t	*clent = cl->edict;

	clent->v.teleport_time = pmove->waterjumptime;
	VectorCopy( pmove->origin, clent->v.origin );
	VectorCopy( pmove->view_ofs, clent->v.view_ofs );
	VectorCopy( pmove->velocity, clent->v.velocity );
	VectorCopy( pmove->basevelocity, clent->v.basevelocity );
	VectorCopy( pmove->punchangle, clent->v.punchangle );
	VectorCopy( pmove->movedir, clent->v.movedir );
	clent->v.flTimeStepSound = pmove->flTimeStepSound;
	clent->v.flFallVelocity = pmove->flFallVelocity;
	clent->v.oldbuttons = pmove->cmd.buttons;
	clent->v.waterlevel = pmove->waterlevel;
	clent->v.watertype = pmove->watertype;
	clent->v.maxspeed = pmove->clientmaxspeed;
	clent->v.flDuckTime = pmove->flDuckTime;
	clent->v.flSwimTime = pmove->flSwimTime;
	clent->v.iStepLeft = pmove->iStepLeft;
	clent->v.movetype = pmove->movetype;
	clent->v.friction = pmove->friction;
	clent->v.deadflag = pmove->deadflag;
	clent->v.effects = pmove->effects;
	clent->v.bInDuck = pmove->bInDuck;
	clent->v.flags = pmove->flags;

	// copy back user variables
	clent->v.iuser1 = pmove->iuser1;
	clent->v.iuser2 = pmove->iuser2;
	clent->v.iuser3 = pmove->iuser3;
	clent->v.iuser4 = pmove->iuser4;
	clent->v.fuser1 = pmove->fuser1;
	clent->v.fuser2 = pmove->fuser2;
	clent->v.fuser3 = pmove->fuser3;
	clent->v.fuser4 = pmove->fuser4;
	VectorCopy( pmove->vuser1, clent->v.vuser1 );
	VectorCopy( pmove->vuser2, clent->v.vuser2 );
	VectorCopy( pmove->vuser3, clent->v.vuser3 );
	VectorCopy( pmove->vuser4, clent->v.vuser4 );

	if( pmove->onground == -1 )
	{
		ClearBits( clent->v.flags, FL_ONGROUND );
	}
	else if( pmove->onground >= 0 && pmove->onground < pmove->numphysent )
	{
		SetBits( clent->v.flags, FL_ONGROUND );
		clent->v.groundentity = SV_EdictNum( pmove->physents[pmove->onground].info );
	}

	// angles
	// show 1/3 the pitch angle and all the roll angle
	if( !clent->v.fixangle )
	{
		VectorCopy( pmove->angles, clent->v.v_angle );
		clent->v.angles[PITCH] = -( clent->v.v_angle[PITCH] / 3.0f );
		clent->v.angles[ROLL] = clent->v.v_angle[ROLL];
		clent->v.angles[YAW] = clent->v.v_angle[YAW];
	}

	SV_SetMinMaxSize( clent, host.player_mins[pmove->usehull], host.player_maxs[pmove->usehull], false );

	// all next calls ignore footstep sounds
	pmove->runfuncs = false;
}

static entity_state_t *SV_FindEntInPack( int index, client_frame_t *frame )
{
	for( int i = 0; i < frame->num_entities; i++ )
	{
		entity_state_t	*state = &svs.packet_entities[(frame->first_entity+i)%svs.num_client_entities];

		if( state->number == index )
			return state;
	}
	return NULL;
}

static qboolean SV_UnlagCheckTeleport( vec3_t old_pos, vec3_t new_pos )
{
	for( int i = 0; i < 3; i++ )
	{
		if( fabs( old_pos[i] - new_pos[i] ) > 64.0f )
			return true;
	}
	return false;
}

static void SV_SetupMoveInterpolant( sv_client_t *cl )
{
	int		i;
	float		finalpush, lerp_msec;
	float		latency, lerpFrac;
	client_frame_t	*frame, *frame2;
	entity_state_t	*state;
	sv_client_t	*check;
	sv_interp_t	*lerp;

	memset( svgame.interp, 0, sizeof( svgame.interp ));
	has_update = false;

	if( !SV_ShouldUnlagForPlayer( cl ))
		return;

	has_update = true;

	for( i = 0, check = svs.clients; i < svs.maxclients; i++, check++ )
	{
		if( check->state != cs_spawned || check == cl )
			continue;

		lerp = &svgame.interp[i];

		VectorCopy( check->edict->v.origin, lerp->oldpos );
		VectorCopy( check->edict->v.absmin, lerp->mins );
		VectorCopy( check->edict->v.absmax, lerp->maxs );
		lerp->active = true;
	}

	latency = Q_min( cl->latency, 1.5f );

	if( sv_maxunlag.value != 0.0f )
	{
		if( sv_maxunlag.value < 0.0f )
			Cvar_DirectSet( &sv_maxunlag, "0" );

		latency = Q_min( latency, sv_maxunlag.value );
	}

	lerp_msec = cl->lastcmd.lerp_msec * 0.001f;

	if( lerp_msec > 0.1f )
		lerp_msec = 0.1f;

	if( lerp_msec < cl->next_messageinterval )
		lerp_msec = cl->next_messageinterval;

	finalpush = ( host.realtime - latency - lerp_msec ) + sv_unlagpush.value;
	if( finalpush > host.realtime ) finalpush = host.realtime; // pushed too much ?

	frame = frame2 = NULL;

	for( i = 0; i < SV_UPDATE_BACKUP; i++, frame2 = frame )
	{
		frame = &cl->frames[(cl->netchan.outgoing_sequence - (i + 1)) & SV_UPDATE_MASK];

		for( int j = 0; j < frame->num_entities; j++ )
		{
			state = &svs.packet_entities[(frame->first_entity+j)%svs.num_client_entities];

			if( state->number < 1 || state->number > svs.maxclients )
				continue;

			lerp = &svgame.interp[state->number-1];
			if( lerp->nointerp ) continue;

			if( state->health <= 0 || FBitSet( state->effects, EF_NOINTERP ))
				lerp->nointerp = true;

			if( lerp->firstframe )
			{
				if( SV_UnlagCheckTeleport( state->origin, lerp->finalpos ))
					lerp->nointerp = true;
			}
			else lerp->firstframe = true;

			VectorCopy( state->origin, lerp->finalpos );
		}

		if( finalpush > frame->senttime )
			break;
	}

	if( i == SV_UPDATE_BACKUP || finalpush - frame->senttime > 1.0f )
	{
		memset( svgame.interp, 0, sizeof( svgame.interp ));
		has_update = false;
		return;
	}

	if( !frame2 )
	{
		frame2 = frame;
		lerpFrac = 0;
	}
	else
	{
		if( frame2->senttime - frame->senttime == 0.0 )
		{
			lerpFrac = 0;
		}
		else
		{
			lerpFrac = (finalpush - frame->senttime) / (frame2->senttime - frame->senttime);
			lerpFrac = bound( 0.0f, lerpFrac, 1.0f );
		}
	}

	for( i = 0; i < frame->num_entities; i++ )
	{
		int		clientnum;
		entity_state_t	*lerpstate;
		vec3_t		curpos, newpos;

		state = &svs.packet_entities[(frame->first_entity+i)%svs.num_client_entities];

		if( state->number < 1 || state->number > svs.maxclients )
			continue;

		clientnum = state->number - 1;
		check = &svs.clients[clientnum];

		if( check->state != cs_spawned || check == cl )
			continue;

		lerp = &svgame.interp[clientnum];

		if( !lerp->active || lerp->nointerp )
			continue;

		lerpstate = SV_FindEntInPack( state->number, frame2 );

		if( !lerpstate )
		{
			VectorCopy( state->origin, curpos );
		}
		else
		{
			VectorSubtract( lerpstate->origin, state->origin, newpos );
			VectorMA( state->origin, lerpFrac, newpos, curpos );
		}

		VectorCopy( curpos, lerp->curpos );
		VectorCopy( curpos, lerp->newpos );

		if( !VectorCompare( curpos, check->edict->v.origin ))
		{
			VectorCopy( curpos, check->edict->v.origin );
			SV_LinkEdict( check->edict, false );
			lerp->moving = true;
		}
	}
}

static void SV_RestoreMoveInterpolant( sv_client_t *cl )
{
	sv_client_t	*check;
	int		i;

	if( !has_update )
	{
		has_update = true;
		return;
	}

	if( !SV_ShouldUnlagForPlayer( cl ))
		return;

	for( i = 0, check = svs.clients; i < svs.maxclients; i++, check++ )
	{
		sv_interp_t	*oldlerp;

		if( check->state != cs_spawned || check == cl )
			continue;

		oldlerp = &svgame.interp[i];

		if( VectorCompareEpsilon( oldlerp->oldpos, oldlerp->newpos, ON_EPSILON ))
			continue; // they didn't actually move.

		if( !oldlerp->moving || !oldlerp->active )
			continue;

		if( VectorCompare( oldlerp->curpos, check->edict->v.origin ))
		{
			VectorCopy( oldlerp->oldpos, check->edict->v.origin );
			SV_LinkEdict( check->edict, false );
		}
	}
}

/*
===========
SV_RunCmd
===========
*/
void SV_RunCmd( sv_client_t *cl, usercmd_t *ucmd, int random_seed )
{
	edict_t	*clent;
	double	frametime;
	usercmd_t cmd;

	// if the player got kicked, do not process commands
	if( cl->state <= cs_zombie )
		return;

	clent = cl->edict;
	cmd = *ucmd;

	if( cl->ignorecmdtime > host.realtime )
	{
		if( !cl->ignorecmdtime_warned && !FBitSet( cl->flags, FCL_FAKECLIENT ))
		{
			// report to server op
			Con_Reportf( S_WARN "%s time is faster than server time (speed hack?)\n", cl->name );
			cl->ignorecmdtime_warned = true;
			cl->ignorecmdtime_warns++;

			// automatically kick player
			if( sv_speedhack_kick.value && cl->ignorecmdtime_warns > sv_speedhack_kick.value )
				SV_KickPlayer( cl, "Speed hacks aren't allowed on this server" );
		}
		cl->cmdtime += ((double)ucmd->msec / 1000.0 );
		return;
	}

	cl->ignorecmdtime = 0.0;
	cl->ignorecmdtime_warned = false;

	// chop up very long commands
	if( cmd.msec > 50 )
	{
		int	oldmsec = ucmd->msec;
		cmd.msec = oldmsec / 2;
		SV_RunCmd( cl, &cmd, random_seed );
		cmd.msec = oldmsec / 2;
		cmd.impulse = 0;
		SV_RunCmd( cl, &cmd, random_seed );
		return;
	}

	if( !FBitSet( cl->flags, FCL_FAKECLIENT ))
		SV_SetupMoveInterpolant( cl );

	svgame.dllFuncs.pfnCmdStart( cl->edict, ucmd, random_seed );

	frametime = ((double)ucmd->msec / 1000.0 );
	cl->timebase += frametime;
	cl->cmdtime += frametime;

	PM_CheckMovingGround( clent, frametime );

	VectorCopy( clent->v.v_angle, svgame.pmove->oldangles ); // save oldangles
	if( !clent->v.fixangle ) VectorCopy( ucmd->viewangles, clent->v.v_angle );

	VectorClear( clent->v.clbasevelocity );

	// copy player buttons
	clent->v.button = ucmd->buttons;
	clent->v.light_level = ucmd->lightlevel;
	if( ucmd->impulse ) clent->v.impulse = ucmd->impulse;

	svgame.globals->time = cl->timebase;

#if !XASH_DEDICATED
	// TELEPORT LOCOMOTION - commit a destination the client aimed and released.
	//
	// Applied here, before pmove and PreThink, so the rest of the frame sees
	// the player already at the new position and nothing has to be re-derived.
	//
	// This is not a substitution: it is an authoritative engine-side move, the
	// same kind SV_PushEntity performs, so it is written straight to origin
	// rather than bracketed and restored. Velocity is cleared because carrying
	// momentum through a teleport is how players end up sliding off the ledge
	// they just arrived on.
	//
	// The client already colour-coded this destination using line traces, but
	// line traces cannot know whether a BODY fits. So re-test with the player's
	// real hull and refuse if it does not - a zero-length hull trace at the
	// destination reports allsolid/startsolid exactly when the space is
	// occupied. Refusing silently is correct here: the alternative is arriving
	// stuck inside geometry.
	if( NET_IsLocalAddress( cl->netchan.remote_address ))
	{
		vec3_t tp_dest;

		if( VR_ConsumeTeleport( tp_dest ))
		{
			trace_t tr = SV_Move( tp_dest, clent->v.mins, clent->v.maxs,
				tp_dest, MOVE_NORMAL, clent, false );

			if( !tr.allsolid && !tr.startsolid )
			{
				VectorCopy( tp_dest, clent->v.origin );
				VectorClear( clent->v.velocity );
				VectorClear( clent->v.basevelocity );
				SV_LinkEdict( clent, true );
			}
		}
	}

	// PCVR fork, diagnostic only: PreThink is the other point the game DLL is
	// handed this usercmd, and the view_ofs substitution below does NOT cover
	// it. A shot leaving the eye in here is a real gap - see
	// VR_CheckTraceOutsideWindow.
	if( NET_IsLocalAddress( cl->netchan.remote_address ))
	{
		vec3_t eye;

		VectorAdd( clent->v.origin, clent->v.view_ofs, eye );
		VR_SetFirePhase( VR_FIRE_PHASE_PRETHINK, eye, ucmd->buttons );
	}
#endif

#if !XASH_DEDICATED
	// Touch to interact. CBasePlayer::PlayerUse() runs inside PreThink and
	// searches from pev->origin + pev->view_ofs along pev->v_angle, so
	// pointing those at the hand makes the mod's OWN use logic run from your
	// fingertips - no knowledge of which entities that mod considers usable,
	// which the engine could not have anyway.
	//
	// Scoped to frames where USE is actually pressed. v_angle is read by more
	// of PreThink than view_ofs is, so it is not something to leave swapped.
	{
		vec3_t use_org, use_ang;
		vec3_t saved_ofs, saved_ang;
		qboolean touched = false;

		if( NET_IsLocalAddress( cl->netchan.remote_address ) && FBitSet( ucmd->buttons, IN_USE ) && VR_GetUseSource( use_org, use_ang ))
		{
			VectorCopy( clent->v.view_ofs, saved_ofs );
			VectorCopy( clent->v.v_angle, saved_ang );
			VectorSubtract( use_org, clent->v.origin, clent->v.view_ofs );
			VectorCopy( use_ang, clent->v.v_angle );
			touched = true;
		}

		svgame.dllFuncs.pfnPlayerPreThink( clent );

		if( touched )
		{
			VectorCopy( saved_ofs, clent->v.view_ofs );
			VectorCopy( saved_ang, clent->v.v_angle );
		}
	}

	VR_SetFirePhase( VR_FIRE_PHASE_NONE, NULL, 0 );
#else
	svgame.dllFuncs.pfnPlayerPreThink( clent );
#endif

	SV_PlayerRunThink( clent, frametime, cl->timebase );


	// If conveyor, or think, set basevelocity, then send to client asap too.
	if( !VectorIsNull( clent->v.basevelocity ))
		VectorCopy( clent->v.basevelocity, clent->v.clbasevelocity );

	// setup playermove state
	SV_SetupPMove( svgame.pmove, cl, ucmd, cl->physinfo );

	// motor!
#if !XASH_DEDICATED
	// CLIMB AT HAND SPEED, THROUGH THE GAME'S OWN LADDER MOVEMENT.
	//
	// Every previous attempt here wrote the player's origin directly and
	// fought pm_shared for the result. That was never going to hold, because
	// PM_LadderMove is not a passive thing to be overridden: it sets
	// MOVETYPE_FLY and zeroes gravity - the game is ALREADY hanging the
	// player on the ladder - and then drives them by velocity. An origin
	// write lands in the middle of that and is undone the next frame, and
	// clearing FL_ONGROUND to compensate was patching a symptom of the fight.
	//
	// Worse, bypassing it threw away everything it does for free: staying
	// attached to the rungs, sliding along them, and stepping off at the top.
	// That last one is why walking up a ladder always worked and climbing it
	// by hand always stranded the player at the lip.
	//
	// So let the game move the player, and change only how fast. PM_LadderMove
	// climbs at MAX_CLIMB_SPEED capped by pmove->maxspeed, so substituting the
	// cap for the duration of the call makes the pull one-to-one with the hand
	// without the mod knowing anything happened. Same shape as the view_ofs
	// substitution around PostThink: engine-owned state, borrowed around one
	// call, put back after.
	float vr_saved_maxspeed = svgame.pmove->maxspeed;
	qboolean vr_climbing = false;
	vec3_t vr_saved_angles, vr_ladder_dir;
	qboolean vr_aimed = false;

	if( NET_IsLocalAddress( cl->netchan.remote_address ))
	{
		float climb = VR_GetLadderClimb();

		if( climb != 0.0f )
		{
			float speed = fabs( climb );

			// A floor so a slow, careful pull still moves, and the existing
			// ceiling so nobody rockets up a shaft on one big swing.
			if( speed < 16.0f ) speed = 16.0f;
			if( speed > 200.0f ) speed = 200.0f;

			svgame.pmove->maxspeed = speed;
			vr_climbing = true;

			// STANDING ON THE FLOOR, THERE IS NO DOWN.
			//
			// This is the other half of the push-off branch, and the half that
			// was throwing the player off the ladder as they grabbed it.
			//
			// To pm_shared, climbing DOWN and STEPPING OFF are the same input -
			// both are movement away from the rungs - and the only thing telling
			// them apart is whether the player is on the ground:
			//
			//     if( onFloor && normal > 0 )   // On ground moving away
			//         VectorMA( velocity, MAX_CLIMB_SPEED, plane.normal, ... )
			//
			// Reaching UP to take hold of a rung moves the hand up, which reads
			// as descend, which at the foot of a ladder reads as stepping off -
			// answered with a shove at full climb speed. The single most natural
			// motion for starting a climb was the one that ended it.
			//
			// Stood on solid ground there is nothing below to climb to, so the
			// input has no meaning worth keeping and dropping it costs nothing.
			if( climb < 0.0f && ( (int)clent->v.flags & FL_ONGROUND ))
			{
				svgame.pmove->cmd.buttons &= ~IN_BACK;
				vr_climbing = false;
				svgame.pmove->maxspeed = vr_saved_maxspeed;
			}

			// AND POINT THE MOVEMENT AT THE LADDER, NOT AT THE GAZE.
			//
			// PM_LadderMove reads direction from the view angles, dots it against
			// the ladder face, and turns the part heading into the ladder into
			// vertical motion. Facing the rungs, forward climbs. Facing away, the
			// SAME input means stepping off - and while stood on the floor it
			// answers that by shoving the player clear at MAX_CLIMB_SPEED.
			//
			// That is why grabbing a ladder pushed the player away from it. A pull
			// is not a gaze: the hands carry their own direction, and the head is
			// free to look wherever it likes - very often down at the rung being
			// grabbed, which is precisely the look that reads as walking away.
			//
			// So borrow the angles for the length of the call and aim them at the
			// rungs. Pull down climbs, pull up descends, and where the player is
			// looking stops meaning anything - which is what hand-over-hand was
			// supposed to be. Restored immediately after, so nothing else sees it.
			if( VR_GetLadderDir( vr_ladder_dir ))
			{
				VectorCopy( svgame.pmove->angles, vr_saved_angles );
				VectorClear( svgame.pmove->angles );
				svgame.pmove->angles[YAW] = ( atan2( vr_ladder_dir[1], vr_ladder_dir[0] )
					* 180.0f / M_PI_F );
				vr_aimed = true;

				// AND NOTHING BUT THE PULL GETS TO USE THOSE ANGLES.
				//
				// The substitution above points movement at the rungs, which is right
				// for the climb and wrong for everything else that reads angles. If
				// the game does not agree we are on a ladder - and PM_Ladder is much
				// stricter than VR_OnLadder, so often it does not - then PM_LadderMove
				// never runs and these angles are simply steering ordinary walking.
				//
				// Any stray forwardmove then drives the player straight at the ladder
				// no matter which way they are facing: a lean, a breath of room-scale
				// drift, a thumb resting on the stick. Observed as sliding sideways on
				// grabbing a ladder while never gaining a single unit of height.
				//
				// Zeroing them here confines the substitution to the ladder path.
				// PM_LadderMove ignores forwardmove anyway and drives by velocity, so
				// where the game DOES agree this costs nothing - and where it does not,
				// the borrowed angles now have nothing to move. Only while actually
				// pulling, so letting go instantly hands walking back.
				svgame.pmove->cmd.forwardmove = 0.0f;
				svgame.pmove->cmd.sidemove = 0.0f;
			}
		}
	}
#endif

	svgame.dllFuncs.pfnPM_Move( svgame.pmove, true );

#if !XASH_DEDICATED
	if( vr_climbing )
		svgame.pmove->maxspeed = vr_saved_maxspeed;

	if( vr_aimed )
		VectorCopy( vr_saved_angles, svgame.pmove->angles );

	// Did the GAME think we were on a ladder? PM_LadderMove is the only
	// thing that sets MOVETYPE_FLY here, so this answers the one question
	// that separates "the pull is wrong" from "the pull went nowhere" -
	// and that question has cost several rounds of guessing to answer.
	if( vr_climbing && vr_diag_ladder.value != 0.0f )
		Con_Printf( "LADDERSV climb=%.1f pm_ran=%d onground=%d z=%.1f\n",
			VR_GetLadderClimb(),
			svgame.pmove->movetype == MOVETYPE_FLY ? 1 : 0,
			((int)clent->v.flags & FL_ONGROUND) ? 1 : 0,
			clent->v.origin[2] );
#endif

	// copy results back to client
	SV_FinishPMove( svgame.pmove, cl );

	if( clent->v.solid != SOLID_NOT && !sv.playersonly )
	{
		if( svgame.physFuncs.PM_PlayerTouch != NULL )
		{
			// run custom impact function
			svgame.physFuncs.PM_PlayerTouch( svgame.pmove, clent );
		}
		else
		{
			// link into place and touch triggers
			SV_LinkEdict( clent, true );
			vec3_t oldvel = Vec3( clent->v.velocity ); // save velocity

			// touch other objects
			for( int i = 0; i < svgame.pmove->numtouch; i++ )
			{
				pmtrace_t *pmtrace = &svgame.pmove->touchindex[i];
				edict_t *touch = SV_EdictNum( svgame.pmove->physents[pmtrace->ent].info );
				trace_t trace;

				VectorCopy( pmtrace->deltavelocity, clent->v.velocity );
				PM_ConvertTrace( &trace, pmtrace, touch );
				SV_Impact( touch, clent, &trace );
			}

			// restore velocity
			VectorCopy( oldvel, clent->v.velocity );
		}
	}

	svgame.pmove->numtouch = 0;
	svgame.globals->time = cl->timebase;
	svgame.globals->frametime = frametime;

	// PCVR fork: fire from the CONTROLLER, not the player's eye.
	//
	// Stock CBasePlayer::GetGunPosition() returns pev->origin + pev->view_ofs,
	// and every weapon traces from it (glock.cpp, crowbar.cpp, gauss.cpp, ...).
	// That is why shots came out of the player's face. Both reference VR ports
	// fixed it by overriding GetGunPosition() in a forked hlsdk - useless here,
	// because a forked hl.dll only fixes the single mod it was built for, and
	// the whole point is to play arbitrary mods with their own game code.
	//
	// entvars_t is ENGINE memory though: the game DLL reads view_ofs, it does
	// not own it. So point view_ofs at the controller for exactly the call
	// that fires the weapon (PostThink -> ItemPostFrame) and restore it
	// immediately. Stock hl.dll then traces from the muzzle with no changes
	// to it at all, and because nothing is added to the network protocol,
	// non-VR clients still connect normally.
	//
	// Local client only: on a listen server this same function runs for every
	// connected player, and remote desktop players must be left alone.
	// Restored right after the call so networking, prediction and the client's
	// own view never observe the substituted value.
	{
		vec3_t saved_view_ofs;
		qboolean vr_muzzle = false;

#if !XASH_DEDICATED
		qboolean vr_local = NET_IsLocalAddress( cl->netchan.remote_address );
		vec3_t vr_off_org, vr_off_dir;
		vec3_t vr_saved_vuser1, vr_saved_vuser2;
		qboolean vr_vuser_sub = false;
		vec3_t vr_eye;
		float vr_saved_aim = 0.0f, vr_saved_allow_autoaim = 0.0f;
		qboolean vr_aim_sub = false;

		// The REAL eye, captured before any substitution - this is where the
		// mod would fire from if it fired outside the bracket below.
		if( vr_local )
			VectorAdd( clent->v.origin, clent->v.view_ofs, vr_eye );

		if( vr_local && VR_WeaponOriginActive( ))
		{
			vec3_t muzzle;

			if( VR_GetWeaponAim( muzzle, NULL ))
			{
				VectorCopy( clent->v.view_ofs, saved_view_ofs );
				VectorSubtract( muzzle, clent->v.origin, clent->v.view_ofs );
				vr_muzzle = true;
			}
		}

		// VR akimbo: hand the off hand's muzzle and aim to the game DLL.
		// vuser1/vuser2 are entvars_t fields reserved "For mods" that stock
		// Half-Life never reads, so a DLL that does not understand them is
		// simply unaffected. Cleared when not dual wielding so a stale pose
		// can never be picked up.
		//
		// SAVED AND RESTORED like every other substitution here, and only
		// touched when there is actually an off-hand pose to hand over.
		//
		// "Reserved for mods" means reserved for THE MOD - and a mod that uses
		// vuser1 for its own state is entitled to. Clearing them every frame
		// (which the previous version did, VR active or not) silently zeroed
		// that mod's data forever, on a fork whose whole point is running
		// arbitrary mods. Writing only inside the PostThink window, and putting
		// the mod's values back afterwards, keeps this invisible to anything
		// that is not looking for it.
		if( vr_local && VR_GetOffhandFire( vr_off_org, vr_off_dir ))
		{
			VectorCopy( clent->v.vuser1, vr_saved_vuser1 );
			VectorCopy( clent->v.vuser2, vr_saved_vuser2 );
			VectorCopy( vr_off_org, clent->v.vuser1 );
			VectorCopy( vr_off_dir, clent->v.vuser2 );
			vr_vuser_sub = true;
		}

		// SUPPRESS STOCK AUTO-AIM for the shot this call is about to fire.
		//
		// Half-Life magnetically snaps aim toward nearby targets, and the
		// deflection is applied to pev->v_angle - which in VR is the tracked
		// weapon direction we just wrote into the usercmd. So the game quietly
		// bends the barrel away from where the player is physically pointing,
		// by up to 25 degrees of pitch and 12 of yaw
		// (CBasePlayer::AutoaimDeflection). It also desynchronises the bullet
		// from the laser sight, which draws the true barrel line - the single
		// largest source of "the gun does not shoot where I am pointing" that
		// is not our own maths.
		//
		// This needs no game-DLL cooperation, which is what makes it fit the
		// fork's rule exactly: the switch is an ENGINE-owned cvar that every
		// mod's autoaim merely reads (verified in the hlsdk-portable, opfor,
		// bshift, theyhunger, decay, poke646, echoes and aomdc trees - all of
		// them gate on g_psv_aim / g_psv_allow_autoaim). So bracket it the same
		// way view_ofs is bracketed above: force it off for exactly the call
		// that fires, then put the player's own setting back.
		//
		// Cvar_DirectSetValue rather than Cvar_Set, so the archived value is
		// never rewritten into config.cfg and a flatscreen player's preference
		// survives untouched.
		if( vr_local )
		{
			vr_saved_aim = sv_aim.value;
			vr_saved_allow_autoaim = sv_allow_autoaim.value;

			if( vr_saved_aim != 0.0f )
				Cvar_DirectSetValue( &sv_aim, 0.0f );
			if( vr_saved_allow_autoaim != 0.0f )
				Cvar_DirectSetValue( &sv_allow_autoaim, 0.0f );

			vr_aim_sub = true;
		}

		// Mark the covered phase. Firing from the eye in here is expected and
		// is exactly what the substitution above redirects to the muzzle.
		if( vr_local )
			VR_SetFirePhase( VR_FIRE_PHASE_POSTTHINK, vr_eye, ucmd->buttons );
#endif

		// VR-TO-VR CROSSPLAY: a REMOTE VR player's muzzle, off the usercmd.
		//
		// Same substitution as the local path, sourced from the wire instead of
		// from client VR state. Only for a client that negotiated
		// NET_EXT_VRPOSE and stamped the sentinel, so a mod using these
		// "reserved for modders" fields for its own purposes can never be
		// mistaken for a pose.
		//
		// Deliberately OUTSIDE the !XASH_DEDICATED guard, and that is the
		// interesting part: this branch is three floats and a VectorSubtract
		// with no VR code behind it, so a DEDICATED server can host
		// VR-correct players without OpenXR, a headset, or any of the client VR
		// layer compiled in at all.
		//
		// Skipped when the local path already claimed it - that one reads
		// full-precision state directly and does not go through the wire's
		// 0.125-unit quantisation.
		if( !vr_muzzle && FBitSet( cl->extensions, NET_EXT_VRPOSE )
			&& ucmd->reserved[0] == VR_USERCMD_POSE_MAGIC )
		{
			vec3_t muzzle;

			muzzle[0] = *(const float *)&ucmd->reserved[1];
			muzzle[1] = *(const float *)&ucmd->reserved[2];
			muzzle[2] = *(const float *)&ucmd->reserved[3];

			// A pose implausibly far from the body is a decoding error or a
			// hostile client, not a long arm. Reject rather than teleport the
			// shot origin across the map.
			{
				vec3_t d;

				VectorSubtract( muzzle, clent->v.origin, d );

				if( VectorLength( d ) < 128.0f )
				{
					VectorCopy( clent->v.view_ofs, saved_view_ofs );
					VectorCopy( d, clent->v.view_ofs );
					vr_muzzle = true;
				}
			}
		}

		// run post-think
		svgame.dllFuncs.pfnPlayerPostThink( clent );

#if !XASH_DEDICATED
		VR_SetFirePhase( VR_FIRE_PHASE_NONE, NULL, 0 );

		if( vr_aim_sub )
		{
			Cvar_DirectSetValue( &sv_aim, vr_saved_aim );
			Cvar_DirectSetValue( &sv_allow_autoaim, vr_saved_allow_autoaim );
		}

		if( vr_vuser_sub )
		{
			VectorCopy( vr_saved_vuser1, clent->v.vuser1 );
			VectorCopy( vr_saved_vuser2, clent->v.vuser2 );
		}
#endif

		// RESTORE SITS OUTSIDE THE GUARD, because one of the substitutions does.
		//
		// The crossplay path above compiles into a dedicated build by design.
		// Leaving its restore inside #if !XASH_DEDICATED meant a dedicated
		// server would substitute view_ofs and never put it back - permanently
		// relocating that player's eye. Everything that reads view_ofs would
		// then be wrong for the rest of the session: monster line-of-sight
		// (FVisible traces to EyePosition = origin + view_ofs), networking, and
		// the client's own view. Whatever set vr_muzzle must be able to undo it,
		// so the two have to be compiled in together.
		if( vr_muzzle )
			VectorCopy( saved_view_ofs, clent->v.view_ofs );
	}

	svgame.dllFuncs.pfnCmdEnd( clent );

	if( !FBitSet( cl->flags, FCL_FAKECLIENT ))
	{
		SV_RestoreMoveInterpolant( cl );
	}
}
