/*
mod_surgery.h - giving a studio model a bone the animators never made
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
#ifndef MOD_SURGERY_H
#define MOD_SURGERY_H

/*
====================
THE CONTENT CEILING, AND HOW TO BREAK IT.

A VR weapon card can only drive parts that are separate bones. Measured across
the Half-Life set, that rules out most of what a hand wants to touch: the MP5
has no bolt and no charging handle bone, and NEITHER the HD nor the SD pistol
has a magazine bone - so a pistol cannot be reloaded by hand at all, however
good the simulator is.

SOME of those parts exist in the mesh, welded to the body because no animator
ever needed them to move on their own. Mesh surgery gives those a bone at load
time: pick the vertices, hang them off a new bone, and the card can drive it
like any other. That is what the M40A1's bolt is - 22 vertices carved off a
bone that both turned and slid, so the two motions could become two joints.

AND SOME OF THEM ARE NOT THERE AT ALL, which this used to claim otherwise and
cost somebody a day to find out. Two measured cases:

  THE HD MP5 HAS NO CHARGING HANDLE. Above z 4.0 on `carbine`, the rearmost
  vertex is at y +1.78 - and the weapon extends back to y +5.48. There is no
  tab, no T-handle, nothing protruding from the top or rear of the receiver.
  The 605 vertices on that bone are a receiver, a carry handle, a front sight
  and a stock. You cannot carve a part out of vertices that were never drawn.

  NEITHER PISTOL HAS A MAGAZINE, and not merely no magazine BONE - no
  magazine. Proved by posing the reload: nothing leaves the grip.

RELOCATE NEVER INSERT means exactly that. This pass cannot draw geometry, so a
part the artist never modelled is out of its reach by construction, and the
answer for those is a separate small model posed by the joint - which is ours
to ship, needs no change to the player's files, and has at least these two
callers waiting. See PCVR_WEAPONS_STATUS.md.

RELOCATE, NEVER INSERT. A studio model is one blob of internal offsets, so
growing an array in place would shift every byte after it and invalidate every
offset in the file - and there are offsets to bones, controllers, hitboxes,
sequences, textures, skins, bodyparts, models, meshes and attachments. So
nothing moves: the original bytes are copied verbatim, the grown arrays are
APPENDED past the end, and only the handful of offsets that point at those
arrays are rewritten. The old copies are left where they lie, unreferenced.
They cost a few kilobytes and they buy a transformation that cannot corrupt
anything it did not deliberately touch.

WHY THE NEW BONE ONLY TRANSLATES. A synthetic bone is parented to the bone its
vertices came from, with no rotation and its origin at the declared pivot, so
moving a vertex across is a subtraction. It also means NORMALS NEED NO
REWRITING: a bone that only ever translates does not rotate its vertices'
normals, so the lighting stays correct without touching the normal arrays,
which in GoldSrc are indexed separately from vertices and cannot be mapped
back to them reliably. A synthetic part that needs to ROTATE - a hammer, a
cylinder latch - would light wrongly as it swings, and is deliberately not
supported yet.

CLIENT VIEW MODELS ONLY. This changes what is drawn and nothing else: no
hitbox, no attachment, no bone index in any existing structure moves, so
server-side hit detection and every mod's own code see exactly what they saw
before.
====================
*/

// The declaration itself lives in common/vrsynth.h, shared with the game DLL
// that parses it out of a card and with the generator that previews it. One
// struct, three callers; see that header.
#include "vrsynth.h"

#define MOD_SYNTH_NAME_LEN  VRSYNTH_NAME_LEN
#define MOD_MAX_SYNTH       VRSYNTH_MAX_PARTS

typedef vr_synthpart_t mstudiosynth_t;

/*
====================
Mod_StudioSurgery

Writes a grown copy of `in` into `out`, with one new bone per synthetic part.

Call with out = NULL to get the size needed. Returns the number of bytes
written (or required), or 0 on refusal with *err set to a static string.

REFUSES, rather than guessing, when:
  - the model is not a studio model, or is not version 10
  - it would exceed MAXSTUDIOBONES
  - any sequence lives in an external sequence-group file, whose bytes are
    not here to grow
  - a rewritten animation offset would not fit in the 16 bits the format
    gives it
  - a selection matches no vertices at all, which means the box is wrong and
    the part would be a bone with nothing on it

A refusal is not a failure: the caller keeps the unmodified model and the
weapon behaves exactly as it does today.
====================
*/
size_t Mod_StudioSurgery( const void *in, size_t inlen, void *out, size_t outcap,
	const mstudiosynth_t *synths, int nsynth, const char **err );

#endif // MOD_SURGERY_H
