#!/usr/bin/env python3
"""
vrcardgen - measure a GoldSrc view model and draft a VR weapon card.

Part E of the weapons plan. A card describes a weapon's moving parts: which
bone each one is, how far it travels, and where it is in the weapon's own
frame. All of that is measurable from the model, and measuring it is this
tool's whole job.

WHY A TOOL AND NOT A GUESS. The shipped bone map scores ten out of ten on
Valve HD and two out of ten on Valve SD, Opposing Force, Blue Shift and MMod,
because it was written by looking at one model set. There are 106 view models
across the mod targets. Hand-writing a card each is not the bottleneck -
hand-MEASURING one is, and that is the part a machine does better.

WHAT IT REFUSES TO DO. It never invents a catch or a control it cannot
measure, and it marks every part it is unsure of as a comment rather than as
a declaration. A card that drives the wrong bone is worse than no card,
because the fallback - the weapon's own reload animation - works.

Usage:
    vrcardgen.py <model.mdl> [...]        analyse and draft a card
    vrcardgen.py --census <dir>           table every model under a directory
    vrcardgen.py --write <outdir> <mdl>   write drafts to outdir/<name>.card
"""

import os
import struct
import sys

# ---------------------------------------------------------------------------
# studio header layout (version 10)
#
# Offsets are spelled out rather than unpacked as one big struct on purpose:
# an earlier pass of this read numseq from offset 180 and got numtextures,
# which for the HD pistol happens to be the same number. Named constants make
# that mistake visible instead of plausible.
# ---------------------------------------------------------------------------
H_BBMIN, H_BBMAX = 112, 124
H_NUMBONES, H_BONEINDEX = 140, 144
H_NUMSEQ, H_SEQINDEX = 164, 168
H_NUMSEQGROUPS, H_SEQGROUPINDEX = 172, 176
H_NUMBODYPARTS, H_BODYPARTINDEX = 204, 208
H_NUMATTACHMENTS, H_ATTACHMENTINDEX = 212, 216

BONE_SIZE = 112
SEQ_SIZE = 176
SEQGROUP_SIZE = 104
BODYPART_SIZE = 76
MODEL_SIZE = 112
ATTACH_SIZE = 88

FNV_BASIS = 2166136261
FNV_PRIME = 16777619
MASK32 = 0xFFFFFFFF


def _i(d, off):
    return struct.unpack_from("<i", d, off)[0]


def _f(d, off):
    return struct.unpack_from("<f", d, off)[0]


def _v3(d, off):
    return struct.unpack_from("<3f", d, off)


def _cstr(b):
    return b.split(b"\0")[0].decode("latin1")


class Bone(object):
    __slots__ = ("index", "name", "parent", "value", "scale")


