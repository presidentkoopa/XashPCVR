"""
Verify the OpenXR quaternion -> GoldSrc euler conversion numerically, so the
correct signs can be established WITHOUT wearing a headset and guessing.

Method: build a quaternion for a known real-world head rotation in OpenXR space,
run the exact conversion used in vr_openxr.c, and check the resulting HL angles
against what AngleVectors() in public/xash3d_mathlib.h would need them to be.

OpenXR : +X right, +Y up, -Z forward, right-handed, meters
GoldSrc: +X forward, +Y left, +Z up,  right-handed, units
"""
import math

def quat_axis_angle(axis, deg):
    """Quaternion (x,y,z,w) for a right-handed rotation of `deg` about `axis`."""
    ax, ay, az = axis
    n = math.sqrt(ax*ax + ay*ay + az*az)
    ax, ay, az = ax/n, ay/n, az/n
    h = math.radians(deg) / 2.0
    s = math.sin(h)
    return (ax*s, ay*s, az*s, math.cos(h))


def convert(q, roll_negated=True):
    """Exact port of VR_ConvertOrientation from vr_openxr.c."""
    x, y, z, w = q

    # forward = q * (0,0,-1)
    fwd_xr = (
        -(2.0 * (x*z + w*y)),
        -(2.0 * (y*z - w*x)),
        -(1.0 - 2.0 * (x*x + y*y)),
    )
    # up = q * (0,1,0)
    up_xr = (
        2.0 * (x*y - w*z),
        1.0 - 2.0 * (x*x + z*z),
        2.0 * (y*z + w*x),
    )

    # XR -> HL axes
    fwd = [-fwd_xr[2], -fwd_xr[0], fwd_xr[1]]
    up  = [-up_xr[2],  -up_xr[0],  up_xr[1]]

    def norm(v):
        n = math.sqrt(sum(c*c for c in v))
        return [c/n for c in v]

    fwd, up = norm(fwd), norm(up)

    # right = fwd x up   (matches AngleVectors: right == forward x up)
    right = [
        fwd[1]*up[2] - fwd[2]*up[1],
        fwd[2]*up[0] - fwd[0]*up[2],
        fwd[0]*up[1] - fwd[1]*up[0],
    ]
    right = norm(right)

    yaw   = math.degrees(math.atan2(fwd[1], fwd[0]))
    pitch = math.degrees(-math.asin(max(-1.0, min(1.0, fwd[2]))))
    roll  = math.degrees(math.atan2(right[2], up[2]))
    if roll_negated:
        roll = -roll
    return pitch, yaw, roll


def angle_vectors(pitch, yaw, roll):
    """Exact port of AngleVectors() from public/xash3d_mathlib.h:431."""
    sy, cy = math.sin(math.radians(yaw)),   math.cos(math.radians(yaw))
    sp, cp = math.sin(math.radians(pitch)), math.cos(math.radians(pitch))
    sr, cr = math.sin(math.radians(roll)),  math.cos(math.radians(roll))
    forward = (cp*cy, cp*sy, -sp)
    right   = (-1.0*sr*sp*cy + -1.0*cr*-sy, -1.0*sr*sp*sy + -1.0*cr*cy, -1.0*sr*cp)
    up      = (cr*sp*cy + -sr*-sy, cr*sp*sy + -sr*cy, cr*cp)
    return forward, right, up


# (description, quaternion, expected HL forward direction, expected sign hints)
CASES = [
    ("look straight ahead (identity)",
     quat_axis_angle((0, 1, 0), 0),
     (1, 0, 0)),

    ("turn head LEFT 90 (yaw about XR +Y)",
     quat_axis_angle((0, 1, 0), 90),
     (0, 1, 0)),          # HL +Y is left

    ("turn head RIGHT 90",
     quat_axis_angle((0, 1, 0), -90),
     (0, -1, 0)),

    ("look UP 45 (pitch about XR +X)",
     quat_axis_angle((1, 0, 0), 45),
     None),

    ("look DOWN 45",
     quat_axis_angle((1, 0, 0), -45),
     None),

    ("tilt head RIGHT 30 (roll about XR -Z = forward axis)",
     quat_axis_angle((0, 0, -1), 30),
     None),

    ("tilt head LEFT 30",
     quat_axis_angle((0, 0, 1), 30),
     None),
]

print("=" * 78)
print(" OpenXR quaternion -> GoldSrc euler verification")
print("=" * 78)

all_ok = True
for desc, q, expect_fwd in CASES:
    pitch, yaw, roll = convert(q)
    fwd, right, up = angle_vectors(pitch, yaw, roll)

    print(f"\n{desc}")
    print(f"   -> HL angles  pitch={pitch:8.2f}  yaw={yaw:8.2f}  roll={roll:8.2f}")
    print(f"      AngleVectors forward = ({fwd[0]:6.3f}, {fwd[1]:6.3f}, {fwd[2]:6.3f})")

    if expect_fwd:
        err = max(abs(a - b) for a, b in zip(fwd, expect_fwd))
        ok = err < 1e-4
        all_ok &= ok
        print(f"      expected forward     = ({expect_fwd[0]:6.3f}, {expect_fwd[1]:6.3f}, {expect_fwd[2]:6.3f})  "
              f"{'OK' if ok else 'MISMATCH'}")

# Round-trip: does convert() invert AngleVectors consistently?
print("\n" + "=" * 78)
print(" round-trip check: HL angles -> basis -> re-extract")
print("=" * 78)
for (p, y, r) in [(0,0,0), (0,45,0), (20,0,0), (-20,0,0), (0,0,30), (0,0,-30), (10,25,15)]:
    fwd, right, up = angle_vectors(p, y, r)
    yaw2   = math.degrees(math.atan2(fwd[1], fwd[0]))
    pitch2 = math.degrees(-math.asin(max(-1.0, min(1.0, fwd[2]))))
    roll2  = -math.degrees(math.atan2(right[2], up[2]))
    ok = (abs(p-pitch2) < 1e-3 and abs(y-yaw2) < 1e-3 and abs(r-roll2) < 1e-3)
    all_ok &= ok
    print(f"  in ({p:6.1f},{y:6.1f},{r:6.1f})  out ({pitch2:6.1f},{yaw2:6.1f},{roll2:6.1f})  "
          f"{'OK' if ok else 'MISMATCH'}")

print("\n" + ("ALL CHECKS PASSED" if all_ok else "*** FAILURES PRESENT ***"))
