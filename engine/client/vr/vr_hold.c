/*
vr_hold.c - integrating the held weapon
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
	See vr_hold.h. No engine headers, so the integrator can be driven from a
	test harness - which is the only way any of this gets checked without a
	headset, and the only way the comfort limits get to be more than a hope.
*/

#include <math.h>
#include <string.h>

#include "vr_hold.h"

// A stiff spring integrated once over a whole frame is a spring that
// explodes. Four sub-steps at 90 Hz is about 2.8 ms each, which is where the
// plan puts it and comfortably stable for the stiffnesses below.
// NO SUBSTEP COUNT. There was a VRHOLD_SUBSTEPS here, defined as 4 and
// referenced by nothing, while the header claimed the step was sub-stepped
// "because a stiff spring integrated once over a long frame is a spring that
// explodes". VRHold_Smooth is the closed form of a critically damped spring
// and is stable at any step, so there was nothing to sub-step and the claim
// described code that was never written. VRHOLD_MAX_DT below is the real
// guard, against a different hazard: a frame long enough to teleport the gun.

// Above this a frame is a hitch rather than a frame, and integrating it is
// worse than skipping it: the gun would lunge. Half a second of nothing is
// less alarming than half a second of catching up at speed.
#define VRHOLD_MAX_DT       0.1f

static float VRHold_Dot( const float a[3], const float b[3] )
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static float VRHold_Len( const float v[3] )
{
	return (float)sqrt( VRHold_Dot( v, v ));
}

static void VRHold_Cross( const float a[3], const float b[3], float out[3] )
{
	out[0] = a[1] * b[2] - a[2] * b[1];
	out[1] = a[2] * b[0] - a[0] * b[2];
	out[2] = a[0] * b[1] - a[1] * b[0];
}

/*
=====================
VRHold_DefaultCfg

The numbers, and where they come from.

Stiffness is per unit of mass, so the same spring on a heavier weapon lags
more - which is the whole mechanism by which a launcher feels different from
a pistol, and why nobody has to tune two sets of numbers.

Damping is critical for the mass: 2*sqrt(k*m). A gun that overshoots its own
hand and swings back is a gun nobody can aim.
=====================
*/
void VRHold_DefaultCfg( vrholdcfg_t *cfg, float mass )
{
	if( !cfg )
		return;

	memset( cfg, 0, sizeof( *cfg ));

	if( mass < 0.2f )
		mass = 0.2f;

	cfg->mass = mass;

	// Inertia grows FASTER than mass, because a heavier weapon is also a
	// longer one and inertia goes as mass times length squared. That is not
	// a detail: with inertia merely proportional to mass, every term in the
	// muzzle climb cancels and a rocket launcher kicks exactly as hard as a
	// pistol. It is the length that makes a heavy weapon steady.
	cfg->inertia = mass * ( 0.6f + 0.35f * mass );

	// PROPORTIONAL TO MASS, and that is the whole mechanism by which a
	// launcher feels different from a pistol. A 0.9 kg pistol comes out at
	// two milliseconds and trails about two millimetres at ordinary hand
	// speeds; a 9 kg launcher comes out at twenty and trails a couple of
	// visible centimetres. Nobody tunes two sets of numbers.
	cfg->follow_time = 0.0022f * mass;
	cfg->turn_time = 0.0060f * mass;

	cfg->max_accel = 4000.0f;
	cfg->max_angaccel = 900.0f;

	// RECOIL IS A PROPERTY OF THE CARTRIDGE, not of the gun holding it. The
	// same round fired from a heavier weapon moves it less, which is the
	// whole reason anybody wants a heavy gun - so these are fixed numbers
	// that the weapon's own mass and inertia then divide into. A card
	// overrides them per weapon, because a .357 is not a 9mm.
	cfg->recoil_impulse = 26.0f;
	cfg->recoil_torque = 9.0f;

	// Three centimetres, which is what the plan caps it at. One Half-Life
	// unit is about an inch, so this is a little over one unit.
	cfg->max_lag = 1.2f;
	cfg->max_tilt = 0.35f;      // about twenty degrees
}