class Model(object):
    """Everything vrcardgen needs out of a .mdl, and nothing else."""

    def __init__(self, path):
        self.path = path
        self.name = os.path.basename(path)
        with open(path, "rb") as fh:
            self.d = fh.read()

        if self.d[:4] != b"IDST":
            raise ValueError("%s is not a studio model" % path)

        self.version = _i(self.d, 4)
        self.numbones = _i(self.d, H_NUMBONES)
        self.boneindex = _i(self.d, H_BONEINDEX)
        self.numseq = _i(self.d, H_NUMSEQ)
        self.seqindex = _i(self.d, H_SEQINDEX)
        self.numseqgroups = _i(self.d, H_NUMSEQGROUPS)
        self.seqgroupindex = _i(self.d, H_SEQGROUPINDEX)
        self.numbodyparts = _i(self.d, H_NUMBODYPARTS)
        self.bodypartindex = _i(self.d, H_BODYPARTINDEX)
        self.numattachments = _i(self.d, H_NUMATTACHMENTS)
        self.attachmentindex = _i(self.d, H_ATTACHMENTINDEX)

        self.bbmin = _v3(self.d, H_BBMIN)
        self.bbmax = _v3(self.d, H_BBMAX)
        self._extent = None
        self.bones = self._read_bones()

    def extent(self):
        """The model's own size, as the diagonal of its vertex bounds.

        FROM THE VERTICES, NOT FROM THE HEADER. GoldSrc view models routinely
        ship with bbmin/bbmax left at zero - every model in the Half-Life set
        checked here does. Trusting the header gave every weapon an extent of
        one unit, which made every part that moved at all look like it was
        arriving from off-screen, and the shotgun came back with no pump.

        The header's box is optional metadata. The vertices are the model.
        """
        if self._extent is not None:
            return self._extent

        lo = [1e30] * 3
        hi = [-1e30] * 3

        for bp in range(self.numbodyparts):
            o = self.bodypartindex + bp * BODYPART_SIZE
            nummodels = _i(self.d, o + 64)
            modelindex = _i(self.d, o + 72)

            for m in range(nummodels):
                mo = modelindex + m * MODEL_SIZE
                numverts = _i(self.d, mo + 80)
                vertindex = _i(self.d, mo + 88)

                for v in range(numverts):
                    xyz = _v3(self.d, vertindex + v * 12)
                    for a in range(3):
                        if xyz[a] < lo[a]:
                            lo[a] = xyz[a]
                        if xyz[a] > hi[a]:
                            hi[a] = xyz[a]

        if lo[0] > hi[0]:
            self._extent = 1.0
        else:
            d = [hi[a] - lo[a] for a in range(3)]
            self._extent = max(1.0, (d[0] ** 2 + d[1] ** 2 + d[2] ** 2) ** 0.5)

        return self._extent

    # -- bones ------------------------------------------------------------

    def _read_bones(self):
        out = []
        for i in range(self.numbones):
            o = self.boneindex + i * BONE_SIZE
            b = Bone()
            b.index = i
            b.name = _cstr(self.d[o:o + 32])
            b.parent = _i(self.d, o + 32)
            b.value = struct.unpack_from("<6f", self.d, o + 64)
            b.scale = struct.unpack_from("<6f", self.d, o + 88)
            out.append(b)
        return out

    def fingerprint(self):
        """Bone count, sequence count, and the hash a card records.

        Must agree byte for byte with VR_HashBoneName in common/vrfingerprint.h
        - the engine computes it from the loaded model and the game DLL
        compares it. A card whose hash is off by a separator byte binds to
        nothing, silently.
        """
        h = FNV_BASIS
        for b in self.bones:
            for c in b.name.encode("latin1"):
                h = ((h ^ c) * FNV_PRIME) & MASK32
            h = ((h ^ 0xFF) * FNV_PRIME) & MASK32
        return self.numbones, self.numseq, h

    # -- geometry ---------------------------------------------------------

    def vertex_owners(self):
        """How many vertices each bone owns.

        The body of the weapon owns most of them; a slide or a magazine owns
        a few percent. That ratio is the first filter on what could possibly
        be a moving part.
        """
        counts = [0] * self.numbones
        total = 0

        for bp in range(self.numbodyparts):
            o = self.bodypartindex + bp * BODYPART_SIZE
            nummodels = _i(self.d, o + 64)
            modelindex = _i(self.d, o + 72)

            for m in range(nummodels):
                mo = modelindex + m * MODEL_SIZE
                numverts = _i(self.d, mo + 80)
                vertinfoindex = _i(self.d, mo + 84)

                for v in range(numverts):
                    bone = self.d[vertinfoindex + v]
                    if 0 <= bone < self.numbones:
                        counts[bone] += 1
                        total += 1

        return counts, total

    def bone_centroids(self):
        """Each bone's vertices' centre, in that bone's own space."""
        acc = [[0.0, 0.0, 0.0] for _ in range(self.numbones)]
        num = [0] * self.numbones

        for bp in range(self.numbodyparts):
            o = self.bodypartindex + bp * BODYPART_SIZE
            nummodels = _i(self.d, o + 64)
            modelindex = _i(self.d, o + 72)

            for m in range(nummodels):
                mo = modelindex + m * MODEL_SIZE
                numverts = _i(self.d, mo + 80)
                vertinfoindex = _i(self.d, mo + 84)
                vertindex = _i(self.d, mo + 88)

                for v in range(numverts):
                    bone = self.d[vertinfoindex + v]
                    if not (0 <= bone < self.numbones):
                        continue
                    x, y, z = _v3(self.d, vertindex + v * 12)
                    acc[bone][0] += x
                    acc[bone][1] += y
                    acc[bone][2] += z
                    num[bone] += 1

        return [
            (a[0] / n, a[1] / n, a[2] / n) if n else None
            for a, n in zip(acc, num)
        ]

    def attachments(self):
        out = []
        for i in range(self.numattachments):
            o = self.attachmentindex + i * ATTACH_SIZE
            out.append((_cstr(self.d[o:o + 32]), _i(self.d, o + 36), _v3(self.d, o + 40)))
        return out

    # -- animation --------------------------------------------------------

    def sequences(self):
        out = []
        for i in range(self.numseq):
            o = self.seqindex + i * SEQ_SIZE
            out.append({
                "index": i,
                "label": _cstr(self.d[o:o + 32]),
                "numframes": _i(self.d, o + 56),
                "numblends": _i(self.d, o + 120),
                "animindex": _i(self.d, o + 124),
                "seqgroup": _i(self.d, o + 156),
            })
        return out

    def _anim_value(self, anim_base, bone_index, axis, frame):
        """One channel of one bone on one frame, out of the RLE stream.

        GoldSrc stores each channel as runs of (valid, total): `valid`
        explicit values followed by `total - valid` repeats of the last one.
        Walk runs until the frame falls inside one.
        """
        b = self.bones[bone_index]
        po = anim_base + bone_index * 12
        off = struct.unpack_from("<H", self.d, po + axis * 2)[0]

        if off == 0:
            return b.value[axis]

        p = po + off
        k = frame

        while True:
            valid = self.d[p]
            total = self.d[p + 1]
            if total == 0:
                return b.value[axis]        # malformed; fall back to rest
            if total > k:
                break
            k -= total
            p += (valid + 1) * 2

        valid = self.d[p]
        if valid > k:
            raw = struct.unpack_from("<h", self.d, p + 2 + k * 2)[0]
        else:
            raw = struct.unpack_from("<h", self.d, p + 2 + (valid - 1) * 2)[0]

        return b.value[axis] + raw * b.scale[axis]

    def bone_motion(self):
        """Per bone, how far it moves and how far it turns, across EVERY
        sequence.

        Every sequence, not just the one named after the action: the census
        found parts that only move in a sequence nobody would have guessed,
        and a generator that looks at `reload` alone finds nothing on half
        the arsenal.

        Measured RELATIVE TO THE BONE'S OWN REST POSE, which for a part
        parented to the weapon body is the same as relative to the weapon -
        and unlike a parent-relative delta it does not vanish when the parent
        moves too.
        """
        span = []
        for b in self.bones:
            span.append({
                "pos_min": [1e30] * 3, "pos_max": [-1e30] * 3,
                "rot_min": [1e30] * 3, "rot_max": [-1e30] * 3,
                "seqs": set(),
            })

        for seq in self.sequences():
            if seq["seqgroup"] != 0:
                continue                    # external sequence file; skipped
            if seq["numframes"] <= 0:
                continue

            base = seq["animindex"]

            for bi in range(self.numbones):
                s = span[bi]

                # PER SEQUENCE, THEN MERGED. An earlier version folded every
                # sequence into one running span and then asked whether that
                # span had grown - which meant that once a bone moved in any
                # sequence, every sequence after it looked like it moved the
                # bone too. The pistol's slide stop came back as moving in
                # draw and holster, where it does not move at all.
                lo = [1e30] * 6
                hi = [-1e30] * 6

                for f in range(seq["numframes"]):
                    for a in range(6):
                        v = self._anim_value(base, bi, a, f)
                        if v < lo[a]:
                            lo[a] = v
                        if v > hi[a]:
                            hi[a] = v

                moved = False
                for a in range(6):
                    if hi[a] - lo[a] > 0.01:
                        moved = True

                if moved:
                    s["seqs"].add(seq["label"])

                for a in range(3):
                    if lo[a] < s["pos_min"][a]:
                        s["pos_min"][a] = lo[a]
                    if hi[a] > s["pos_max"][a]:
                        s["pos_max"][a] = hi[a]
                for a in range(3):
                    if lo[3 + a] < s["rot_min"][a]:
                        s["rot_min"][a] = lo[3 + a]
                    if hi[3 + a] > s["rot_max"][a]:
                        s["rot_max"][a] = hi[3 + a]

        return span


