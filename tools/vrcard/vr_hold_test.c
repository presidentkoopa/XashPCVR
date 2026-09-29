/*
 * Headless test for vr_hold.
 *
 * Part F makes claims that can be checked without a headset: a pistol
 * follows the hand within a millimetre or two, a launcher visibly lags, two
 * hands steady it, recoil climbs and comes back, and nothing ever trails the
 * hand by more than three centimetres. Each of those is a case here.
 *
 * One Half-Life unit is about an inch, so a millimetre is about 0.04 units
 * and three centimetres about 1.2.
 */
#include <stdio.h>
#include <math.h>
#include <string.h>

#include "vr_hold.h"

static int g_fail = 0;
static const char *g_case = "";

static vrhold_t    H;
static vrholdcfg_t CFG;

static void ok( void ) { printf( "  ok   %s\n", g_case ); }

static void fail( const char *what, double got, double want )
{
	printf( "  FAIL %s\n       %s: %.4f, wanted %.4f\n", g_case, what, got, want );
	g_fail++;
}

static const float IDENT[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

/* How far the weapon is turned away from the hand's orientation, in radians.
 * Muzzle climb is a rotation; the couple of millimetres a gun travels
 * backwards in a firm grip is not the part anybody sees. */
static float tilt( void )
{
	float d[4], s, a;

	d[3] = H.quat[3] * IDENT[3] + H.quat[0] * IDENT[0]
		+ H.quat[1] * IDENT[1] + H.quat[2] * IDENT[2];

	if( d[3] < 0.0f ) d[3] = -d[3];
	if( d[3] > 1.0f ) d[3] = 1.0f;

	a = 2.0f * (float)acos( d[3] );
	s = 0.0f;
	(void)s;
	return a;
}

static void setup( float mass )
{
	float p[3] = { 0.0f, 0.0f, 0.0f };

	VRHold_DefaultCfg( &CFG, mass );
	VRHold_Reset( &H, p, IDENT );
}

/* Hold the hand still at (x,0,0) for a stretch of time, and report how far
 * the weapon ends up from it. */
static float settle( float x, int hands, float seconds )
{
	float t[3];
	float dt = 1.0f / 90.0f;
	int i, n = (int)( seconds / dt );

	t[0] = x; t[1] = 0.0f; t[2] = 0.0f;

	for( i = 0; i < n; i++ )
		VRHold_Step( &H, &CFG, t, IDENT, hands, 0, dt );

	return VRHold_Separation( &H, t );
}

/* Sweep the hand at a steady speed and report the trailing distance. */
static float sweep( float speed, int hands, float seconds )
{
	float t[3] = { 0.0f, 0.0f, 0.0f };
	float dt = 1.0f / 90.0f;
	int i, n = (int)( seconds / dt );

	for( i = 0; i < n; i++ )
	{
		t[0] += speed * dt;
		VRHold_Step( &H, &CFG, t, IDENT, hands, 0, dt );
	}

	return VRHold_Separation( &H, t );
}

/* ------------------------------------------------------------------ */

static void t_a_still_hand_holds_a_still_gun( void )
{
	float d;

	g_case = "a gun held still sits exactly where the hand is";
	setup( 1.0f );
	d = settle( 0.0f, 1, 1.0f );

	if( d > 0.001f )
		fail( "separation", d, 0.0 );
	ok();
}

static void t_a_pistol_follows_closely( void )
{
	float d;

	g_case = "a pistol follows a moving hand within a millimetre or two";
	setup( 0.9f );                  /* a loaded Glock, near enough */

	/* a brisk but ordinary hand movement: about a metre a second */
	d = sweep( 40.0f, 1, 1.0f );

	if( d > 0.08f )                 /* 0.08 units is about two millimetres */
		fail( "trailing distance", d, 0.08 );
	ok();
}

static void t_a_launcher_visibly_lags( void )
{
	float pistol, launcher;

	g_case = "a launcher lags where a pistol does not";

	setup( 0.9f );
	pistol = sweep( 40.0f, 1, 1.0f );

	setup( 9.0f );                  /* an RPG, ten times the mass */
	launcher = sweep( 40.0f, 1, 1.0f );

	if( launcher <= pistol * 2.0f )
		fail( "launcher lag against pistol lag", launcher, pistol * 2.0 );

	printf( "       (pistol %.3f units, launcher %.3f units)\n", pistol, launcher );
	ok();
}

static void t_nothing_ever_lags_further_than_the_cap( void )
{
	float d;
	int i;

	g_case = "nothing trails the hand by more than three centimetres";

	/* something absurd, swung absurdly fast */
	setup( 40.0f );

	for( i = 0; i < 10; i++ )
	{
		d = sweep( 400.0f, 1, 0.5f );

		if( d > CFG.max_lag + 0.001f )
		{
			fail( "trailing distance", d, CFG.max_lag );
			break;
		}
	}
	ok();
}

static void t_two_hands_steady_it( void )
{
	float one, two;

	g_case = "a second hand steadies the weapon";

	setup( 3.0f );
	one = sweep( 80.0f, 1, 1.0f );

	setup( 3.0f );
	two = sweep( 80.0f, 2, 1.0f );

	if( two >= one )
		fail( "two-handed lag against one-handed", two, one );

	printf( "       (one hand %.3f units, two hands %.3f units)\n", one, two );
	ok();
}

static void t_recoil_climbs_and_comes_back( void )
{
	float fwd[3] = { 1.0f, 0.0f, 0.0f };
	float t[3] = { 0.0f, 0.0f, 0.0f };
	float dt = 1.0f / 90.0f;
	float peak = 0.0f, after;
	int i;

	g_case = "recoil throws the gun and the springs bring it back";
	setup( 2.0f );

	VRHold_Recoil( &H, &CFG, fwd );

	/* MUZZLE CLIMB IS A ROTATION. The couple of millimetres a gun travels
	 * backwards in a firm grip is not the part anybody sees, and measuring
	 * that instead is how this test first claimed recoil did nothing. */
	for( i = 0; i < 90; i++ )       /* a second */
	{
		float d = tilt();

		if( d > peak )
			peak = d;

		VRHold_Step( &H, &CFG, t, IDENT, 1, 0, dt );
	}

	after = tilt();

	if( peak < 0.02f )              /* at least about a degree of climb */
		fail( "peak muzzle climb, radians", peak, 0.02 );

	if( after > 0.005f )
		fail( "settled back to", after, 0.0 );

	printf( "       (climb %.3f rad, settled %.4f)\n", peak, after );
	ok();
}

static void t_a_heavier_gun_recoils_less( void )
{
	float fwd[3] = { 1.0f, 0.0f, 0.0f };
	float t[3] = { 0.0f, 0.0f, 0.0f };
	float light, heavy;
	int i;

	g_case = "the same impulse moves a heavy weapon less";

	setup( 1.0f );
	VRHold_Recoil( &H, &CFG, fwd );
	light = 0.0f;
	for( i = 0; i < 30; i++ )
	{
		float d = tilt();
		if( d > light ) light = d;
		VRHold_Step( &H, &CFG, t, IDENT, 1, 0, 1.0f / 90.0f );
	}

	setup( 8.0f );
	VRHold_Recoil( &H, &CFG, fwd );
	heavy = 0.0f;
	for( i = 0; i < 30; i++ )
	{
		float d = tilt();
		if( d > heavy ) heavy = d;
		VRHold_Step( &H, &CFG, t, IDENT, 1, 0, 1.0f / 90.0f );
	}

	if( heavy >= light )
		fail( "heavy peak against light peak", heavy, light );

	printf( "       (1kg %.3f units, 8kg %.3f units)\n", light, heavy );
	ok();
}

static void t_a_hitch_does_not_launch_it( void )
{
	float t[3] = { 100.0f, 0.0f, 0.0f };
	float d;

	g_case = "a dropped second does not fling the gun across the room";
	setup( 1.0f );

	/* the hand teleports a hundred units away and a whole second passes */
	VRHold_Step( &H, &CFG, t, IDENT, 1, 0, 1.0f );

	d = VRHold_Separation( &H, t );

	/* it may not have arrived, but it must not have overshot either */
	if( d > 100.0f )
		fail( "separation after the hitch", d, 100.0 );

	if( H.pos[0] > 101.0f )
		fail( "overshoot", H.pos[0], 100.0 );
	ok();
}

static void t_being_blocked_stops_it( void )
{
	float at[3] = { 5.0f, 0.0f, 0.0f };
	float n[3] = { -1.0f, 0.0f, 0.0f };
	float t[3] = { 20.0f, 0.0f, 0.0f };

	g_case = "a wall stops the gun and it slides rather than grinding in";
	setup( 1.0f );

	sweep( 200.0f, 1, 0.2f );
	VRHold_Blocked( &H, at, n );

	if( H.pos[0] != 5.0f )
		fail( "held at the wall", H.pos[0], 5.0 );

	if( H.vel[0] > 0.001f )
		fail( "velocity into the wall", H.vel[0], 0.0 );

	(void)t;
	ok();
}

int main( void )
{
	printf( "vr_hold, headless:\n" );

	t_a_still_hand_holds_a_still_gun();
	t_a_pistol_follows_closely();
	t_a_launcher_visibly_lags();
	t_nothing_ever_lags_further_than_the_cap();
	t_two_hands_steady_it();
	t_recoil_climbs_and_comes_back();
	t_a_heavier_gun_recoils_less();
	t_a_hitch_does_not_launch_it();
	t_being_blocked_stops_it();

	printf( "\n%s\n", g_fail ? "FAILURES ABOVE" : "all cases pass" );
	return g_fail ? 1 : 0;
}
