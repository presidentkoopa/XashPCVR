"""Find candidate control bosses on a weapon model.

A control is a point in space, not a bone - Part G wants a magazine release, a
slide stop, a safety and a selector under the thumb, and models mark almost
none of them with a bone or an attachment. What they DO have is geometry: a
boss, a paddle or a lever is a small cluster of vertices standing out from the
frame on the side a thumb can reach.

That is what this finds. It is how the pistol's two controls were measured -
a five-vertex cluster at x 1.04..1.12 under the slide and thirteen more behind
the trigger guard - and it reproduces those numbers exactly, which is the only
reason to trust it anywhere else.

    python find_controls.py <model.mdl> [--side x|-x|y|-y|z|-z] [--top N]

Works in the BODY bone's own space, because that is the frame a card records
its positions in. Prints clusters furthest out along the chosen axis, since a
control protrudes: the default scans both directions of every axis and reports
whichever stand out most.

Nothing here decides what a cluster IS. It narrows 400 vertices to a handful of
candidates with coordinates ready to paste into a card; a human still has to
look at the gun and say which lump is the magazine release.
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vrcardgen as V


def body_verts(model, bone_index):
    """Every vertex owned by one bone, in that bone's own space."""
    out = []
    for bp in range(model.numbodyparts):
        o = model.bodypartindex + bp * V.BODYPART_SIZE
        nummodels = V._i(model.d, o + 64)
        modelindex = V._i(model.d, o + 72)
        for m in range(nummodels):
            mo = modelindex + m * V.MODEL_SIZE
            numverts = V._i(model.d, mo + 80)
            vertinfo = V._i(model.d, mo + 84)
            vertindex = V._i(model.d, mo + 88)
            for v in range(numverts):
                if model.d[vertinfo + v] != bone_index:
                    continue
                out.append(V._v3(model.d, vertindex + v * 12))
    return out


def cluster(verts, tol=0.45):
    """Group vertices that are within tol of each other, single linkage."""
    groups = []
    for p in verts:
        hit = None
        for g in groups:
            for q in g:
                if (p[0] - q[0]) ** 2 + (p[1] - q[1]) ** 2 + (p[2] - q[2]) ** 2 <= tol * tol:
                    hit = g
                    break
            if hit:
                break
        if hit:
            hit.append(p)
        else:
            groups.append([p])
    # single linkage leaves groups that touch each other; merge once
    merged = True
    while merged:
        merged = False
        for i in range(len(groups)):
            for j in range(i + 1, len(groups)):
                near = False
                for p in groups[i]:
                    for q in groups[j]:
                        if (p[0] - q[0]) ** 2 + (p[1] - q[1]) ** 2 + (p[2] - q[2]) ** 2 <= tol * tol:
                            near = True
                            break
                    if near:
                        break
                if near:
                    groups[i] += groups[j]
                    del groups[j]
                    merged = True
                    break
            if merged:
                break
    return groups


AXES = [(0, 1, '+x'), (0, -1, '-x'), (1, 1, '+y'), (1, -1, '-y'), (2, 1, '+z'), (2, -1, '-z')]


def main(argv):
    if not argv:
        print(__doc__)
        return 2

    path = argv[0]
    top = 4
    if '--top' in argv:
        top = int(argv[argv.index('--top') + 1])

    m = V.Model(path)
    counts, total = m.vertex_owners()
    if not total:
        print('model owns no vertices')
        return 1

    bi = V.find_body(m, counts, total)
    bone = m.bones[bi].name
    verts = body_verts(m, bi)

    print('%s' % os.path.basename(path))
    print('  body bone %d "%s", %d vertices' % (bi, bone, len(verts)))
    if not verts:
        return 1

    lo = [min(p[a] for p in verts) for a in range(3)]
    hi = [max(p[a] for p in verts) for a in range(3)]
    print('  extent  x %.2f..%.2f   y %.2f..%.2f   z %.2f..%.2f'
          % (lo[0], hi[0], lo[1], hi[1], lo[2], hi[2]))
    print()

    wanted = [a for a in AXES if ('--side' not in argv) or argv[argv.index('--side') + 1] == a[2]]

    for axis, sign, label in wanted:
        # the outer tenth along this axis is where a protruding control lives
        span = hi[axis] - lo[axis]
        if span <= 0.01:
            continue
        cut = (hi[axis] - span * 0.12) if sign > 0 else (lo[axis] + span * 0.12)
        sel = [p for p in verts if (p[axis] >= cut if sign > 0 else p[axis] <= cut)]
        if len(sel) < 3:
            continue

        groups = [g for g in cluster(sel) if len(g) >= 3]
        if not groups:
            continue

        groups.sort(key=lambda g: -len(g))
        print('  %s  (%d vertices past %.2f, %d cluster(s))' % (label, len(sel), cut, len(groups)))
        for g in groups[:top]:
            c = [sum(p[a] for p in g) / len(g) for a in range(3)]
            rad = max(math.sqrt(sum((p[a] - c[a]) ** 2 for a in range(3))) for p in g)
            print('      %2d verts  centroid %7.2f %7.2f %7.2f   spread %.2f'
                  % (len(g), c[0], c[1], c[2], rad))
        print()

    print('  control lines take the centroid as-is:')
    print('      control <catch> at <x> <y> <z>  finger thumb  kind button')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