# ---------------------------------------------------------------------------
# classification
# ---------------------------------------------------------------------------

# THE WEAPON IS A SUBTREE, NOT A SHARE OF THE MESH.
#
# The plan's rule was "exclude the body bone - a root-parented bone owning
# more than about 40% of the mesh". Measured against real Half-Life view
# models, that rule does not hold: on v_9mmhandgun the weapon body is
# `Hands mesh`, which owns 47% of the mesh and is parented to `Bip01 R Hand`,
# eleven bones deep in an arm rig that starts at the pelvis. Nothing about it
# is root-parented.
#
# What IS true, on every model checked, is that the weapon hangs off one bone
# and the hand rig hangs off another. So: the body is the bone owning the
# most mesh, and the weapon's moving parts are its DESCENDANTS. Fingers,
# forearms and the spine are excluded not by name - "Hands mesh 2" is a
# pistol slide - but by not being part of the weapon at all.
#
# This is also why a name-based bone map scored two out of ten on four of the
# five model sets: the names are arbitrary and the structure is not.

# Bones this small are landmarks - muzzle_pos, HUD_pos - not parts. They are
# worth reporting, because a card wants the muzzle, but they do not move on
# their own.
LANDMARK_SHARE = 0.001

# Below this, a bone that "moves" is animation noise or a rounding artefact.
MIN_TRAVEL = 0.15       # model units
MIN_ROTATION = 0.05     # radians, ~3 degrees

IGNORED_BONES = ("camera_bone",)        # MMod's, and it moves in everything


def _descendants(model, root):
    """Every bone under `root`, transitively."""
    out = set()
    changed = True
    while changed:
        changed = False
        for b in model.bones:
            if b.index in out or b.index == root:
                continue
            if b.parent == root or b.parent in out:
                out.add(b.index)
                changed = True
    return out


def _children(model):
    kids = [[] for _ in range(model.numbones)]
    for b in model.bones:
        if b.parent != -1:
            kids[b.parent].append(b.index)
    return kids