void VRHold_Reset( vrhold_t *h, const float pos[3], const float quat[4] )
{
	int i;

	if( !h )
		return;

	memset( h, 0, sizeof( *h ));

	for( i = 0; i < 3; i++ )
		h->pos[i] = pos ? pos[i] : 0.0f;

	if( quat )
	{
		for( i = 0; i < 4; i++ )
			h->quat[i] = quat[i];
	}
	else
	{
		h->quat[3] = 1.0f;
	}

	h->have = 1;
}

/*
=====================
VRHold_Twist

The shortest rotation from where the weapon is to where the hand has it, as
an axis times an angle.

Through the quaternion difference rather than through Euler angles, because
Euler angles have a pole and a gun pointed straight up is exactly where a
player will put one.
=====================
*/
static void VRHold_Twist( const float from[4], const float to[4], float out[3] )
{
	float inv[4], d[4], s, angle;
	int i;

	inv[0] = -from[0];
	inv[1] = -from[1];
	inv[2] = -from[2];
	inv[3] =  from[3];

	// d = to * inverse(from)
	d[3] = to[3] * inv[3] - to[0] * inv[0] - to[1] * inv[1] - to[2] * inv[2];
	d[0] = to[3] * inv[0] + to[0] * inv[3] + to[1] * inv[2] - to[2] * inv[1];
	d[1] = to[3] * inv[1] - to[0] * inv[2] + to[1] * inv[3] + to[2] * inv[0];
	d[2] = to[3] * inv[2] + to[0] * inv[1] - to[1] * inv[0] + to[2] * inv[3];

	// Both signs of a quaternion are the same rotation; the negative one is
	// the long way round. Take the short way, or the gun spins 359 degrees
	// to reach a pose one degree away.
	if( d[3] < 0.0f )
	{
		for( i = 0; i < 4; i++ )
			d[i] = -d[i];
	}

	s = (float)sqrt( d[0] * d[0] + d[1] * d[1] + d[2] * d[2] );

	if( s < 1e-6f )
	{
		out[0] = out[1] = out[2] = 0.0f;
		return;
	}

	if( d[3] > 1.0f )  d[3] = 1.0f;
	if( d[3] < -1.0f ) d[3] = -1.0f;

	angle = 2.0f * (float)acos( d[3] );

	for( i = 0; i < 3; i++ )
		out[i] = d[i] / s * angle;
}

static void VRHold_Normalise( float q[4] )
{
	float n = (float)sqrt( q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3] );
	int i;

	if( n < 1e-8f )
	{
		q[0] = q[1] = q[2] = 0.0f;
		q[3] = 1.0f;
		return;
	}

	for( i = 0; i < 4; i++ )
		q[i] /= n;
}

// Turn a quaternion by an angular velocity over dt.
static void VRHold_Spin( float q[4], const float w[3], float dt )
{
	float d[4];

	d[0] = 0.5f * dt * ( w[0] * q[3] + w[1] * q[2] - w[2] * q[1] );
	d[1] = 0.5f * dt * ( -w[0] * q[2] + w[1] * q[3] + w[2] * q[0] );
	d[2] = 0.5f * dt * ( w[0] * q[1] - w[1] * q[0] + w[2] * q[3] );
	d[3] = 0.5f * dt * ( -w[0] * q[0] - w[1] * q[1] - w[2] * q[2] );

	q[0] += d[0];
	q[1] += d[1];
	q[2] += d[2];
	q[3] += d[3];

	VRHold_Normalise( q );
}

static void VRHold_ClampLen( float v[3], float max )
{
	float n = VRHold_Len( v );
	int i;

	if( n <= max || n < 1e-8f )
		return;

	for( i = 0; i < 3; i++ )
		v[i] = v[i] / n * max;
}

