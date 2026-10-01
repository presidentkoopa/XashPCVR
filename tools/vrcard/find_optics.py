"""Find the optics on a weapon model: a scope's tube, its two glass faces, its
axis.

Part H needs the same three numbers three times over. The scope (H-03) renders
a narrow view from the objective and puts it on the eyepiece. A reticle at
infinity (H-02) is drawn where the line from one eye to a far point along the
axis crosses the lens. Iron sights (H-01) are a front and a rear point the
player lines up. All of it is: WHERE the optical axis is, WHICH WAY it points,
and WHERE the glass sits on it.

NO MODEL MARKS ANY OF THAT, and neither of the two scoped weapons we carry a
card for has a bone for its scope. The HD crossbow's scope is 135 vertices of
the body mesh and the M40A1's is 148 of the stock's; there is no `scope` bone,
no `lens`, no attachment. A lens therefore cannot be declared by bone name the
way a joint is - it has to be a measured point in the body bone's frame, which
is what `control` already does and why the card syntax this prints looks like
a control's.

What a model DOES have is the geometry, and a scope is the one thing on a gun
that is a long thin cylinder somewhere other than the bore. So this fits
cylinders: it sweeps candidate axes across the cross-section, keeps the ones
whose vertices sit at a consistent radius over a run of the length, and reports
each with its two end faces.

    python find_optics.py <model.mdl> [--body BONE] [--forward x|-x|y|-y|z|-z]
                                      [--max-radius R] [--top N]

VERIFIED AGAINST THE TURRETS, which is the only reason to trust it. The M40A1
carries two bones the card already identified as "the scope's windage and
elevation turrets" - `m40a1_sideknob` and `m40a1_topknob`. A turret sits
perpendicular to the tube, so the one that is not displaced sideways states the
axis's lateral position and the one that is not displaced vertically states its
height: between them, y -0.032 z 3.163. This program never looks at a bone, and
it fits that model's objective section to y 0.000 z 3.170 with a radius spread
of 0.6%. Two independent measurements of one axis agreeing to three hundredths
of a unit is what makes the crossbow's fit - which has no turrets to check it
against - believable.

NOTHING HERE DECIDES WHICH TUBE IS GLASS. A barrel is a cylinder too, and on a
model with a flash hider or a gas tube so are those. The output is sorted and
annotated with whatever bones sit near each axis, so the turret check is there
when a model offers one; a human still has to look at the gun and say which
cylinder is a scope.
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vrcardgen as V
from find_controls import body_verts


# A STUDIO CYLINDER IS TWO RINGS AND A QUAD STRIP. There are no vertices along
# a smooth tube's length - the M40A1's objective section is fourteen vertices at
# x -8.4 and fourteen more at -5.4 with two and a half units of nothing between
# them. So a tube is not a run of populated slices (the first version of this
# looked for three consecutive and found the receiver instead); it is TWO OR
# MORE RINGS of the same radius about the same axis, however far apart.
MIN_RING = 5            # vertices before a slice counts as a ring
RING_SPREAD = 0.10      # radius sd as a fraction of the radius, within a ring
RING_AGREE = 0.25       # how far two rings' radii may differ and still be one tube
MIN_RINGS = 2
MIN_LENGTH = 0.9        # model units between the outermost rings
SLICE = 0.4             # model units
AXIS_STEP = 0.25        # model units, the coarse search grid
REFINE_STEPS = 40       # per axis, over +/- one coarse step


def long_axis(verts):
    """(long, cross, cross) by extent. A weapon is longer than it is wide."""
    spans = sorted(((max(p[a] for p in verts) - min(p[a] for p in verts), a)
                    for a in range(3)), reverse=True)
    L = spans[0][1]
    cross = sorted(a for a in range(3) if a != L)
    return L, cross[0], cross[1]


def forward_sign(verts, axis):
    """Which end is the muzzle.

    By geometry, not by the attachment: vrcardgen's `_frame` reads the first
    attachment's bone translation and falls back to -y, and on the crossbow -
    which has no attachments at all - that fallback is the only reason the
    numbers in its card are right. Here the rule is that a muzzle is THIN and a
    receiver is FAT, which is true of every v_ model measured so far and is
    checkable from the printed profile when it is not.
    """
    lo = min(p[axis] for p in verts)
    hi = max(p[axis] for p in verts)
    span = hi - lo
    if span <= 0.01:
        return -1
    cross = [a for a in range(3) if a != axis]

    def fatness(sel):
        if len(sel) < 3:
            return 0.0
        c = [sum(p[a] for p in sel) / len(sel) for a in cross]
        return max(math.hypot(p[cross[0]] - c[0], p[cross[1]] - c[1]) for p in sel)

    near_lo = fatness([p for p in verts if p[axis] <= lo + span * 0.1])
    near_hi = fatness([p for p in verts if p[axis] >= hi - span * 0.1])
    return -1 if near_lo <= near_hi else 1


def _slices(verts, axis, step):
    lo = min(p[axis] for p in verts)
    hi = max(p[axis] for p in verts)
    n = max(1, int(math.ceil((hi - lo) / step)))
    out = []
    for i in range(n):
        a0 = lo + i * step
        out.append((a0, [p for p in verts if a0 <= p[axis] < a0 + step]))
    return out


def rings_about(verts, L, A, B, ca, cb, max_radius):
    """Slices that hold a clean ring about (ca, cb): [(station, radius, verts)].

    ONE SLICE CAN HOLD TWO CONCENTRIC THINGS. Taking every vertex within some
    radius and asking whether it is a ring does not work: at the M40A1's
    objective the same slice holds the scope at 1.07 and the barrel 1.78 below
    the scope's axis, and averaging the two gives a spread of 13% and throws the
    scope away. So each slice is scanned for BANDS - the tightest groups of
    vertices at a common distance - and every band is reported. The barrel and
    the scope then come out as two rings, and grouping by radius sorts them into
    two tubes.
    """
    out = []
    for a0, g in _slices(verts, L, SLICE):
        ds = sorted((math.hypot(p[A] - ca, p[B] - cb), p) for p in g
                    if math.hypot(p[A] - ca, p[B] - cb) <= max_radius)
        if len(ds) < MIN_RING:
            continue
        n = len(ds)
        i = 0
        while i <= n - MIN_RING:
            if ds[i][0] < 0.15:
                i += 1
                continue
            j = i
            while j + 1 < n and ds[j + 1][0] - ds[i][0] <= RING_SPREAD * 2.2 * ds[i][0]:
                j += 1
            if j - i + 1 >= MIN_RING:
                band = ds[i:j + 1]
                rs = [d for d, _ in band]
                mean = sum(rs) / len(rs)
                sd = math.sqrt(sum((r - mean) ** 2 for r in rs) / len(rs))
                if sd / mean <= RING_SPREAD:
                    out.append((a0 + SLICE / 2.0, mean, [p for _, p in band]))
                i = j + 1
            else:
                i += 1
    return out


def tubes_about(verts, L, A, B, ca, cb, max_radius):
    """Group rings of the same radius into tubes.

    Returns [(l0, l1, radius, spread, nverts, nrings)]. Rings are grouped by
    radius rather than by adjacency, which is the whole point: the two ends of
    a smooth cylinder are the only vertices it has.
    """
    rings = rings_about(verts, L, A, B, ca, cb, max_radius)
    if len(rings) < MIN_RINGS:
        return []

    groups = []
    for station, radius, sel in sorted(rings, key=lambda r: r[1]):
        if groups and abs(radius - groups[-1][0][1]) <= RING_AGREE * radius:
            groups[-1].append((station, radius, sel))
        else:
            groups.append([(station, radius, sel)])

    out = []
    for g in groups:
        if len(g) < MIN_RINGS:
            continue
        allv = [p for _, _, sel in g for p in sel]
        rs = [math.hypot(p[A] - ca, p[B] - cb) for p in allv]
        mean = sum(rs) / len(rs)
        sd = math.sqrt(sum((r - mean) ** 2 for r in rs) / len(rs))
        l0 = min(p[L] for p in allv)
        l1 = max(p[L] for p in allv)
        if l1 - l0 < MIN_LENGTH:
            continue
        out.append((l0, l1, mean, sd / mean if mean else 1.0, len(allv), len(g)))
    return out


def _circle_fit(pts):
    """Algebraic circle fit (Kasa): solve u^2+v^2 = Au + Bv + C by least
    squares, which puts the centre at (A/2, B/2). Closed form, so the axis is
    not limited to whatever a search grid happened to sample - the first
    version of this searched 0.25-unit steps and stuck the M40A1's objective at
    y -0.09 z 3.34 when its vertices state y 0.00 z 3.17."""
    n = len(pts)
    su = sv = suu = svv = suv = sw = swu = swv = 0.0
    for u, v in pts:
        w = u * u + v * v
        su += u
        sv += v
        suu += u * u
        svv += v * v
        suv += u * v
        sw += w
        swu += w * u
        swv += w * v
    m = [[suu, suv, su], [suv, svv, sv], [su, sv, float(n)]]
    r = [swu, swv, sw]
    # Gaussian elimination on a 3x3; a degenerate cluster (all collinear) has
    # no circle and is reported as a miss rather than as a wild centre.
    for c in range(3):
        piv = max(range(c, 3), key=lambda k: abs(m[k][c]))
        if abs(m[piv][c]) < 1e-9:
            return None
        m[c], m[piv] = m[piv], m[c]
        r[c], r[piv] = r[piv], r[c]
        for k in range(c + 1, 3):
            f = m[k][c] / m[c][c]
            for j in range(c, 3):
                m[k][j] -= f * m[c][j]
            r[k] -= f * r[c]
    x = [0.0, 0.0, 0.0]
    for c in (2, 1, 0):
        s = r[c] - sum(m[c][j] * x[j] for j in range(c + 1, 3))
        x[c] = s / m[c][c]
    ca, cb = x[0] / 2.0, x[1] / 2.0
    disc = x[2] + ca * ca + cb * cb
    if disc <= 0:
        return None
    return ca, cb, math.sqrt(disc)


def refine(verts, L, A, B, ca, cb, l0, l1, radius):
    """Fit the axis to this tube's own band, re-selecting the band as it moves.

    The band, not everything within some radius: feeding the barrel back in
    here was what held the M40A1's objective off its true axis. Two tenths of a
    unit is the difference between a reticle that sits on the aim point and one
    that does not.
    """
    for _ in range(4):
        sel = [p for p in verts if l0 - 0.2 <= p[L] <= l1 + 0.2
               and abs(math.hypot(p[A] - ca, p[B] - cb) - radius) <= radius * 0.5]
        if len(sel) < MIN_RING:
            return ca, cb, radius, 1.0
        fit = _circle_fit([(p[A], p[B]) for p in sel])
        if fit is None:
            return ca, cb, radius, 1.0
        na, nb, nr = fit
        moved = math.hypot(na - ca, nb - cb) + abs(nr - radius)
        ca, cb, radius = na, nb, nr
        if moved < 1e-4:
            break

    sel = [p for p in verts if l0 - 0.2 <= p[L] <= l1 + 0.2
           and abs(math.hypot(p[A] - ca, p[B] - cb) - radius) <= radius * 0.5]
    if len(sel) < MIN_RING:
        return ca, cb, radius, 1.0
    ds = [math.hypot(p[A] - ca, p[B] - cb) for p in sel]
    mean = sum(ds) / len(ds)
    sd = math.sqrt(sum((d - mean) ** 2 for d in ds) / len(ds))
    return ca, cb, mean, (sd / mean if mean else 1.0)


def face(verts, L, A, B, ca, cb, at, radius):
    """The ring at one end of a tube: how many vertices and what radius."""
    sel = [p for p in verts if abs(p[L] - at) < 0.35
           and abs(math.hypot(p[A] - ca, p[B] - cb) - radius) < radius * 0.45]
    if not sel:
        return 0, radius, radius
    rs = [math.hypot(p[A] - ca, p[B] - cb) for p in sel]
    return len(sel), min(rs), max(rs)


def nearby_bones(model, body_index, L, A, B, ca, cb, l0, l1):
    """Bones whose bind position sits beside this axis, within its run.

    THE TURRET CHECK. A scope's adjustment turrets are bones that never animate
    and own a dozen vertices each, and they are the only independent statement
    a .mdl makes about where a scope's axis is. Anything this prints beside a
    candidate tube is worth looking at: on the M40A1 it prints both turrets and
    they confirm the fit to three hundredths of a unit.
    """
    out = []
    for b in model.bones:
        if b.index == body_index or b.parent != body_index:
            continue
        p = b.value[:3]
        if not (l0 - 1.0 <= p[L] <= l1 + 1.0):
            continue
        d = math.hypot(p[A] - ca, p[B] - cb)
        if d <= 3.0:
            out.append((d, b.name, p))
    out.sort()
    return out


AXES = {'x': (0, 1), '-x': (0, -1), 'y': (1, 1), '-y': (1, -1),
        'z': (2, 1), '-z': (2, -1)}


def main(argv):
    if not argv:
        print(__doc__.strip().splitlines()[0])
        print('usage: find_optics.py <model.mdl> [--body BONE] '
              '[--forward x|-x|y|-y|z|-z] [--max-radius R] [--top N]')
        return 2

    path = argv[0]
    top = int(argv[argv.index('--top') + 1]) if '--top' in argv else 6
    max_radius = (float(argv[argv.index('--max-radius') + 1])
                  if '--max-radius' in argv else 3.0)

    m = V.Model(path)
    counts, total = m.vertex_owners()
    if not total:
        print('model owns no vertices')
        return 1

    if '--body' in argv:
        want = argv[argv.index('--body') + 1]
        hit = [b.index for b in m.bones if b.name == want]
        if not hit:
            print('no bone named "%s" - the card`s body bone must exist' % want)
            return 1
        bi = hit[0]
    else:
        bi = V.find_body(m, counts, total)

    verts = body_verts(m, bi)
    print('%s' % os.path.basename(path))
    print('  body bone %d "%s", %d vertices' % (bi, m.bones[bi].name, len(verts)))
    if len(verts) < 40:
        print('  too little mesh on this bone to fit anything')
        return 1

    L, A, B = long_axis(verts)
    if '--forward' in argv:
        fa, fs = AXES[argv[argv.index('--forward') + 1]]
        if fa != L:
            print('  note: --forward names %s but the long axis is %s; using %s'
                  % ('xyz'[fa], 'xyz'[L], 'xyz'[fa]))
            L = fa
            A, B = [a for a in range(3) if a != L]
        sign = fs
    else:
        sign = forward_sign(verts, L)

    lo = min(p[L] for p in verts)
    hi = max(p[L] for p in verts)
    print('  long axis %s, forward %s%s, cross axes %s %s'
          % ('xyz'[L], '-' if sign < 0 else '+', 'xyz'[L], 'xyz'[A], 'xyz'[B]))
    print('  extent along %s: %.2f..%.2f   muzzle end %s = %.2f'
          % ('xyz'[L], lo, hi, 'xyz'[L], lo if sign < 0 else hi))
    print()

    a0 = min(p[A] for p in verts)
    a1 = max(p[A] for p in verts)
    b0 = min(p[B] for p in verts)
    b1 = max(p[B] for p in verts)

    found = []
    a = a0
    while a <= a1 + 1e-6:
        b = b0
        while b <= b1 + 1e-6:
            for (l0, l1, rad, spread, nv, nr) in tubes_about(
                    verts, L, A, B, a, b, max_radius):
                found.append((l0, l1, rad, spread, nv, nr, a, b))
            b += AXIS_STEP
        a += AXIS_STEP

    # Refine, then drop duplicates: the coarse grid finds the same tube from
    # several neighbouring centres, and the best of those is the one whose
    # refined axis leaves the least radius spread.
    tubes = []
    for (l0, l1, rad, spread, nv, nr, a, b) in found:
        ra, rb, rrad, rspread = refine(verts, L, A, B, a, b, l0, l1, rad)
        dup = False
        for t in tubes:
            if (abs(t['a'] - ra) < 0.6 and abs(t['b'] - rb) < 0.6
                    and abs(t['l0'] - l0) < 1.5 and abs(t['l1'] - l1) < 1.5):
                dup = True
                if rspread < t['spread']:
                    t.update(a=ra, b=rb, radius=rrad, spread=rspread,
                             l0=l0, l1=l1, nverts=nv, nrings=nr)
                break
        if dup:
            continue
        tubes.append(dict(a=ra, b=rb, radius=rrad, spread=rspread,
                          l0=l0, l1=l1, nverts=nv, nrings=nr))

    # ROUNDNESS FIRST. A tube's radius spread is the one measurement that
    # separates glass from a lump: the M40A1's objective fits to 0.6% and the
    # receiver blob the first version of this preferred fits to 13%. Length and
    # vertex count only break ties, because a barrel is long and round too -
    # which is why this prints a ranking and not a verdict.
    tubes.sort(key=lambda t: (max(t['spread'], 0.003),
                              -(t['l1'] - t['l0']) * t['nverts']))

    if not tubes:
        print('  no cylinder found. Either this weapon has no optic, or its '
              'tube is\n  shorter than %d slices or wider than --max-radius '
              '%.1f.' % (MIN_SLICES, max_radius))
        return 0

    print('  %d candidate cylinder(s), best first:' % len(tubes))
    for n, t in enumerate(tubes[:top]):
        front = t['l0'] if sign < 0 else t['l1']
        rear = t['l1'] if sign < 0 else t['l0']
        fn, fr0, fr1 = face(verts, L, A, B, t['a'], t['b'], front, t['radius'])
        rn, rr0, rr1 = face(verts, L, A, B, t['a'], t['b'], rear, t['radius'])
        print()
        print('  [%d] axis %s=%.3f %s=%.3f   radius %.3f (spread %.1f%%)   '
              '%d verts in %d rings'
              % (n, 'xyz'[A], t['a'], 'xyz'[B], t['b'],
                 t['radius'], 100 * t['spread'], t['nverts'], t['nrings']))
        print('      runs %s %.3f .. %.3f   length %.3f'
              % ('xyz'[L], t['l0'], t['l1'], t['l1'] - t['l0']))
        print('      front face %s=%7.3f  %2d verts  radius %.3f..%.3f'
              % ('xyz'[L], front, fn, fr0, fr1))
        print('      rear  face %s=%7.3f  %2d verts  radius %.3f..%.3f'
              % ('xyz'[L], rear, rn, rr0, rr1))
        for d, name, p in nearby_bones(m, bi, L, A, B, t['a'], t['b'],
                                       t['l0'], t['l1']):
            print('      beside it: bone "%s" at %.2f %.2f %.2f, %.2f off the axis'
                  % (name, p[0], p[1], p[2], d))

        fp = [0.0, 0.0, 0.0]
        rp = [0.0, 0.0, 0.0]
        fp[L], fp[A], fp[B] = front, t['a'], t['b']
        rp[L], rp[A], rp[B] = rear, t['a'], t['b']
        print('      as a card block:')
        print('          optic scope')
        print('                front at %.2f %.2f %.2f  radius %.2f'
              % (fp[0], fp[1], fp[2], max(fr1, t['radius'])))
        print('                rear  at %.2f %.2f %.2f  radius %.2f'
              % (rp[0], rp[1], rp[2], max(rr1, t['radius'])))

    print()
    print('  A BARREL IS A CYLINDER TOO. Check each candidate against the gun:')
    print('  a scope sits ABOVE the bore and ends short of the muzzle, and any')
    print('  bone printed beside a candidate is the model`s own second opinion.')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