def find_rig_bones(model):
    """Bones belonging to a finger rig, found by SHAPE rather than by name.

    A hand is unmistakable structurally: one bone with four or more children,
    each of which roots a chain of single-child bones two or more deep. Real
    weapon parts do not look like that - a slide is a leaf, a cylinder is a
    leaf, a muzzle landmark is at most one link long.

    Name matching cannot do this job. On v_9mmhandgun the pistol's own slide
    is called "Hands mesh 2" and the weapon body is "Hands mesh", both
    children of `Bip01 R Hand`; anything keying on "hand" or "finger" throws
    the weapon away and keeps the rig. The shape is unambiguous where the
    names are actively misleading.

    Only the chains are marked, never the whole subtree under the hand,
    because on most view models the weapon hangs off that same bone.
    """
    kids = _children(model)
    rig = set()

    def chain_from(i):
        """The chain of single-child bones starting at i, or None."""
        out = [i]
        cur = i
        while len(kids[cur]) == 1:
            cur = kids[cur][0]
            out.append(cur)
        if len(kids[cur]) > 1:
            return None
        return out

    for b in model.bones:
        chains = []
        for c in kids[b.index]:
            ch = chain_from(c)
            if ch and len(ch) >= 2:
                chains.append(ch)

        if len(chains) >= 4:
            for ch in chains:
                rig.update(ch)

    return rig


def find_body(model, counts, total):
    """The bone the weapon's mesh hangs off.

    THE MUZZLE KNOWS WHERE THE GUN IS. Picking the bone that owns the most
    mesh works on a pistol and fails on a revolver: on models where the hands
    are modelled in more detail than the weapon, the most-owning bone is the
    hand, and every finger then reads as a moving part. v_357 reported nine.

    So: start from the first attachment - on a view model that is the muzzle,
    which is on the weapon by definition - and walk up its ancestors, taking
    whichever owns the most mesh. That is the weapon body, whatever it is
    called and however deep in an arm rig it sits.

    Models with no attachments at all (the crowbar) fall back to the
    most-owning bone, which for a weapon with no separate hand mesh is right.
    """
    if not total:
        return -1

    att = model.attachments()
    rig = find_rig_bones(model)

    kids = _children(model)

    if att:
        # The muzzle's ancestors, AND their direct children.
        #
        # Ancestors alone is not enough: on v_egon the muzzle hangs off
        # `Bip01 R Hand`, which owns 3% of the mesh, while the weapon body
        # `egon` owns 74% and is a SIBLING of that chain rather than above
        # it. Walking upward found the hand and called it the gun.
        chain = []
        b = att[0][1]
        while b != -1 and b < model.numbones:
            chain.append(b)
            b = model.bones[b].parent

        near = set(chain)
        for c in chain:
            near.update(kids[c])

        near = [i for i in near if i not in rig]

        if near:
            best = max(near, key=lambda i: counts[i])
            if counts[best]:
                return best

    best = max(range(model.numbones), key=lambda i: counts[i])

    b = model.bones[best]
    while b.parent != -1:
        p = model.bones[b.parent]
        if counts[p.index] / total < 0.10:
            break
        b = p

    return b.index


def classify(model):
    counts, total = model.vertex_owners()
    motion = model.bone_motion()
    centroids = model.bone_centroids()

    rig = find_rig_bones(model)
    body = find_body(model, counts, total)
    inside = _descendants(model, body) if body >= 0 else set()

    # A finger rig hanging off the same bone as the weapon is not the weapon.
    inside -= rig

    parts = []

    for b in model.bones:
        share = (counts[b.index] / total) if total else 0.0
        s = motion[b.index]

        dpos = [s["pos_max"][a] - s["pos_min"][a] for a in range(3)]
        drot = [s["rot_max"][a] - s["rot_min"][a] for a in range(3)]
        travel = max(dpos)
        rotation = max(drot)

        note = []
        if b.index == body:
            note.append("WEAPON BODY")
        elif b.index not in inside:
            note.append("not part of the weapon")
        elif b.name.lower() in IGNORED_BONES:
            note.append("ignored by name")
        elif share < LANDMARK_SHARE:
            note.append("landmark")
        elif travel < MIN_TRAVEL and rotation < MIN_ROTATION:
            note.append("never moves")

        kind = None
        if not note:
            # A part that turns much more than it slides is a hinge.
            # Compared against a rough conversion rather than a tuned one: a
            # radian of swing and an inch of slide are both "a lot".
            kind = "hinge" if rotation * 4.0 > travel else "slide"

        parts.append({
            "bone": b,
            "share": share,
            "verts": counts[b.index],
            "travel": travel,
            "rotation": rotation,
            "kind": kind,
            "seqs": sorted(s["seqs"]),
            "note": ", ".join(note),
            "centroid": centroids[b.index],
            "inside": b.index in inside or b.index == body,
            "role": None,
            "reload_only": False,
            "chain_len": 0,
        })

    _mark_round_sets(parts)
    _mark_static_parts(parts)
    _mark_arrivals(model, parts)
    _mark_chains(model, parts)
    _mark_reload_only(parts)

    return parts


