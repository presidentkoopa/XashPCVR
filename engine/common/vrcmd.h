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

// PLAIN TYPES ON PURPOSE.
//
// byte, vec3_t and qboolean are engine typedefs, and this header has to be
// includable from a game DLL that has never seen them - the whole point of
// the channel is that mod code can read it. It is also the header destined
// to be published as an SDK, where depending on our internals would be
// worse still. unsigned char and float[3] are what those typedefs are.

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
#define VRCMD_VERSION 5

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
#define VRCMD_FL_MUZZLE_UP (1U<<4) // the barrel is pointed up: cases fall out

// Buttons on the hand that is on the gun, in vrcmd_t::buttons. Named for the
// shape of the input rather than for a controller's silkscreen, because the
// same bit is A on a Touch, A on an Index and a trackpad press on a Vive.
#define VRBTN_FACE_A       (1U<<0)
#define VRBTN_FACE_B       (1U<<1)
#define VRBTN_STICK_CLICK  (1U<<2)
#define VRBTN_THUMB_TOUCH  (1U<<3) // the thumb is off its rest, so it is
                                   // reaching for something

typedef struct vrcmd_s
{
	// Where each part is, as the hand has it: 0 at rest, 255 at full extent.
	//
	// A byte, not a float, because the travel it describes is at most a few
	// units and the hand cannot hold a slide to better than a millimetre
	// anyway. 255 steps over a 3.4-unit shotgun pump is 0.013 units a step.
	unsigned char part_value[VRCMD_MAX_PARTS];

	// Which of those a hand is actually on this command. A part nobody is
	// touching still reports a value - it has one - but the weapon code must
	// not read a movement into a part that is simply where the animation left
	// it. This is the bit that says the difference.
	unsigned char part_held;

	// WHICH PART IS WHICH, as the engine currently believes.
	//
	// Provisional, and the card format replaces the SOURCE of this without
	// changing the channel: until a weapon has a card, the engine's own bone
	// map is the only thing that knows the magazine from the action, and the
	// game DLL has no way to work it out from indices alone. 0xFF for absent.
	unsigned char part_action;
	unsigned char part_mag;

	unsigned char flags;            // VRCMD_FL_*
	unsigned char carried;          // vrcarry_t

	// Where the muzzle is, in world space.
	//
	// Folded in from the usercmd_t::reserved[] carrier, which works but is out
	// of room: all four slots are spent on one position, there is nowhere to
	// put anything else, and a mod using those fields for its own purposes
	// costs us the pose entirely. Here it is just three floats among others.
	float    muzzle[3];

	// ---- v3: the fire control group --------------------------------------
	//
	// THE SEAR NEEDS AN AXIS, NOT A BUTTON. A VR trigger has travel: half a
	// press must do nothing, the break must be a place the finger finds, and
	// letting off to just short of the break must not fire again. None of
	// that can be expressed as IN_ATTACK, which is why this is here rather
	// than folded into the button bits it superficially resembles.
	unsigned char trigger;          // 0 at rest, 255 at the back of its travel

	// WHICH CONTROLS A FINGER IS ON. One bit per control in the weapon's
	// card, in the order the card declares them.
	//
	// THE ENGINE REPORTS POSITION, NOT INTENT. It knows where the card put
	// each control and where the grip solve put each fingertip, so it can
	// say which control is under a thumb. It does NOT say that the control
	// was operated: that depends on what the player pressed and on what the
	// control does, and both of those are game code's. An engine that
	// decided "the magazine release was pressed" would be an engine holding
	// a threshold the server cannot check and prediction cannot replay.
	unsigned short controls_under;

	// ...and what the hand on the gun is doing, raw. Face buttons and stick
	// click as bits, thumbstick as two signed axes. The mapping from these
	// to a control's effect - a face button works a magazine release, the
	// stick pulled back thumbs a hammer - lives in the weapon code, where
	// it can be predicted and where the card's own controls are known.
	unsigned char buttons;
	signed char   stick_x;
	signed char   stick_y;

	// ---- v5: what the off hand is closing on -----------------------------
	//
	// The entity index of the nearest thing within reach of the off hand at
	// the moment it closed, or 0 for nothing. Part I's "closing a hand on it"
	// gesture.
	//
	// A PROPOSAL, NOT A CLAIM, and that distinction is the whole design. The
	// engine knows where the hands are and the game knows what is worth
	// picking up, so neither can decide this alone. The client therefore
	// offers the nearest entity it can see - whatever it is, grabbable or not,
	// without filtering - and the game looks it up, decides whether it is a
	// thing a hand can take, and bounds it by distance from the player before
	// acting. A proposal that is wrong, stale or invented is simply refused.
	//
	// This is also why grabbability does NOT have to be networked. It is
	// tempting to think the client needs to know which entities are grabbable
	// in order to pick one - entity_state_t carries no movetype, so it cannot
	// - but it does not need to know: "nearest entity to my hand" is a
	// question about geometry, which the client has, and every other part of
	// the decision belongs to the server anyway.
	//
	// ONE HAND, deliberately. The off hand is the one that fetches on every
	// other gesture in this fork - the pouch, a round to the port, a magazine
	// into the well - because the dominant hand is holding the weapon. A
	// second slot for the dominant hand is the obvious extension and is what
	// picking a dropped WEAPON up will want; it is not pretended to exist.
	unsigned short grab_ent;
} vrcmd_t;

#endif // VRCMD_H