/*
=====================
VRHold_Smooth

One axis of a critically damped follow, integrated implicitly.

The closed form of a critically damped spring over a step, rather than the
spring integrated explicitly - which matters because the stiffness this needs
is exactly the stiffness an explicit integrator cannot survive. A pistol has
to sit within a couple of millimetres of the hand, and a spring that tight
explodes at any sub-step a ninety-hertz frame can afford. This one is stable
at any step by construction.

The rational approximation to exp(-x) is the usual one; it is accurate to
better than a thousandth over the range a frame can produce, and it costs no
call into the maths library on a path that runs three times a frame.
=====================
*/
static void VRHold_Smooth( float *pos, float *vel, float target, float tau, float dt,
	float max_accel )
{
	float omega, x, ex, change, temp, newvel, accel;

	if( tau < 1e-5f )
		tau = 1e-5f;

	omega = 2.0f / tau;
	x = omega * dt;
	ex = 1.0f / ( 1.0f + x + 0.48f * x * x + 0.235f * x * x * x );

	change = *pos - target;
	temp = ( *vel + omega * change ) * dt;

	newvel = ( *vel - omega * temp ) * ex;

	// THE GRIP CAN ONLY PULL SO HARD. Without this a heavy weapon would
	// track a violently moving hand as obediently as a light one, and the
	// recoil of a one-handed shotgun would never get away from anybody.
	accel = ( newvel - *vel ) / ( dt > 1e-6f ? dt : 1e-6f );

	if( accel > max_accel )       newvel = *vel + max_accel * dt;
	else if( accel < -max_accel ) newvel = *vel - max_accel * dt;

	*vel = newvel;
	*pos = target + ( change + temp ) * ex;
}

/*
=====================
VRHold_Step
=====================
*/
void VRHold_Step( vrhold_t *h, const vrholdcfg_t *cfg,
	const float tpos[3], const float tquat[4], int hands, int shouldered, float dt )
{
	float follow, turn;
	int i;

	if( !h || !cfg || !tpos || !tquat )
		return;

	if( !h->have )
	{
		VRHold_Reset( h, tpos, tquat );
		return;
	}

	if( dt <= 0.0f )
		return;

	// A hitch is not a frame. Integrating one makes the gun lunge across the
	// room to catch up, which is both wrong and unpleasant.
	if( dt > VRHOLD_MAX_DT )
		dt = VRHOLD_MAX_DT;

	follow = cfg->follow_time;
	turn = cfg->turn_time;

	// A SECOND HAND IS A SECOND CONSTRAINT, and a shouldered stock a third.
	// The weapon is steadier because it is physically held more, not because
	// a steadiness number was turned up somewhere.
	if( hands > 1 )
	{
		follow *= 0.45f;
		turn *= 0.35f;
	}

	if( shouldered )
		turn *= 0.55f;

	for( i = 0; i < 3; i++ )
		VRHold_Smooth( &h->pos[i], &h->vel[i], tpos[i], follow, dt, cfg->max_accel );

	// ---- and the turning -------------------------------------------
	//
	// The same follow, applied to the shortest rotation to the hand's pose.
	// Done as one rotation rather than three Euler angles, because a gun
	// pointed straight up is exactly where a player will put one and that is
	// where Euler angles come apart.
	{
		float twist[3], n, want, speed;

		VRHold_Twist( h->quat, tquat, twist );
		n = VRHold_Len( twist );

		if( n > 1e-6f )
		{
			float omega = 2.0f / ( turn < 1e-5f ? 1e-5f : turn );
			float x = omega * dt;
			float ex = 1.0f / ( 1.0f + x + 0.48f * x * x + 0.235f * x * x * x );

			// How much of the remaining angle to take this frame, and how
			// fast the spin has to be to take it.
			want = n * ( 1.0f - ex );
			speed = want / dt;

			for( i = 0; i < 3; i++ )
				h->avel[i] += ( twist[i] / n * speed - h->avel[i] );
		}

		VRHold_ClampLen( h->avel, cfg->max_angaccel );

		// THE HAND'S SPIN PLUS WHATEVER THE GUN IS STILL DOING. The kick
		// rides on top rather than being overwritten by the assignment
		// above, which is what turns a shot into a rise and a settle
		// instead of one frame of enormous angular velocity.
		{
			float total[3];

			for( i = 0; i < 3; i++ )
				total[i] = h->avel[i] + h->kick[i];

			VRHold_Spin( h->quat, total, dt );
		}

		// The kick decays, so an impulse from a shot rings down rather than
		// turning the gun forever. That is what this comment always meant;
		// until the kick had somewhere of its own to live it was decaying a
		// number the hand had already overwritten.
		{
			float keep = 1.0f - dt / ( turn * 4.0f < 1e-4f ? 1e-4f : turn * 4.0f );

			if( keep < 0.0f ) keep = 0.0f;

			for( i = 0; i < 3; i++ )
				h->kick[i] *= keep;
		}
	}

	// ---- comfort ---------------------------------------------------
	//
	// A LIMIT, NOT A BEHAVIOUR. However heavy the weapon, it may not trail
	// the hand further than this. A gun that lags far behind the controller
	// is a gun that cannot be aimed and a reliable way to make somebody ill,
	// and no amount of physical honesty is worth that.
	{
		float lag[3];

		for( i = 0; i < 3; i++ )
			lag[i] = h->pos[i] - tpos[i];

		if( VRHold_Len( lag ) > cfg->max_lag )
		{
			VRHold_ClampLen( lag, cfg->max_lag );

			for( i = 0; i < 3; i++ )
				h->pos[i] = tpos[i] + lag[i];
		}
	}

	{
		float twist[3];
		float n;

		VRHold_Twist( h->quat, tquat, twist );
		n = VRHold_Len( twist );

		if( n > cfg->max_tilt )
		{
			float w[3];

			for( i = 0; i < 3; i++ )
				w[i] = twist[i] / n * ( n - cfg->max_tilt );

			VRHold_Spin( h->quat, w, 1.0f );
		}
	}
}

