/*
mod_surgery.c - giving a studio model a bone the animators never made
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

/*
	See mod_surgery.h for what this is for and why it is shaped this way.

	No engine calls and no allocator: the caller provides the buffer. That is
	not fastidiousness - it is what lets the whole transformation be driven
	from a test harness over a real .mdl file and the result checked
	independently, which is the only way anybody is going to trust a pass that
	rewrites model data.
*/

#include <string.h>
#include <stdio.h>

#include "mod_surgery.h"

/* ------------------------------------------------------------------ */
/* the format, spelled out                                             */
/*                                                                     */
/* Named offsets rather than a mapped struct, so this file needs no    */
/* engine headers and can be compiled on its own by the test harness.  */
/* ------------------------------------------------------------------ */

#define H_ID            0
#define H_VERSION       4
#define H_LENGTH        72
#define H_NUMBONES      140
#define H_BONEINDEX     144
#define H_NUMSEQ        164
#define H_SEQINDEX      168
#define H_NUMSEQGROUPS  172
#define H_NUMBODYPARTS  204
#define H_BODYPARTINDEX 208

#define BONE_SIZE       112
#define BONE_NAME       0
#define BONE_PARENT     32
#define BONE_FLAGS      36
#define BONE_CONTROLLER 40
#define BONE_VALUE      64      // float[6]: x y z, then rotation
#define BONE_SCALE      88      // float[6]

#define SEQ_SIZE        176
#define SEQ_NUMFRAMES   56
#define SEQ_NUMBLENDS   120
#define SEQ_ANIMINDEX   124
#define SEQ_SEQGROUP    156

#define BODYPART_SIZE   76
#define BODYPART_NUMMODELS  64
#define BODYPART_MODELINDEX 72

#define MODEL_SIZE      112
#define MODEL_NUMVERTS      80
#define MODEL_VERTINFOINDEX 84
#define MODEL_VERTINDEX     88

#define ANIM_SIZE       12      // mstudioanim_t: uint16 offset[6]

#define STUDIO_VERSION  10
#define MAX_BONES       128     // MAXSTUDIOBONES

// The units a written animation value carries. 1/64 of a unit is finer than
// studiomdl's own default and keeps a twenty-unit magazine's raw values
// inside a signed short with room to spare.
#define SYNTH_SCALE     ( 1.0f / 64.0f )

// A radian is a big quantity where a unit is a small one, so a rotation
// written at the position scale would come out visibly notched. This is
// finer by a factor of sixteen, which keeps a ninety-degree swing well
// inside a signed short.
#define SYNTH_ROT_SCALE ( 1.0f / 1024.0f )

#define FAIL( msg )     do { if( err ) *err = ( msg ); return 0; } while( 0 )

static int RdI( const unsigned char *d, size_t o )
{
	int v;
	memcpy( &v, d + o, 4 );
	return v;
}

static void WrI( unsigned char *d, size_t o, int v )
{
	memcpy( d + o, &v, 4 );
}

static float RdF( const unsigned char *d, size_t o )
{
	float v;
	memcpy( &v, d + o, 4 );
	return v;
}

static void WrF( unsigned char *d, size_t o, float v )
{
	memcpy( d + o, &v, 4 );
}

static unsigned short RdU16( const unsigned char *d, size_t o )
{
	unsigned short v;
	memcpy( &v, d + o, 2 );
	return v;
}

static void WrU16( unsigned char *d, size_t o, unsigned short v )
{
	memcpy( d + o, &v, 2 );
}

static size_t Align4( size_t x )
{
	return ( x + 3 ) & ~(size_t)3;
}

/*
=====================
Surgery_FindBone
=====================
*/
static int Surgery_FindBone( const unsigned char *d, const char *name )
{
	int i, numbones = RdI( d, H_NUMBONES );
	size_t base = (size_t)RdI( d, H_BONEINDEX );

	for( i = 0; i < numbones; i++ )
	{
		const char *bn = (const char *)( d + base + (size_t)i * BONE_SIZE + BONE_NAME );

		if( !strncmp( bn, name, MOD_SYNTH_NAME_LEN ))
			return i;
	}

	return -1;
}

