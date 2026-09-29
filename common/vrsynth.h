/*
vrsynth.h - a part the animators never made, declared once for everyone
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
#ifndef VRSYNTH_H
#define VRSYNTH_H

/*
====================
THREE CALLERS, ONE STRUCT.

A synthetic part is DECLARED in a weapon card, which the game DLL parses;
APPLIED by the engine's model loader, which is the only thing that sees a
model before it is drawn; and PREVIEWED by the generator, so a person can
choose the box by looking rather than guessing.

That is three pieces of code in two repositories and two languages. They have
to agree on the layout exactly, and the cheapest way to make that true is for
there to be one declaration - so this header is dependency-free, like vrcmd.h
and vrfingerprint.h, and carries no engine types.
====================
*/

#define VRSYNTH_NAME_LEN    32
#define VRSYNTH_MAX_PARTS   4

typedef struct vr_synthpart_s
{
	char    name[VRSYNTH_NAME_LEN];     // the new bone
	char    from[VRSYNTH_NAME_LEN];     // whose vertices it takes

	// The selection, in the SOURCE BONE's own space - which is the space the
	// generator's preview draws and the space a card states. Anything else
	// would need the reader to do forward kinematics before it could tell
	// which vertices were meant.
	float   box_min[3];
	float   box_max[3];

	// Where the new bone sits. Vertices are re-expressed relative to it, and
	// because it carries no rotation of its own that is a subtraction - which
	// is also why the part's normals need no rewriting.
	float   pivot[3];

	// WHICH WAY IT MOVES, AND HOW FAR, in the source bone's space.
	//
	// A real part is measured: the engine finds which sequence moves its
	// bone furthest and brackets the travel. A synthetic part has no
	// animation to measure - nobody ever animated it, which is the whole
	// reason it needed a bone - so the card states the motion instead, and
	// the surgery WRITES an animation for it. The part is then measured by
	// exactly the same code as every other part, and one posing path
	// continues to serve both.
	//
	// A zero axis means the part is posed by nothing and simply sits at its
	// pivot: still useful, since a bone that can be hidden or shrunk is
	// worth having on its own.
	float   axis[3];
	float   travel;
} vr_synthpart_t;

#endif // VRSYNTH_H
