/*
vr_hold.h - the held weapon as a thing with mass
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
#ifndef VR_HOLD_H
#define VR_HOLD_H

/*
====================
PART F. Today the weapon is pinned to the controller: wherever the hand is,
that is exactly where the gun is, instantly and rigidly. In the full build it
is a rigid body the hands hold through springs, and its settled pose is what
gets drawn and what aims.

Everything Part F asks for falls out of that one change rather than being
scripted on top of it. Muzzle climb is an impulse at the muzzle and a torque
that lifts it, brought back by the same springs that hold the gun. A second
hand steadies it because a second constraint is stiffer. A shouldered rifle
is steadier because the stock has a third constraint to the shoulder. A heavy
weapon lags because it is heavy. None of those is a special case.

FLOATS, AND DELIBERATELY. Everything in the shared simulator is fixed-point
because the server and the client's prediction must reach identical answers.
This is not that: the plan puts it on the client, "local, like view angles",
and its RESULT - the gun's pose and the muzzle - is what goes up in the
command. The server never runs this and has nothing to disagree with.

NO PHYSICS LIBRARY. One rigid body on spring constraints is an integrator,
not a simulation. The library the plan calls for is wanted by the WORLD -
dropped magazines, spent cases, weapons lying where they fell - and that is
Part I's problem, not this one.
====================
*/

typedef struct vrholdcfg_s
{
	// From the model: the body's volume times a density for its class. A
	// rocket launcher ends up heavier than a pistol without anybody tuning
	// anything, which is the point.
	float   mass;
	float   inertia;

	// HOW LONG THE WEAPON TAKES TO CATCH UP, in seconds, and it is the one
	// number that decides how a weapon feels. The trailing distance while
	// the hand is moving is this time multiplied by the hand's speed, which
	// makes it directly tunable: a pistol at two milliseconds trails a
	// couple of millimetres at ordinary hand speeds, and a launcher at
	// twenty trails a visible couple of centimetres.
	//
	// A TIME RATHER THAN A STIFFNESS, because a spring stiff enough to hold
	// a pistol within two millimetres is not stable when integrated
	// explicitly at any sub-step a frame can afford. The smoother below is
	// critically damped by construction and stable at any step, which buys
	// the stiffness the feel needs without the explosion.
	float   follow_time;
	float   turn_time;

	// The most a hand can accelerate the weapon. This is what makes a heavy
	// weapon get away from one hand: the impulse of a shot can want more
	// than a grip can give, and the gun goes where it likes for a moment.
	float   max_accel;
	float   max_angaccel;

	// A shot: an impulse along the barrel and a torque that lifts the
	// muzzle. Per weapon, from the card.
	float   recoil_impulse;
	float   recoil_torque;

	// COMFORT, and these are limits rather than behaviour. However heavy a
	// weapon is, it may not trail the hand by more than this - a gun that
	// lags far behind the controller is a gun the player cannot aim and a
	// reliable way to make somebody ill.
	float   max_lag;        // units
	float   max_tilt;       // radians
} vrholdcfg_t;

typedef struct vrhold_s
{
	float   pos[3];
	float   vel[3];
	float   quat[4];        // x y z w
	float   avel[3];        // radians per second, world axes

	// ANGULAR VELOCITY FROM IMPULSES, kept apart from avel because the hand
	// OVERWRITES avel every step.
	//
	// The spring that holds the gun computes the spin it wants and assigns
	// it - correct for following a hand, fatal for anything added on top. A
	// recoil torque written into avel survived exactly one frame: on the
	// frame after the shot the gun is still at the hand's orientation, so
	// the twist is zero, the assignment is skipped, and the impulse gets
	// one step. The muzzle climbed in a single 11ms spike and was then
	// fought straight back down - while the decay written to make "an
	// impulse from a shot ring down rather than turning the gun forever"
	// never saw the impulse at all.
	//
	// Kept here instead, added to the spin each step and decayed on its own
	// clock, the kick does what that comment always said it did.
	float   kick[3];

	int     have;           // false until the first pose arrives
} vrhold_t;

// Numbers for a weapon of this mass. A card overrides any of them.
void VRHold_DefaultCfg( vrholdcfg_t *cfg, float mass );

// Snap to a pose with no velocity - drawing a weapon, or teleporting.
void VRHold_Reset( vrhold_t *h, const float pos[3], const float quat[4] );

/*
====================
VRHold_Step

One frame. `tpos` and `tquat` are where the hands have the weapon; `hands` is
how many are on it, which is how a second hand steadies the gun; `dt` is the
frame in seconds.

NOT sub-stepped, and it does not need to be. This said it was - and
VRHOLD_SUBSTEPS was defined in the .c and referenced by nothing - but
VRHold_Smooth is the CLOSED FORM of a critically damped spring, stable at any
step size. There is no stiff integration here to explode. The dt clamp is for
a different hazard: a frame long enough to teleport the gun.
====================
*/
void VRHold_Step( vrhold_t *h, const vrholdcfg_t *cfg,
	const float tpos[3], const float tquat[4], int hands, int shouldered, float dt );

// A shot. `fwd` is the barrel's direction; the impulse goes back along it and
// the torque lifts the muzzle.
void VRHold_Recoil( vrhold_t *h, const vrholdcfg_t *cfg, const float fwd[3] );

// The weapon ran into something. Stop it there and kill the velocity into
// the surface, so the gun pivots in the hand rather than passing through.
void VRHold_Blocked( vrhold_t *h, const float at[3], const float normal[3] );

// How far the weapon is trailing the hand, for the "why has my aim stopped"
// outline the plan asks for.
float VRHold_Separation( const vrhold_t *h, const float tpos[3] );

#endif // VR_HOLD_H