/*
=====================
Surgery_AnimExtent

How far one sequence's animation data actually reaches.

Walked rather than assumed. The obvious shortcut - "it ends where the next
sequence begins" - is only true while sequences are laid out in order and
nothing shares data, and a model that broke either assumption would have its
tail silently truncated. Walking every run of every channel is exact, and a
few thousand bytes of arithmetic at load time costs nothing.

Returns 0 if any offset points outside the file, which is the caller's cue to
refuse the model rather than read it.
=====================
*/
static size_t Surgery_AnimExtent( const unsigned char *d, size_t len,
	size_t block, int nentries, int numframes )
{
	size_t reach = block + (size_t)nentries * ANIM_SIZE;
	int i, j;

	for( i = 0; i < nentries; i++ )
	{
		size_t ent = block + (size_t)i * ANIM_SIZE;

		if( ent + ANIM_SIZE > len )
			return 0;

		for( j = 0; j < 6; j++ )
		{
			unsigned short off = RdU16( d, ent + (size_t)j * 2 );
			size_t p;
			int k;

			if( !off )
				continue;

			p = ent + off;

			// Walk the runs the way the decoder does: each run is one
			// (valid, total) pair followed by `valid` values, and covers
			// `total` frames.
			for( k = 0; k < numframes; )
			{
				int valid, total;

				if( p + 2 > len )
					return 0;

				valid = d[p];
				total = d[p + 1];

				if( total <= 0 )
					return 0;

				p += (size_t)( valid + 1 ) * 2;

				if( p > len )
					return 0;

				k += total;
			}

			if( p > reach )
				reach = p;
		}
	}

	return reach;
}

