"""How complete are Valve's weapon meshes, actually?

The owner's point: Valve did not draw complete models. A grip solver that closes
fingers until they touch geometry will close them into empty space wherever the
geometry was never drawn, and a finger that finds nothing ends up inside the gun.

So measure it per model, in the only terms that matter to a solver:
  - boundary edges: an edge used by exactly ONE triangle. A closed solid has
    zero. These are the holes - where a finger passes straight through.
  - non-manifold edges: an edge used by THREE OR MORE triangles. Interior walls,
    coincident faces, geometry stuffed inside other geometry.
  - degenerate triangles: zero area or a repeated index. These break any
    point-in-triangle or distance-to-triangle test that divides by area.
  - triangle count: the load-time cost of a triangle-based solve.
"""
import glob
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vrcardgen

COMMON = os.environ.get('HL_ROOT', r'D:\SteamLibrary\steamapps\common\Half-Life')
BODYPART_SIZE = vrcardgen.BODYPART_SIZE
MODEL_SIZE = vrcardgen.MODEL_SIZE
_i = vrcardgen._i
_v3 = vrcardgen._v3


def raw_tris(model):
    tris = []
    pos = {}
    for bp in range(model.numbodyparts):
        o = model.bodypartindex + bp * BODYPART_SIZE
        nummodels = _i(model.d, o + 64)
        modelindex = _i(model.d, o + 72)
        for m in range(nummodels):
            mo = modelindex + m * MODEL_SIZE
            nummesh = _i(model.d, mo + 72)
            meshindex = _i(model.d, mo + 76)
            vertindex = _i(model.d, mo + 88)
            numverts = _i(model.d, mo + 80)
            base = (bp, m)
            for v in range(numverts):
                pos[(base, v)] = _v3(model.d, vertindex + v * 12)
            for k in range(nummesh):
                me = meshindex + k * 20
                p = _i(model.d, me + 4)
                while True:
                    n = struct.unpack_from('<h', model.d, p)[0]
                    p += 2
                    if n == 0:
                        break
                    fan = n < 0
                    n = abs(n)
                    run = []
                    for _ in range(n):
                        vi = struct.unpack_from('<H', model.d, p)[0]
                        p += 8
                        run.append((base, vi))
                    for t in range(n - 2):
                        if fan:
                            tri = (run[0], run[t + 1], run[t + 2])
                        elif t % 2:
                            tri = (run[t + 1], run[t], run[t + 2])
                        else:
                            tri = (run[t], run[t + 1], run[t + 2])
                        tris.append(tri)
    return tris, pos


def area2(a, b, c):
    u = [b[i] - a[i] for i in range(3)]
    v = [c[i] - a[i] for i in range(3)]
    cx = u[1] * v[2] - u[2] * v[1]
    cy = u[2] * v[0] - u[0] * v[2]
    cz = u[0] * v[1] - u[1] * v[0]
    return cx * cx + cy * cy + cz * cz


def analyse(path):
    m = vrcardgen.Model(path)
    tris, pos = raw_tris(m)
    edges = {}
    degen = 0
    for tri in tris:
        if len(set(tri)) < 3:
            degen += 1
            continue
        p = [pos[v] for v in tri]
        if area2(*p) < 1e-12:
            degen += 1
        # Weld by POSITION, not index: GoldSrc splits verts per mesh, so two
        # meshes meeting at a seam carry different indices for the same point.
        key = []
        for v in tri:
            q = pos[v]
            key.append((round(q[0], 3), round(q[1], 3), round(q[2], 3)))
        for i in range(3):
            a, b = key[i], key[(i + 1) % 3]
            e = (a, b) if a <= b else (b, a)
            edges[e] = edges.get(e, 0) + 1
    boundary = sum(1 for c in edges.values() if c == 1)
    nonman = sum(1 for c in edges.values() if c >= 3)
    return {
        'bones': m.numbones,
        'tris': len(tris),
        'edges': len(edges),
        'boundary': boundary,
        'nonmanifold': nonman,
        'degenerate': degen,
        'open_pct': (100.0 * boundary / len(edges)) if edges else 0.0,
    }


DIRS = sys.argv[1:] or ['valve', 'gearbox', 'bshift']
print('%-18s %-9s %6s %7s %8s %7s %6s  %s' %
      ('weapon', 'gamedir', 'bones', 'tris', 'open-%', 'holes', 'n-man', 'verdict'))
print('-' * 94)
worst = []
for d in DIRS:
    mdir = os.path.join(COMMON, d, 'models')
    for p in sorted(glob.glob(os.path.join(mdir, 'v_*.mdl'))):
        name = os.path.splitext(os.path.basename(p))[0]
        try:
            r = analyse(p)
        except Exception as exc:
            print('%-18s %-9s  FAILED: %s' % (name, d, exc))
            continue
        if r['open_pct'] < 1.0:
            verdict = 'closed solid'
        elif r['open_pct'] < 10.0:
            verdict = 'mostly closed'
        elif r['open_pct'] < 30.0:
            verdict = 'OPEN - real holes'
        else:
            verdict = 'SHELL - not solid at all'
        print('%-18s %-9s %6d %7d %7.1f%% %7d %6d  %s' %
              (name, d, r['bones'], r['tris'], r['open_pct'],
               r['boundary'], r['nonmanifold'], verdict))
        worst.append((r['open_pct'], name, d, r['tris'], r['degenerate']))

print()
worst.sort(reverse=True)
print('MOST INCOMPLETE - a finger closing on these finds nothing:')
for pct, name, d, t, dg in worst[:14]:
    print('   %-18s %-9s %5.1f%% of edges are holes, %5d tris, %d degenerate' % (name, d, pct, t, dg))
print()
tc = sorted(w[3] for w in worst)
print('triangle counts across %d models: min %d, median %d, max %d' % (len(tc), tc[0], tc[len(tc) // 2], tc[-1]))
print('total degenerate triangles across all models: %d' % sum(w[4] for w in worst))
print()
tally = {}
for pct, name, d, t, dg in worst:
    if pct < 1.0:    k = 'closed solid'
    elif pct < 10.0: k = 'mostly closed'
    elif pct < 30.0: k = 'OPEN - real holes'
    else:            k = 'SHELL - not solid at all'
    tally[k] = tally.get(k, 0) + 1
print('TALLY over %d models:' % len(worst))
for k in ('closed solid', 'mostly closed', 'OPEN - real holes', 'SHELL - not solid at all'):
    if k in tally:
        print('   %-26s %d' % (k, tally[k]))
