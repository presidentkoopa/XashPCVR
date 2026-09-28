/*
vrcmd.h - what the player's hands were doing, for the command they were doing it on
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
#ifndef VRCMD_H
#define VRCMD_H

/*
====================
STATE, NOT EVENTS. THIS IS THE WHOLE DESIGN.

A command is sent redundantly - the client backs up several of them in every
packet - and replayed by prediction, repeatedly, for as long as the server has
not confirmed it. So anything carried alongside a command is going to arrive
more than once and be processed more than once.

A block of STATE survives that: replaying "the magazine is 80% of the way out
and the off hand is gripping" twice gives the same answer twice. An EVENT does
not: "the magazine came out" replayed twice fires twice, and an event in a
dropped packet never fires at all.

Every event-shaped bug in the impulse path came from that. act_worked existed
for exactly one frame and was cleared before anything read it. The magazine
drop only fired on an edge. IN_RELOAD was pulsed once per shell inserted. Each
one was patched where it broke rather than where it was wrong, and the fix is
the same for all of them: send what the hands ARE, and let the weapon code
compare this command's hands against the last ones it saw and work out what
happened. Deriving an event from two states is idempotent. Receiving one is
not.

So: no edges in this struct, no "just happened" flags, no counters. If you find
yourself wanting to add one, the thing you actually want is a state the weapon
code can difference.
====================
*/

// Parts a weapon can have a hand on at once. Matches VR_MAX_PARTS in
// ref_api.h, which is the renderer's side of the same limit; they are declared
// apart because this header is protocol and that one is rendering, and a game
// DLL reading this should not have to include a renderer header.
#define VRCMD_MAX_PARTS 8

// Bumped when this struct's layout changes. Rides the capability handshake, so
// two builds that disagree refuse each other at connect rather than quietly
// reading each other's bytes wrong.
#define VRCMD_VERSION 1

// What the off hand is carrying. Not a count - the hand holds one thing at a
// time, which is what makes a magazine and a shell the same two gestures.
typedef enum
{
	VRCARRY_NOTHING = 0,
	VRCARRY_ROUND,        // a single round or shell, for a tube or a cylinder
	VRCARRY_MAGAZINE,     // a box magazine
	VRCARRY_LOADER,       // a speedloader, which fills every chamber at once
} vrcarry_t;

// Flag bits in vrcmd_t::flags.
#define VRCMD_FL_GRIP      (1U<<0) // the off hand is closed
#define VRCMD_FL_AT_PORT   (1U<<1) // what it carries is at the weapon's loading port
#define VRCMD_FL_TWOHAND   (1U<<2) // both hands are on the weapon
#define VRCMD_FL_MUZZLE    (1U<<3) // muzzle[] is good this command

typedef struct vrcmd_s
{
	// Where each part is, as the hand has it: 0 at rest, 255 at full extent.
	//
	// A byte, not a float, because the travel it describes is at most a few
	// units and the hand cannot hold a slide to better than a millimetre
	// anyway. 255 steps over a 3.4-unit shotgun pump is 0.013 units a step.
	byte     part_value[VRCMD_MAX_PARTS];

	// Which of those a hand is actually on this command. A part nobody is
	// touching still reports a value - it has one - but the weapon code must
	// not read a movement into a part that is simply where the animation left
	// it. This is the bit that says the difference.
	byte     part_held;

	byte     flags;                 // VRCMD_FL_*
	byte     carried;               // vrcarry_t

	// Where the muzzle is, in world space.
	//
	// Folded in from the usercmd_t::reserved[] carrier, which works but is out
	// of room: all four slots are spent on one position, there is nowhere to
	// put anything else, and a mod using those fields for its own purposes
	// costs us the pose entirely. Here it is just three floats among others.
	vec3_t   muzzle;
} vrcmd_t;

#endif // VRCMD_H
