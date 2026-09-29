/*
vr_gameapi.h - the optional interface between the engine's VR layer and a game DLL
Copyright (C) 2026 Xash3D FWGS PCVR fork

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/
#ifndef VR_GAMEAPI_H
#define VR_GAMEAPI_H

#include "vrcmd.h"

/*
====================
OPTIONAL, AND OPTIONAL IS THE POINT.

The engine looks this export up when it loads a game DLL, the same way it
already looks up GetEntityAPI2 and GetNewDLLFunctions. A DLL that does not
export it is not broken and is not second-class - it simply gets the
engine-side behaviour it has always had: parts move visually, IN_RELOAD is
pulsed per insert, the engine blocks the trigger. That covers every closed mod
and every one nobody has rebuilt, which is most of the twelve hundred.

NOTHING is added to enginefuncs_t. That table is the ABI every GoldSrc game
DLL on earth was compiled against, and a mod that has never heard of VR must
keep loading and running exactly as before. Growing it would be the one change
this fork cannot take back.

So the shape is: the engine hands the DLL a small table of things it can ask,
and takes a table of things it can be told, and neither side has to exist for
the other to work.
====================
*/

#define VR_GAMEAPI_EXPORT   "GetVRWeaponAPI"

// Bumped when either table changes shape. The engine passes the version it
// speaks; a DLL that does not recognise it must return 0 and will be treated
// as though it never exported anything, rather than reading a table whose
// layout it is guessing at.
#define VR_GAMEAPI_VERSION  2

struct edict_s;

// What the engine can be asked.
typedef struct vr_engine_funcs_s
{
	// The hand state for the command being run for this player, right now.
	//
	// Works identically on the server, where "now" is the command SV_RunCmd is
	// executing, and in client prediction, where it is the command being
	// replayed. That symmetry is the whole reason weapon code can live in one
	// place and be trusted: both sides ask the same question during the same
	// command and get the same answer.
	//
	// Returns false when there is nothing to report - no headset, a desktop
	// player, a command that arrived without a block, or a mod-side call made
	// outside a command. `out` is zeroed in that case, which reads as no hands
	// and nothing held, so a caller that ignores the return value still
	// behaves sanely.
	qboolean ( *pfnGetVRCmd )( struct edict_s *player, vrcmd_t *out );

	// Is this player loading by hand at all? The same per-player userinfo the
	// game DLLs already read, offered here so weapon code has one place to ask
	// rather than reaching for a cvar by name.
	qboolean ( *pfnPlayerHandLoads )( struct edict_s *player );

	// ---- version 2 ----

	// What model is this, really?
	//
	// A weapon card binds to the model it was measured FROM, not to a file
	// name, because Half-Life's own `valve` directory holds two entirely
	// different rigs under the same paths. Only the engine has the loaded
	// model; only game code has the card. So the engine answers the
	// question and game code does the comparing.
	//
	// `namehash` is VR_HashBoneName folded over every bone name in bone
	// order - see vrfingerprint.h, which both sides include so that they
	// cannot disagree about it.
	//
	// Returns false for a model that is not loaded or is not a studio
	// model, leaving the outputs untouched. A card that cannot be checked
	// must not be applied: the fallback is a weapon that works.
	qboolean ( *pfnGetModelFingerprint )( const char *model, int *bones,
		int *seqs, unsigned int *namehash );
} vr_engine_funcs_t;

// What the game DLL offers back. Deliberately empty in version 1: the engine
// has nothing it needs to call yet, and inventing callbacks before there is a
// caller is how interfaces rot. It exists so that adding one later is a
// version bump rather than a new export.
typedef struct vr_game_funcs_s
{
	int unused;
} vr_game_funcs_t;

// The export itself. Returns VR_GAMEAPI_VERSION on success, 0 to decline.
typedef int ( *VR_GAMEAPI_FN )( int version, const vr_engine_funcs_t *engine, vr_game_funcs_t *game );

#endif // VR_GAMEAPI_H
