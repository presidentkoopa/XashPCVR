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
#include "vrsynth.h"

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
#define VR_GAMEAPI_VERSION  5

struct edict_s;

// ---- version 3 ----

// One joint's predicted position, as the shared simulator has it.
//
// BY BONE NAME, NOT BY INDEX. The renderer numbers parts in the order its
// own bone scan found them; a card numbers joints in the order it declares
// them. Those two orders have no reason to agree, and matching them by
// index would pose the right value onto the wrong part - silently, and
// differently per model.
//
// A fixed array rather than a pointer because this crosses a DLL boundary:
// a borrowed pointer would tie the caller to the game DLL's idea of how
// long its card cache lives, and eight names a frame is nothing.
typedef struct vr_jointvalue_s
{
	char    bone[32];
	float   value;          // 0 at rest, 1 at full travel
} vr_jointvalue_t;

// A control on a weapon, as the engine needs to see it.
//
// The engine's only job here is geometry: is a fingertip near this point. It
// is told WHICH FINGER because Part G's table is per digit - a safety under
// the thumb is not the same gesture as a bolt release under the index - and
// it is told the radius because how near counts is a property of the control
// rather than a constant. Everything about what the control DOES stays in the
// simulator, which the server runs too.
//
// Position is in the weapon's own frame, in units, as the card measured it.
typedef struct vr_control_s
{
	// WHICH BONE at[] IS MEASURED IN. The card records it and it travels
	// here, by name, because the renderer and the card number bones in
	// orders that have no reason to agree - the same reason joint posing
	// and part matching are both by name. An empty name means the position
	// cannot be placed in the world, and the engine must treat the control
	// as never under anything rather than guess a frame.
	char    bone[32];

	float   at[3];
	float   radius;
	int     finger;         // VR_FINGER_*
} vr_control_t;

// Which digit works a control. Matches the card's `finger` vocabulary.
#define VR_FINGER_THUMB   0
#define VR_FINGER_INDEX   1
#define VR_FINGER_MIDDLE  2
#define VR_FINGER_RING    3
#define VR_FINGER_LITTLE  4


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

// What the game DLL offers back. Empty through version 2, because the engine
// had nothing it needed to call and inventing callbacks before there is a
// caller is how interfaces rot. Version 3 is what it was left there for.
typedef struct vr_game_funcs_s
{
	// Where the weapon's parts actually are, as predicted by the shared
	// simulator this command.
	//
	// THE ENGINE ASKS; IT DOES NOT DECIDE. Until a weapon has a card the
	// engine drives part positions from the hand directly, and that stays
	// exactly as it was. For a carded weapon the joint simulation is the
	// authority - it is the thing the server also runs - so the VR layer
	// takes these values and poses with them instead. One posing path for
	// both, which is what keeps the magazine shrink, the child re-pose and
	// the lighting-chain fix working for carded weapons without being
	// written twice.
	//
	// Returns how many entries were filled. Zero is the normal answer for
	// a player with no carded weapon in hand, and for every mod that has
	// never heard of any of this.
	int ( *pfnGetJointValues )( vr_jointvalue_t *out, int max );

	// ---- version 4 ----

	// Does this model need a bone the animators never made?
	//
	// Mesh surgery is DECLARED in a weapon card and APPLIED by the engine's
	// model loader - the only thing that sees a model before it is drawn.
	// Cards are the game DLL's, so the engine asks rather than parsing them
	// a second time. Called once per model, at load.
	//
	// Returns how many parts were written. Zero is the normal answer, and
	// the answer for every mod that has never heard of any of this.
	int ( *pfnGetSynthParts )( const char *model, vr_synthpart_t *out, int max );

	// ---- version 5 ----

	// Where this weapon's controls are, so the engine can tell which one a
	// thumb is on.
	//
	// THE BIT IS THE INDEX. controls_under in vrcmd_t is a bitmask "a bit per
	// control in the card's order", and this returns them in that order, so
	// out[i] is the control that bit i stands for. The engine decides nothing
	// about what a control DOES - it reports only which are under a finger,
	// and the simulator, which the server also runs, decides the rest. That
	// split is Part G's and it is why the engine never needed to see a card
	// before now.
	//
	// Positions are in the weapon's own frame, in units, as the card measured
	// them. Returns how many were filled; zero for a weapon with no card, no
	// controls, or none in hand - which is the normal answer and the answer
	// for every mod that has never heard of any of this.
	int ( *pfnGetControls )( vr_control_t *out, int max );
} vr_game_funcs_t;

// The export itself. Returns VR_GAMEAPI_VERSION on success, 0 to decline.
typedef int ( *VR_GAMEAPI_FN )( int version, const vr_engine_funcs_t *engine, vr_game_funcs_t *game );

#endif // VR_GAMEAPI_H