/*
=====================
Mod_StudioSurgery
=====================
*/
size_t Mod_StudioSurgery( const void *in, size_t inlen, void *out, size_t outcap,
	const mstudiosynth_t *synths, int nsynth, const char **err )
{
	const unsigned char *src = (const unsigned char *)in;
	unsigned char *dst;
	int numbones, numseq, newbones, i, s;
	size_t need, cursor;
	size_t seq_block[256], seq_size[256];
	int seq_entries[256], seq_frames[256];
	int drive_seq = -1, drive_frames = 0;
	int from_bone[MOD_MAX_SYNTH];

	if( err )
		*err = NULL;

	if( !src || inlen < 256 || !synths || nsynth <= 0 )
		FAIL( "nothing to do" );

	if( nsynth > MOD_MAX_SYNTH )
		FAIL( "too many synthetic parts at once" );

	if( memcmp( src, "IDST", 4 ))
		FAIL( "not a studio model" );

	if( RdI( src, H_VERSION ) != STUDIO_VERSION )
		FAIL( "not a version 10 studio model" );

	numbones = RdI( src, H_NUMBONES );
	numseq = RdI( src, H_NUMSEQ );
	newbones = numbones + nsynth;

	if( numbones <= 0 || numseq < 0 )
		FAIL( "model has no bones" );

	if( newbones > MAX_BONES )
		FAIL( "would exceed the studio bone limit" );

	if( numseq > (int)( sizeof( seq_block ) / sizeof( seq_block[0] )))
		FAIL( "more sequences than this pass handles" );

	// Every synthetic part has to come from a bone that exists.
	for( s = 0; s < nsynth; s++ )
	{
		from_bone[s] = Surgery_FindBone( src, synths[s].from );

		if( from_bone[s] < 0 )
			FAIL( "the bone a synthetic part comes from is not in this model" );

		if( Surgery_FindBone( src, synths[s].name ) >= 0 )
			FAIL( "a bone by that name already exists" );
	}

	// ---- measure every sequence's animation block -----------------------
	for( i = 0; i < numseq; i++ )
	{
		size_t sq = (size_t)RdI( src, H_SEQINDEX ) + (size_t)i * SEQ_SIZE;
		int numframes, numblends, seqgroup;
		size_t extent;

		if( sq + SEQ_SIZE > inlen )
			FAIL( "sequence table runs past the end of the file" );

		seqgroup = RdI( src, sq + SEQ_SEQGROUP );

		// The doc is explicit about this one: a sequence living in an
		// external .mdl has no bytes here to grow, and silently leaving it
		// with the old bone count would desynchronise every bone after it.
		if( seqgroup != 0 )
			FAIL( "model uses external sequence-group files" );

		numframes = RdI( src, sq + SEQ_NUMFRAMES );
		numblends = RdI( src, sq + SEQ_NUMBLENDS );

		if( numblends <= 0 )
			numblends = 1;

		seq_block[i] = (size_t)RdI( src, sq + SEQ_ANIMINDEX );
		seq_entries[i] = numblends * numbones;
		seq_frames[i] = numframes;

		// The sequence a synthetic part's motion is written into: the
		// longest one, so the travel is finely sampled, and a single one, so
		// the engine's "which sequence moves this bone furthest" measurement
		// has an unambiguous answer.
		if( numframes > drive_frames )
		{
			drive_frames = numframes;
			drive_seq = i;
		}

		if( numframes <= 0 )
		{
			seq_size[i] = (size_t)seq_entries[i] * ANIM_SIZE;
			continue;
		}

		extent = Surgery_AnimExtent( src, inlen, seq_block[i], seq_entries[i], numframes );

		if( !extent )
			FAIL( "animation data points outside the file" );

		seq_size[i] = extent - seq_block[i];
	}

	// ---- how big the grown copy has to be -------------------------------
	//
	// The original, verbatim, plus a new bone array and a new animation
	// block per sequence, appended past the end. Nothing is inserted, so
	// nothing that already exists has to move.
	need = Align4( inlen );
	need += (size_t)newbones * BONE_SIZE;

	for( i = 0; i < numseq; i++ )
	{
		size_t sq = (size_t)RdI( src, H_SEQINDEX ) + (size_t)i * SEQ_SIZE;
		int numblends = RdI( src, sq + SEQ_NUMBLENDS );

		if( numblends <= 0 )
			numblends = 1;

		need = Align4( need );
		need += (size_t)( numblends * newbones ) * ANIM_SIZE;
		need += seq_size[i] - (size_t)seq_entries[i] * ANIM_SIZE;

		// ...and the written animation, where it goes. Three channels per
		// part, each a header pair plus one value a frame, plus a run header
		// every 255 frames because both counts are bytes.
		if( i == drive_seq )
		{
			need += (size_t)nsynth * 3 *
				( (size_t)drive_frames * 2 + ( (size_t)drive_frames / 255 + 2 ) * 2 );
		}
	}

	need = Align4( need );

	if( !out )
		return need;

	if( outcap < need )
		FAIL( "output buffer too small" );

	dst = (unsigned char *)out;
	memset( dst, 0, need );
	memcpy( dst, src, inlen );

	cursor = Align4( inlen );

	// ---- the bone array, grown ------------------------------------------
	{
		size_t oldbase = (size_t)RdI( src, H_BONEINDEX );
		size_t newbase = cursor;

		memcpy( dst + newbase, src + oldbase, (size_t)numbones * BONE_SIZE );

		for( s = 0; s < nsynth; s++ )
		{
			size_t b = newbase + (size_t)( numbones + s ) * BONE_SIZE;
			int k;

			memset( dst + b, 0, BONE_SIZE );
			strncpy( (char *)( dst + b + BONE_NAME ), synths[s].name, MOD_SYNTH_NAME_LEN - 1 );

			// Parented to the bone its vertices came from, with the origin
			// at the declared pivot and NO ROTATION - which is what makes
			// moving a vertex across a subtraction, and what keeps the
			// normals correct without touching them.
			WrI( dst, b + BONE_PARENT, from_bone[s] );

			for( k = 0; k < 6; k++ )
				WrI( dst, b + BONE_CONTROLLER + (size_t)k * 4, -1 );

			WrF( dst, b + BONE_VALUE + 0, synths[s].pivot[0] );
			WrF( dst, b + BONE_VALUE + 4, synths[s].pivot[1] );
			WrF( dst, b + BONE_VALUE + 8, synths[s].pivot[2] );

			// The scale a written animation value is multiplied by. It has
			// to be non-zero on the channels the ramp will use, or every
			// value decodes back to the rest pose - which is precisely the
			// bug a bone with no animation has, and the one this exists to
			// fix.
			//
			// Rotation gets a finer scale than position: a radian is a
			// large quantity where a unit is a small one, and the same step
			// would make a hinge visibly notched.
			for( k = 0; k < 6; k++ )
			{
				int rot = ( k >= 3 );
				int used = synths[s].rotates ? rot : !rot;

				WrF( dst, b + BONE_SCALE + (size_t)k * 4,
					used ? ( rot ? SYNTH_ROT_SCALE : SYNTH_SCALE ) : 0.0f );
			}
		}

		WrI( dst, H_BONEINDEX, (int)newbase );
		cursor = newbase + (size_t)newbones * BONE_SIZE;
	}

	// ---- one grown animation block per sequence -------------------------
	for( i = 0; i < numseq; i++ )
	{
		size_t sq = (size_t)RdI( src, H_SEQINDEX ) + (size_t)i * SEQ_SIZE;
		int numblends = RdI( src, sq + SEQ_NUMBLENDS );
		size_t oldblock = seq_block[i];
		size_t newblock, olddata, newdata, datasize;
		int b, k;

		if( numblends <= 0 )
			numblends = 1;

		cursor = Align4( cursor );
		newblock = cursor;

		olddata = (size_t)seq_entries[i] * ANIM_SIZE;
		newdata = (size_t)( numblends * newbones ) * ANIM_SIZE;
		datasize = seq_size[i] - olddata;

		// The compressed values themselves are copied untouched. Only the
		// offsets that reach them are recomputed - which is the whole reason
		// this pass needs no re-encoding, and why a model with animation
		// this code does not understand still comes out intact.
		memcpy( dst + newblock + newdata, src + oldblock + olddata, datasize );

		for( b = 0; b < numblends; b++ )
		{
			for( k = 0; k < numbones; k++ )
			{
				size_t olde = oldblock + (size_t)( b * numbones + k ) * ANIM_SIZE;
				size_t newe = newblock + (size_t)( b * newbones + k ) * ANIM_SIZE;
				int j;

				for( j = 0; j < 6; j++ )
				{
					unsigned short off = RdU16( src, olde + (size_t)j * 2 );
					size_t d, no;

					if( !off )
					{
						WrU16( dst, newe + (size_t)j * 2, 0 );
						continue;
					}

					// Where in the value data this channel started...
					d = ( olde + off ) - ( oldblock + olddata );

					// ...and how far that now is from the entry's new home.
					no = ( newblock + newdata + d ) - newe;

					if( no > 0xFFFF )
						FAIL( "a rewritten animation offset will not fit in 16 bits" );

					WrU16( dst, newe + (size_t)j * 2, (unsigned short)no );
				}
			}

			// The synthetic bones' entries. All-zero offsets, which GoldSrc
			// reads as "use the bone's default value", so the part sits at
			// its pivot - correct in every sequence except the one that
			// drives it, filled in below.
			for( s = 0; s < nsynth; s++ )
			{
				size_t newe = newblock + (size_t)( b * newbones + numbones + s ) * ANIM_SIZE;
				memset( dst + newe, 0, ANIM_SIZE );
			}
		}

		cursor = newblock + newdata + datasize;

		// ---- the written animation ---------------------------------
		//
		// A real part is measured: the engine finds the sequence that moves
		// its bone furthest and brackets the travel. A synthetic part has no
		// animation to measure, so one is written here - and from that point
		// on it is measured, posed and driven by exactly the same code as
		// every part the artist made. That is what keeps one posing path
		// rather than two.
		if( i == drive_seq && drive_frames > 1 )
		{
			for( s = 0; s < nsynth; s++ )
			{
				size_t newe = newblock + (size_t)( numbones + s ) * ANIM_SIZE;
				int ch;

				if( synths[s].travel == 0.0f )
					continue;

				for( ch = 0; ch < 3; ch++ )
				{
					size_t at, delta;
					int f, left;
					int chan = synths[s].rotates ? ( 3 + ch ) : ch;
					float scale = synths[s].rotates ? SYNTH_ROT_SCALE : SYNTH_SCALE;

					if( synths[s].axis[ch] == 0.0f )
						continue;

					cursor = Align4( cursor );
					at = cursor;
					delta = at - newe;

					if( delta > 0xFFFF )
						FAIL( "no room to write a synthetic part's animation" );

					// Runs of at most 255 frames, because `valid` and
					// `total` are each a single byte.
					f = 0;
					while( f < drive_frames )
					{
						left = drive_frames - f;
						if( left > 255 )
							left = 255;

						dst[cursor++] = (unsigned char)left;   // valid
						dst[cursor++] = (unsigned char)left;   // total

						for( ; left > 0; left--, f++ )
						{
							float t = (float)f / (float)( drive_frames - 1 );
							float v = synths[s].axis[ch] * synths[s].travel * t;
							int raw = (int)( v / scale );

							if( raw > 32767 )  raw = 32767;
							if( raw < -32768 ) raw = -32768;

							WrU16( dst, cursor, (unsigned short)(short)raw );
							cursor += 2;
						}
					}

					WrU16( dst, newe + (size_t)chan * 2, (unsigned short)delta );
				}
			}
		}

		WrI( dst, sq + SEQ_ANIMINDEX, (int)newblock );
	}

	// ---- move the vertices across ---------------------------------------
	for( s = 0; s < nsynth; s++ )
	{
		int newindex = numbones + s;
		int moved = 0;
		int bp;

		for( bp = 0; bp < RdI( src, H_NUMBODYPARTS ); bp++ )
		{
			size_t o = (size_t)RdI( src, H_BODYPARTINDEX ) + (size_t)bp * BODYPART_SIZE;
			int nummodels = RdI( src, o + BODYPART_NUMMODELS );
			size_t modelindex = (size_t)RdI( src, o + BODYPART_MODELINDEX );
			int m;

			for( m = 0; m < nummodels; m++ )
			{
				size_t mo = modelindex + (size_t)m * MODEL_SIZE;
				int numverts = RdI( src, mo + MODEL_NUMVERTS );
				size_t vi = (size_t)RdI( src, mo + MODEL_VERTINFOINDEX );
				size_t vx = (size_t)RdI( src, mo + MODEL_VERTINDEX );
				int v;

				if( vi + (size_t)numverts > inlen
					|| vx + (size_t)numverts * 12 > inlen )
					FAIL( "vertex data points outside the file" );

				for( v = 0; v < numverts; v++ )
				{
					float p[3];
					int a, inside = 1;

					if( dst[vi + v] != from_bone[s] )
						continue;

					for( a = 0; a < 3; a++ )
						p[a] = RdF( dst, vx + (size_t)v * 12 + (size_t)a * 4 );

					for( a = 0; a < 3; a++ )
					{
						if( p[a] < synths[s].box_min[a] || p[a] > synths[s].box_max[a] )
							inside = 0;
					}

					if( !inside )
						continue;

					// Re-expressed relative to the new bone's origin. A pure
					// subtraction, because the new bone has no rotation of
					// its own - see the header.
					for( a = 0; a < 3; a++ )
						WrF( dst, vx + (size_t)v * 12 + (size_t)a * 4, p[a] - synths[s].pivot[a] );

					dst[vi + v] = (unsigned char)newindex;
					moved++;
				}
			}
		}

		// A selection that caught nothing means the box is wrong, and the
		// result would be a bone with no geometry on it - a part that exists
		// to the simulator and is invisible to the player. Refuse instead.
		if( !moved )
			FAIL( "a synthetic part's box selected no vertices" );
	}

	WrI( dst, H_NUMBONES, newbones );
	WrI( dst, H_LENGTH, (int)need );

	return need;
}