/*
=====================
VRHold_Recoil

An impulse at the muzzle: back along the barrel, and a torque that lifts it.

The climb and the recovery are not scripted. The impulse throws the gun, the
same springs that hold it bring it back, and how far it goes depends on the
weapon's mass and on how many hands are on it. One-handed with something
heavy, the impulse can exceed what a hand can pull against and the gun really
does get away from the player for a moment.
=====================
*/
void VRHold_Recoil( vrhold_t *h, const vrholdcfg_t *cfg, const float fwd[3] )
{
	float up[3] = { 0.0f, 0.0f, 1.0f };
	float axis[3];
	int i;

	if( !h || !cfg || !fwd )
		return;

	// The shot gives the weapon velocity backwards along its own barrel.
	// What happens next is not scripted: the same follow that holds the gun
	// pulls it back to the hand, and how far it gets depends on the mass and
	// on how many hands are on it.
	for( i = 0; i < 3; i++ )
		h->vel[i] -= fwd[i] * cfg->recoil_impulse / cfg->mass;

	// The muzzle rises, so the gun turns about the axis across the barrel
	// and across the vertical - which is the same axis whichever way the
	// player is facing, and does not need to know which way is up in the
	// weapon's own frame.
	VRHold_Cross( fwd, up, axis );

	if( VRHold_Len( axis ) < 1e-4f )
		return;             // pointed straight up: nothing to climb

	VRHold_ClampLen( axis, 1.0f );

	// INTO kick, NOT avel. The hand's spring assigns avel outright every
	// step, so a torque written there is gone by the next frame - see the
	// note on vrhold_t::kick. That is the whole reason muzzle climb was a
	// one-frame spike rather than a rise and a settle.
	for( i = 0; i < 3; i++ )
		h->kick[i] += axis[i] * cfg->recoil_torque / cfg->inertia;
}

void VRHold_Blocked( vrhold_t *h, const float at[3], const float normal[3] )
{
	float into;
	int i;

	if( !h || !at || !normal )
		return;

	for( i = 0; i < 3; i++ )
		h->pos[i] = at[i];

	// Only the component going INTO the surface is removed. A gun dragged
	// along a wall should still slide along it.
	into = VRHold_Dot( h->vel, normal );

	if( into < 0.0f )
	{
		for( i = 0; i < 3; i++ )
			h->vel[i] -= normal[i] * into;
	}
}

float VRHold_Separation( const vrhold_t *h, const float tpos[3] )
{
	float d[3];
	int i;

	if( !h || !tpos || !h->have )
		return 0.0f;

	for( i = 0; i < 3; i++ )
		d[i] = h->pos[i] - tpos[i];

	return VRHold_Len( d );
}
