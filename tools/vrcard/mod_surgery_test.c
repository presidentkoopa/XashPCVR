/*
 * mod_surgery_test - run mesh surgery over a real model and write the result.
 *
 * The transformation rewrites model data, so it gets checked against a real
 * .mdl rather than a fixture: load a view model, give it a new bone, write
 * the grown copy out, and let something that is not this code read the
 * result back. vrcardgen.py is that something - a separate implementation of
 * the same format, in a different language, which will not share a mistake
 * with the C.
 *
 * Usage:
 *   mod_surgery_test <in.mdl> <out.mdl> <from-bone> <name> \
 *       <minx miny minz> <maxx maxy maxz> <pivx pivy pivz>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mod_surgery.h"

static unsigned char *slurp( const char *path, size_t *len )
{
	FILE *fh = fopen( path, "rb" );
	unsigned char *buf;
	long n;

	if( !fh )
		return NULL;

	fseek( fh, 0, SEEK_END );
	n = ftell( fh );
	fseek( fh, 0, SEEK_SET );

	buf = (unsigned char *)malloc( n );

	if( !buf || fread( buf, 1, n, fh ) != (size_t)n )
	{
		fclose( fh );
		free( buf );
		return NULL;
	}

	fclose( fh );
	*len = (size_t)n;
	return buf;
}

int main( int argc, char **argv )
{
	mstudiosynth_t synth;
	unsigned char *in, *out;
	size_t inlen = 0, need, wrote;
	const char *err = NULL;
	FILE *fh;
	int i;

	if( argc < 14 )
	{
		printf( "usage: %s <in.mdl> <out.mdl> <from-bone> <name> "
			"minx miny minz maxx maxy maxz pivx pivy pivz\n", argv[0] );
		return 2;
	}

	in = slurp( argv[1], &inlen );

	if( !in )
	{
		printf( "cannot read %s\n", argv[1] );
		return 1;
	}

	memset( &synth, 0, sizeof( synth ));
	strncpy( synth.from, argv[3], MOD_SYNTH_NAME_LEN - 1 );
	strncpy( synth.name, argv[4], MOD_SYNTH_NAME_LEN - 1 );

	for( i = 0; i < 3; i++ )
	{
		synth.box_min[i] = (float)atof( argv[5 + i] );
		synth.box_max[i] = (float)atof( argv[8 + i] );
		synth.pivot[i]   = (float)atof( argv[11 + i] );
		synth.axis[i]    = ( argc > 17 ) ? (float)atof( argv[14 + i] ) : 0.0f;
	}

	if( argc > 17 )
		synth.travel = (float)atof( argv[17] );

	// An 18th argument of "rot" makes it a hinge: `travel` is then radians.
	// "rewrite" replaces the source bone's own motion instead of carving a
	// new bone out of it.
	if( argc > 18 )
	{
		if( strstr( argv[18], "rot" ))     synth.rotates = 1;
		if( strstr( argv[18], "rewrite" )) synth.rewrite = 1;
	}

	need = Mod_StudioSurgery( in, inlen, NULL, 0, &synth, 1, &err );

	if( !need )
	{
		printf( "refused: %s\n", err ? err : "?" );
		free( in );
		return 1;
	}

	out = (unsigned char *)malloc( need );
	wrote = Mod_StudioSurgery( in, inlen, out, need, &synth, 1, &err );

	if( !wrote )
	{
		printf( "refused: %s\n", err ? err : "?" );
		free( in );
		free( out );
		return 1;
	}

	fh = fopen( argv[2], "wb" );

	if( !fh || fwrite( out, 1, wrote, fh ) != wrote )
	{
		printf( "cannot write %s\n", argv[2] );
		if( fh ) fclose( fh );
		free( in );
		free( out );
		return 1;
	}

	fclose( fh );

	printf( "%s -> %s\n", argv[1], argv[2] );
	printf( "  %lu bytes in, %lu out (+%lu)\n",
		(unsigned long)inlen, (unsigned long)wrote,
		(unsigned long)( wrote - inlen ));
	printf( "  new bone \"%s\" off \"%s\"\n", synth.name, synth.from );

	free( in );
	free( out );
	return 0;
}
