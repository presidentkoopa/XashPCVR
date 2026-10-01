/*
 * What will the engine fingerprint this model as - and will a card bind to it?
 *
 * This exists because the answer was wrong for every model that gets mesh
 * surgery. Mod_StudioFingerprint used to read the LOADED model through
 * Mod_StudioExtradata, and Mod_LoadStudioModel replaces that cache with the
 * surgery's grown buffer during load. So the fingerprint reported the grown
 * bone count and a hash containing the synthetic bone's name, while the card
 * declared what the generator measured from the file on disk, and the card was
 * refused. Worse, the surgery still ran - it is driven by a deliberately
 * unchecked query - so the player got a carved mesh with vanilla behaviour.
 *
 * The fix was to read the shipped bytes. This tool reads them the same way, in
 * C, through the same vrfingerprint.h the engine, the game DLL and the card
 * generator all share - so agreeing with it is evidence that all four agree.
 * Being a separate program that touches no engine state, it also cannot be
 * fooled by the bug it was written to catch.
 *
 *   vrfingerprint_check <model.mdl> [<card>]
 *
 * With a card, it compares and sets the exit code, so it can gate a commit.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../common/vrfingerprint.h"

#define IDSTUDIOHEADER  (('T'<<24)+('S'<<16)+('D'<<8)+'I')
#define STUDIO_VERSION  10
#define MAXSTUDIOBONES  128

/* Offsets into studiohdr_t. Written out in full, with the running total, because
 * this project has now misread this header THREE times: numseq once read from
 * 180 (numtextures), and numbones from 156 (numhitboxes) in the first draft of
 * this very file - which reported 8 bones for a 41-bone SAW while getting numseq
 * exactly right, because only one of the two offsets was wrong.
 *
 *     0  ident                 4
 *     4  version               4
 *     8  name[64]             64
 *    72  length                4
 *    76  eyeposition          12
 *    88  min                  12
 *   100  max                  12
 *   112  bbmin                12
 *   124  bbmax                12
 *   136  flags                 4
 *   140  numbones              4   <-- here
 *   144  boneindex             4   <--
 *   148  numbonecontrollers    4
 *   152  bonecontrollerindex   4
 *   156  numhitboxes           4   <-- NOT numbones
 *   160  hitboxindex           4
 *   164  numseq                4   <-- here
 *   168  seqindex              4
 *
 * Cross-checked against engine/studio.h and against vrcardgen.py:41-42, which
 * is a separate implementation in another language that has been measuring
 * these models correctly all along. When in doubt, agree with the generator. */
#define H_IDENT         0
#define H_VERSION       4
#define H_NUMBONES      140
#define H_BONEINDEX     144
#define H_NUMSEQ        164

#define BONE_STRIDE     112     /* sizeof( mstudiobone_t ) */

static int rd_i32( const unsigned char *d, size_t off )
{
	return (int)( d[off] | ( d[off+1] << 8 ) | ( d[off+2] << 16 ) | ( (unsigned)d[off+3] << 24 ));
}

int main( int argc, char **argv )
{
	FILE          *f;
	unsigned char *buf;
	long           len;
	int            numbones, boneindex, numseq, i;
	size_t         need;
	unsigned int   h = VR_FINGERPRINT_BASIS;

	if( argc < 2 )
	{
		fprintf( stderr, "usage: vrfingerprint_check <model.mdl> [<card>]\n" );
		return 2;
	}

	f = fopen( argv[1], "rb" );

	if( !f )
	{
		fprintf( stderr, "cannot open %s\n", argv[1] );
		return 2;
	}

	fseek( f, 0, SEEK_END );
	len = ftell( f );
	fseek( f, 0, SEEK_SET );

	if( len < 244 )
	{
		fprintf( stderr, "%s is too short to be a studio model\n", argv[1] );
		fclose( f );
		return 2;
	}

	buf = (unsigned char *)malloc( (size_t)len );

	if( !buf || fread( buf, 1, (size_t)len, f ) != (size_t)len )
	{
		fprintf( stderr, "cannot read %s\n", argv[1] );
		free( buf );
		fclose( f );
		return 2;
	}

	fclose( f );

	if( rd_i32( buf, H_IDENT ) != IDSTUDIOHEADER || rd_i32( buf, H_VERSION ) != STUDIO_VERSION )
	{
		fprintf( stderr, "%s is not a v10 studio model\n", argv[1] );
		free( buf );
		return 2;
	}

	numbones  = rd_i32( buf, H_NUMBONES );
	boneindex = rd_i32( buf, H_BONEINDEX );
	numseq    = rd_i32( buf, H_NUMSEQ );

	if( numbones <= 0 || numbones > MAXSTUDIOBONES || boneindex < 244 )
	{
		fprintf( stderr, "%s has an implausible bone table (%d bones at %d)\n",
			argv[1], numbones, boneindex );
		free( buf );
		return 2;
	}

	need = (size_t)boneindex + (size_t)numbones * BONE_STRIDE;

	if( need > (size_t)len )
	{
		fprintf( stderr, "%s claims %d bones that do not fit in %ld bytes\n",
			argv[1], numbones, len );
		free( buf );
		return 2;
	}

	/* The bone name is the first 32 bytes of mstudiobone_t. */
	for( i = 0; i < numbones; i++ )
		h = VR_HashBoneName( h, (const char *)( buf + boneindex + (size_t)i * BONE_STRIDE ));

	free( buf );

	printf( "%s\n", argv[1] );
	printf( "  match  bones %d  seqs %d  hash 0x%08x\n", numbones, numseq, h );

	if( argc >= 3 )
	{
		FILE *c = fopen( argv[2], "r" );
		char  line[1024];
		int   cb = -1, cs = -1;
		unsigned int ch = 0;
		int   found = 0;

		if( !c )
		{
			fprintf( stderr, "cannot open card %s\n", argv[2] );
			return 2;
		}

		while( fgets( line, sizeof( line ), c ))
		{
			char *p = line;

			while( *p == ' ' || *p == '\t' ) p++;

			if( strncmp( p, "match", 5 ))
				continue;

			if( sscanf( p, "match bones %d seqs %d hash 0x%x", &cb, &cs, &ch ) == 3 )
			{
				found = 1;
				break;
			}
		}

		fclose( c );

		if( !found )
		{
			printf( "  card %s has no parsable match line\n", argv[2] );
			return 1;
		}

		printf( "  card   bones %d  seqs %d  hash 0x%08x\n", cb, cs, ch );

		/* This is VRCard_Matches: bones must agree, seqs only when the card
		 * states one, and the hash must agree. */
		if( cb != numbones || ( cs && cs != numseq ) || ch != h )
		{
			printf( "  WILL NOT BIND\n" );
			return 1;
		}

		printf( "  binds\n" );
	}

	return 0;
}
