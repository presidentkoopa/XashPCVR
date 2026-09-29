/*
vrfingerprint.h - identifying the model a weapon card was measured from
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
#ifndef VRFINGERPRINT_H
#define VRFINGERPRINT_H

/*
====================
ONE DEFINITION, BOTH SIDES.

A weapon card records a hash of the bone names of the model it was measured
from, and applies only to a model whose bones hash the same. The ENGINE
computes that hash from a loaded model; the GAME DLL compares it against what
the card recorded; the GENERATOR writes it. Three callers, and if any two of
them disagreed about the hash by so much as a separator byte, every card
would silently stop binding and every weapon would quietly fall back.

So it is one inline function in one header rather than three implementations
that look alike. It is header-only and dependency-free for the same reason
vrcmd.h is: a game DLL has no engine tree to include from.

BINDING BY FINGERPRINT IS THE WHOLE POINT. Applying the first card whose bones
a model happens to have will misfire - `Bone01` and `Bone02` are generic 3ds
Max names present in many rigs, so Blue Shift's shotgun (pump on `Bone02`)
would match an SD pistol card (slide on `Bone02`) and drive the wrong bone.
Half-Life's own `valve` directory holds two entirely different rigs, because
the HD models are an overlay mounted over the same paths.
====================
*/

// FNV-1a. Chosen because it is four lines, has no table, and cannot be got
// subtly wrong in a second implementation - not because it is the strongest
// hash available. This guards against coincidence, never against malice: a
// card is a local file, and anyone who can write one can already write worse.
#define VR_FINGERPRINT_BASIS    2166136261U
#define VR_FINGERPRINT_PRIME    16777619U

static inline unsigned int VR_HashBoneName( unsigned int h, const char *name )
{
	int i;

	for( i = 0; name && name[i]; i++ )
	{
		h ^= (unsigned char)name[i];
		h *= VR_FINGERPRINT_PRIME;
	}

	// A separator, so the rigs ("AB","C") and ("A","BC") do not hash alike.
	// Without it a renamed bone that merely moved a character across a
	// boundary would still match, which is exactly the near-miss this is
	// here to catch.
	h ^= 0xFF;
	h *= VR_FINGERPRINT_PRIME;

	return h;
}

#endif // VRFINGERPRINT_H