def _mark_round_sets(parts):
    """Siblings that are copies of each other are a cylinder's worth of rounds.

    The revolver has six bones called SHELL01..06 that swing out during
    reload; the crossbow has its bolts. The plan gives these a ROLE -
    "appearing only in reload and travelling in from outside" - and they are
    emphatically not joints: a card declaring six hinges would have the
    simulator trying to drive the ejecting brass.

    DISTANCE CANNOT SEPARATE THEM FROM A MAGAZINE. The plan's own sketch has
    the pistol magazine travelling 21 units, further than any shell, because
    a magazine's measured travel is however far the reload animation carries
    it. So "it moves a long way" identifies nothing.

    What does is that rounds come in SETS and magazines come alone: three or
    more siblings that are near-copies of each other by vertex count. Travel
    corroborates, loosely. Rotation is deliberately not used - Euler angles
    wrap, so one shell of six reads as rotating 4.5 radians where its
    identical siblings read 2.0, and the odd one out would be dropped from
    its own set.

    Structural throughout, because the names are whatever the artist typed.
    """
    by_parent = {}

    for p in parts:
        if not p["kind"]:
            continue
        by_parent.setdefault(p["bone"].parent, []).append(p)

    for group in by_parent.values():
        if len(group) < 3:
            continue

        verts = sorted(g["verts"] for g in group)
        vmid = verts[len(verts) // 2]

        if vmid <= 0:
            continue

        travels = sorted(g["travel"] for g in group)
        tmid = travels[len(travels) // 2]

        alike = [
            g for g in group
            if abs(g["verts"] - vmid) <= 0.40 * vmid
            and abs(g["travel"] - tmid) <= 0.60 * tmid + 0.5
        ]

        if len(alike) >= 3:
            for g in alike:
                g["role"] = "round"


def _mark_static_parts(parts):
    """Parts the artist modelled and never animated.

    A bone inside the weapon that owns real mesh and never moves in any
    sequence is a part that EXISTS but has no motion to measure - the
    revolver's hammer, the pistol's `BoxPistol`. Those are not failures of
    the model; they are the content ceiling the plan's mesh surgery (E-02) is
    aimed at, and its first targets are exactly this list: the pistol's slide
    stop and magazine release, the revolver's hammer and cylinder latch.

    Worth reporting loudly, because a bone that already exists needs no mesh
    surgery at all - it needs a card line giving it a travel, which is a
    minute's work instead of an afternoon's.
    """
    for p in parts:
        if p["note"] == "never moves" and p["share"] >= 0.005:
            p["role"] = "static"


def _mark_arrivals(model, parts):
    """Parts that travel further than the whole weapon is long.

    A speedloader on the revolver travels 66 units; the weapon's bounding box
    diagonal is a fraction of that. Nothing hinged or sliding on a gun moves
    that far - the part is simply parked off-screen until the reload brings
    it in, which makes it an OBJECT the feed path hands over, not a joint.

    Judged against the model's own extent rather than against a number of
    units, because the same absolute travel means different things on a
    pistol and on a rocket launcher. The plan's own example - a magazine
    measured at 21 units because that is where the reload animation carries
    it - stays a joint under this test, which is the point: `out_at` exists
    precisely so an overlong magazine travel can be corrected rather than
    reclassified.
    """
    extent = model.extent()

    for p in parts:
        if not p["kind"] or p["role"]:
            continue
        # A margin, because a magazine's measured travel is however far the
        # reload animation carries it and that can legitimately approach the
        # weapon's own length. Comfortably past it is another matter.
        if p["travel"] > 1.5 * extent:
            p["role"] = "arrival"


def _mark_chains(model, parts):
    """Segmented parts that bend as one thing.

    The crossbow's limbs are LA1..LA4 and RA1..RA4: two bows, each cut into
    four bones so the curve deforms smoothly, every bone turning about a
    fifth of a radian in the same sequences. Read naively that is eight
    joints, and a card declaring eight would have the player able to grab the
    second segment of the left limb.

    It is two. A chain of single-child bones that all move, by similar
    amounts, in the same sequences, is ONE part with a driver at its root -
    the rest follow. The card declares the root; the renderer can distribute
    the bend across the followers however the artist intended.

    Not the same test as the hand rig: that one keys on four or more chains
    meeting at a bone, this one on a single chain moving as a unit. A hand
    has many short stiff chains; a bow limb is one long soft one.
    """
    kids = _children(model)
    by_index = {p["bone"].index: p for p in parts}

    for b in model.bones:
        # start only at the top of a chain
        if b.parent != -1 and len(kids[b.parent]) == 1:
            continue

        chain = [b.index]
        cur = b.index
        while len(kids[cur]) == 1:
            cur = kids[cur][0]
            chain.append(cur)

        moving = [i for i in chain if by_index[i]["kind"]]

        if len(moving) < 3:
            continue

        amounts = [max(by_index[i]["travel"], by_index[i]["rotation"]) for i in moving]
        lo, hi = min(amounts), max(amounts)

        if lo <= 0 or hi > 3.0 * lo:
            continue            # not moving as a unit

        seqs = [set(by_index[i]["seqs"]) for i in moving]
        if not set.intersection(*seqs):
            continue            # they do not even move at the same times

        for i in moving[1:]:
            by_index[i]["role"] = "chain"

        by_index[moving[0]]["chain_len"] = len(moving)


def _mark_reload_only(parts):
    """Parts that only move while reloading.

    A strong hint about role: a slide cycles when firing too, while a
    magazine, a speedloader or a rocket only ever moves during a reload. Not
    a decision - some weapons animate the slide only in reload - but it is
    the first thing a person reading the card wants to know.
    """
    for p in parts:
        if not p["kind"] or not p["seqs"]:
            continue
        if all("reload" in sq.lower() for sq in p["seqs"]):
            p["reload_only"] = True


# ---------------------------------------------------------------------------
# output
# ---------------------------------------------------------------------------

def report(model, parts):
    bones, seqs, h = model.fingerprint()
    print("=== %s" % model.name)
    print("    %d bones, %d sequences, fingerprint 0x%08x" % (bones, seqs, h))

    att = model.attachments()
    if att:
        print("    muzzle attachment on bone %d (%s)"
              % (att[0][1], model.bones[att[0][1]].name))

    outside = len([p for p in parts if not p["inside"]])

    print()
    print("    %-22s %6s %6s %7s %7s  %s" %
          ("bone", "verts", "share", "travel", "rot", "verdict"))

    for p in parts:
        if not p["inside"]:
            continue

        if p["note"] and p["role"] != "static":
            verdict = p["note"]
        elif p["role"] == "static":
            verdict = "EXISTS BUT NEVER ANIMATED - give it a travel"
        elif p["role"] == "round":
            verdict = "round or case (one of a set)"
        elif p["role"] == "arrival":
            verdict = "arrives from outside - an object, not a joint"
        elif p["role"] == "chain":
            verdict = "follows the segment above it"
        else:
            verdict = "CANDIDATE %s" % p["kind"]
            if p["chain_len"]:
                verdict += ", drives %d segments" % p["chain_len"]
            if p["reload_only"]:
                verdict += ", reload only"

        print("    %-22s %6d %5.1f%% %7.2f %7.3f  %s" %
              (p["bone"].name[:22], p["verts"], p["share"] * 100.0,
               p["travel"], p["rotation"], verdict))

        if p["kind"] and p["seqs"] and p["role"] != "round":
            print("    %-22s   moves in: %s" % ("", ", ".join(p["seqs"][:6])))

    print("    (%d bones outside the weapon - arm, hand and fingers - not shown)"
          % outside)

    joints = [p for p in parts if p["kind"] and not p["role"]]
    rounds = [p for p in parts if p["role"] == "round"]
    static = [p for p in parts if p["role"] == "static"]

    print()
    arrivals = [p for p in parts if p["role"] == "arrival"]

    print("    %d drivable joint%s, %d round/case bone%s, %d arriving object%s, "
          "%d modelled but never animated"
          % (len(joints), "" if len(joints) == 1 else "s",
             len(rounds), "" if len(rounds) == 1 else "s",
             len(arrivals), "" if len(arrivals) == 1 else "s",
             len(static)))
    print("    weapon extent %.1f units" % model.extent())
    print()


def draft_card(model, parts):
    bones, seqs, h = model.fingerprint()
    name = os.path.splitext(model.name)[0]
    cand = [p for p in parts if p["kind"] and not p["role"]]
    rounds = [p for p in parts if p["role"] == "round"]
    static = [p for p in parts if p["role"] == "static"]

    out = []
    out.append("# Drafted by vrcardgen from %s" % model.name)
    out.append("#")
    out.append("# EVERY JOINT BELOW IS A MEASUREMENT, NOT A DECISION. The tool")
    out.append("# found bones that move and measured how far; it did not decide")
    out.append("# which is a slide and which is a magazine, and it has declared")
    out.append("# no catch, no control and no feed point. Those are the lines a")
    out.append("# person adds after looking at the model.")
    out.append("#")
    out.append("# Until a `type` line is added this card declares joints and")
    out.append("# nothing else, which means it will bind and pose but not feed.")
    out.append("")
    out.append("weapon %s" % name)
    out.append("match  bones %d  seqs %d  hash 0x%08x" % (bones, seqs, h))
    out.append("")
    out.append("# type   magazine_slide  capacity ??  rpm ??")
    out.append("")

    objects = [p for p in parts if p["role"] in ("round", "arrival")]

    if objects:
        out.append("# %d bones are objects the feed path owns, not joints:" % len(objects))
        out.append("#   %s" % ", ".join(p["bone"].name for p in objects))
        out.append("# They are NOT joints. Once the feed path owns them the")
        out.append("# renderer draws them from chamber state instead.")
        out.append("")

    if static:
        out.append("# These bones exist and own mesh but are never animated, so")
        out.append("# there is no travel to measure. They need a travel stated by")
        out.append("# hand - which is a minute's work, and far cheaper than the")
        out.append("# mesh surgery a part with no bone at all would need:")
        for p in static:
            out.append('#   joint ?  bone "%s"  slide  travel ??  rest 0    # owns %.1f%%'
                       % (p["bone"].name, p["share"] * 100.0))
        out.append("")

    if not cand:
        out.append("# No part bones found. This model has nothing drivable, which")
        out.append("# is a real answer: the weapon falls back to its own reload.")
        return "\n".join(out) + "\n"

    for i, p in enumerate(cand):
        b = p["bone"]
        amount = p["rotation"] if p["kind"] == "hinge" else p["travel"]
        out.append('joint %d  bone "%s"  %s  travel %.2f  rest 0'
                   % (i, b.name, p["kind"], amount))
        if p["centroid"]:
            out.append("    grab  point %.2f %.2f %.2f  radius 2.5"
                       % p["centroid"])
        out.append("    # owns %.1f%% of the mesh; moves in %s"
                   % (p["share"] * 100.0, ", ".join(p["seqs"][:4]) or "nothing"))
        out.append("")

    return "\n".join(out) + "\n"



# ---------------------------------------------------------------------------
# preview
#
# The plan asks for exactly this: "a synthetic part is defined by looking at
# the model, not by guessing coordinates in a headset". A box guessed wrong
# detaches the wrong half of a gun, and the only place that shows up is in
# somebody's hands.
# ---------------------------------------------------------------------------

_PALETTE = ["#d94f3d", "#2f7fd1", "#3aa675", "#c9a227", "#8a5bd6",
            "#d1568f", "#4aa8c0", "#8a7f6b"]


def _frame(model):
    """A weapon-aligned frame: forward toward the muzzle, with a roll chosen
    for stability rather than for meaning."""
    att = model.attachments()
    fwd = None

    if att:
        v = list(model.bones[att[0][1]].value[:3])
        n = (v[0] ** 2 + v[1] ** 2 + v[2] ** 2) ** 0.5
        if n > 0.01:
            fwd = [c / n for c in v]

    if fwd is None:
        fwd = [0.0, -1.0, 0.0]

    ref = [0.0, 0.0, 1.0]
    if abs(ref[0] * fwd[0] + ref[1] * fwd[1] + ref[2] * fwd[2]) > 0.9:
        ref = [1.0, 0.0, 0.0]

    d = sum(ref[i] * fwd[i] for i in range(3))
    up = [ref[i] - d * fwd[i] for i in range(3)]
    n = (up[0] ** 2 + up[1] ** 2 + up[2] ** 2) ** 0.5
    up = [c / n for c in up]

    right = [fwd[1] * up[2] - fwd[2] * up[1],
             fwd[2] * up[0] - fwd[0] * up[2],
             fwd[0] * up[1] - fwd[1] * up[0]]

    return fwd, up, right


def preview(model, parts, path):
    counts, total = model.vertex_owners()
    fwd, up, right = _frame(model)

    body = None
    for p in parts:
        if p["note"] == "WEAPON BODY":
            body = p["bone"].index

    keep = dict((p["bone"].index, p["bone"].name) for p in parts if p["inside"])

    # each kept bone's offset from the body, so everything plots in one frame
    offs = {}
    for b in model.bones:
        if b.index not in keep:
            continue
        o = [0.0, 0.0, 0.0]
        i = b.index
        while i != -1 and i != body:
            for a in range(3):
                o[a] += model.bones[i].value[a]
            i = model.bones[i].parent
        offs[b.index] = o

    pts = []
    for bp in range(model.numbodyparts):
        o = model.bodypartindex + bp * BODYPART_SIZE
        nm = _i(model.d, o + 64)
        mi = _i(model.d, o + 72)
        for k in range(nm):
            mo = mi + k * MODEL_SIZE
            nv = _i(model.d, mo + 80)
            vi = _i(model.d, mo + 84)
            vx = _i(model.d, mo + 88)
            for v in range(nv):
                bi = model.d[vi + v]
                if bi not in keep:
                    continue
                q = _v3(model.d, vx + v * 12)
                d = offs[bi]
                w = [q[a] + d[a] for a in range(3)]
                pts.append((bi,
                            sum(w[a] * fwd[a] for a in range(3)),
                            sum(w[a] * up[a] for a in range(3)),
                            sum(w[a] * right[a] for a in range(3))))

    if not pts:
        print("    nothing to preview")
        return

    order = sorted(set(p[0] for p in pts))
    colour = dict((b, _PALETTE[i % len(_PALETTE)]) for i, b in enumerate(order))

    lo = [min(p[1 + a] for p in pts) - 1.0 for a in range(3)]
    hi = [max(p[1 + a] for p in pts) + 1.0 for a in range(3)]

    SC = 26.0
    GUT = 48
    views = [("side   -  forward x up", 0, 1),
             ("below  -  forward x right", 0, 2),
             ("front  -  right x up", 2, 1)]

    W = max((hi[h] - lo[h]) for _, h, _ in views) * SC + GUT * 2
    H = sum((hi[v] - lo[v]) for _, _, v in views) * SC + GUT * (len(views) + 1) + 40 + 22 * len(order)

    o = []
    o.append('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d" font-family="ui-monospace,monospace">' % (int(W), int(H), int(W), int(H)))
    o.append('<rect width="100%" height="100%" fill="#14161a"/>')
    o.append('<text x="%d" y="26" fill="#e8e6e3" font-size="15">%s  -  %d bones - grid is 1 model unit, labels every 5</text>' % (GUT, model.name, model.numbones))

    y0 = 54

    for title, h, v in views:
        wpx = (hi[h] - lo[h]) * SC
        hpx = (hi[v] - lo[v]) * SC

        o.append('<text x="%d" y="%d" fill="#9aa0a6" font-size="12">%s</text>' % (GUT, y0 - 7, title))
        o.append('<rect x="%d" y="%d" width="%.1f" height="%.1f" fill="#0d0f12" stroke="#2b2f36"/>' % (GUT, y0, wpx, hpx))

        g = int(lo[h]) - 1
        while g <= hi[h]:
            x = GUT + (g - lo[h]) * SC
            if GUT <= x <= GUT + wpx:
                strong = (g % 5 == 0)
                o.append('<line x1="%.1f" y1="%d" x2="%.1f" y2="%.1f" stroke="%s"/>' % (x, y0, x, y0 + hpx, "#39404a" if strong else "#21252b"))
                if strong:
                    o.append('<text x="%.1f" y="%.1f" fill="#6b7280" font-size="10" text-anchor="middle">%d</text>' % (x, y0 + hpx + 13, g))
            g += 1

        g = int(lo[v]) - 1
        while g <= hi[v]:
            y = y0 + (hi[v] - g) * SC
            if y0 <= y <= y0 + hpx:
                strong = (g % 5 == 0)
                o.append('<line x1="%d" y1="%.1f" x2="%.1f" y2="%.1f" stroke="%s"/>' % (GUT, y, GUT + wpx, y, "#39404a" if strong else "#21252b"))
                if strong:
                    o.append('<text x="%d" y="%.1f" fill="#6b7280" font-size="10" text-anchor="end">%d</text>' % (GUT - 6, y + 3, g))
            g += 1

        for bi, f, u, r in pts:
            c = [f, u, r]
            x = GUT + (c[h] - lo[h]) * SC
            y = y0 + (hi[v] - c[v]) * SC
            o.append('<circle cx="%.1f" cy="%.1f" r="1.8" fill="%s" opacity="0.85"/>' % (x, y, colour[bi]))

        y0 += hpx + GUT

    o.append('<text x="%d" y="%d" fill="#9aa0a6" font-size="12">bones (vertex count):</text>' % (GUT, y0 - 12))
    for bi in order:
        o.append('<circle cx="%d" cy="%d" r="5" fill="%s"/>' % (GUT + 6, y0 + 5, colour[bi]))
        o.append('<text x="%d" y="%d" fill="#e8e6e3" font-size="12">%s  (%d)</text>' % (GUT + 18, y0 + 9, keep[bi], counts[bi]))
        y0 += 22

    o.append('</svg>')

    fh = open(path, "w")
    fh.write("\n".join(o))
    fh.close()

    print("    wrote %s  (%d vertices, %d bones)" % (path, len(pts), len(order)))


def census(root):
    rows = []
    seen = set()

    for dirpath, _, files in os.walk(root):
        for fn in files:
            if not fn.lower().endswith(".mdl"):
                continue
            path = os.path.join(dirpath, fn)

            # One row per model, not per copy. A model set commonly appears
            # under several paths - an `animov` subdirectory, an HD overlay -
            # and listing the same fingerprint three times says nothing.
            key = os.path.realpath(path)
            if key in seen:
                continue
            seen.add(key)
            try:
                m = Model(path)
            except Exception:
                continue
            if not fn.lower().startswith("v_"):
                continue
            try:
                parts = classify(m)
            except Exception as exc:
                rows.append((fn, m.numbones, m.numseq, 0, "failed: %s" % exc))
                continue
            b, s, h = m.fingerprint()
            joints = len([p for p in parts if p["kind"] and not p["role"]])
            rounds = len([p for p in parts if p["role"] in ("round", "arrival")])
            static = len([p for p in parts if p["role"] == "static"])
            rows.append((fn, b, s, h, "%2d joints  %2d rounds  %2d static"
                         % (joints, rounds, static)))

    rows.sort()
    print("%-26s %6s %5s %12s  %s" % ("model", "bones", "seqs", "fingerprint", "parts"))
    for fn, b, s, h, note in rows:
        print("%-26s %6d %5d   0x%08x  %s" % (fn[:26], b, s, h, note))
    print("\n%d view models" % len(rows))


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2

    if argv[1] == "--preview":
        if len(argv) < 3:
            print("--preview needs a model")
            return 2
        mp = Model(argv[2])
        pp = classify(mp)
        report(mp, pp)
        preview(mp, pp, argv[3] if len(argv) > 3 else
                os.path.splitext(os.path.basename(argv[2]))[0] + ".svg")
        return 0

    if argv[1] == "--census":
        if len(argv) < 3:
            print("--census needs a directory")
            return 2
        census(argv[2])
        return 0

    outdir = None
    args = argv[1:]
    if args[0] == "--write":
        outdir = args[1]
        args = args[2:]
        os.makedirs(outdir, exist_ok=True)

    for path in args:
        try:
            m = Model(path)
        except Exception as exc:
            print("%s: %s" % (path, exc))
            continue

        parts = classify(m)
        report(m, parts)
        card = draft_card(m, parts)

        if outdir:
            dest = os.path.join(outdir, os.path.splitext(m.name)[0] + ".card")
            with open(dest, "w") as fh:
                fh.write(card)
            print("    wrote %s" % dest)
        else:
            print(card)

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
