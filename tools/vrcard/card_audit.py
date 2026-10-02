"""Check what a card CLAIMS against what the model actually is.

Two tools already check a card and neither checks this. `vr_card_check` proves
a card PARSES and prints what the simulator will see; `vrfingerprint_check`
proves a card BINDS to a given model. Between them sits everything a card
asserts about geometry - that a bone exists, that a part travels 10.14 units,
that a grab point is somewhere a hand could reach - and nothing verified any of
it.

That gap is where the 73 defects an earlier audit pass found mostly live, and
PCVR_WEAPONS_STATUS.md says why it matters: "Those comments matter: the next
lane reads them as spec." A card that declares `travel 4.81` on a bone that
moves 6.2 is not a cosmetic error. The simulator normalises every joint to
0..VRF_ONE over its declared travel, so a travel that is wrong by a third means
every detent, catch and feed point on that joint is wrong by a third, and the
card still parses, still binds, and still looks right.

    python card_audit.py <card> [<card>...] [--models DIR] [--quiet]
    python card_audit.py --all              # every card against the set it binds to

WHAT IT CHECKS, in rough order of how badly each one bites:

  the fingerprint         bones, sequences and the FNV-1a of the bone names
  every bone named        body, joints, synth sources - a name that is not
                          there means the card drives nothing at all
  every declared travel   against the bone's measured range over all
                          sequences, which is what the generator measures
  every declared point    grab, control, chamber, eject, strike, pry - inside
                          the weapon's own vertex bounds, because a point
                          outside them is somewhere no hand goes

WHAT IT DOES NOT CHECK, so nobody reads more into a PASS than is there: it says
nothing about whether a travel is the RIGHT part of a bone's range (a bone that
slides in one animation and is parked elsewhere in another has a range larger
than its travel), nothing about the prose in the comments, and nothing about
whether a control is on the lever a player expects. A clean run means the
numbers are consistent with the model, not that the card is good.
"""
import math
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vrcardgen as V
from find_controls import body_verts

# How far a declared travel may differ from the bone's measured range before it
# is worth a human looking. Generous: a bone's full range over every sequence
# is an upper bound on any one gesture's travel, so under-declaring is normal
# and only a declared travel LARGER than the measured range is certainly wrong.
TRAVEL_SLACK = 0.25

# How far outside the weapon's vertex bounds a declared point may sit. A grab
# point is meant to be ON the part, but a centroid of a thin shell plus a
# radius legitimately pushes a little outside.
POINT_SLACK = 3.0

# Where the card sets live, and which model directory each binds to. From
# PCVR_WEAPONS_STATUS.md's measured table - cards/valve/ binds to the Half-Life
# VR Mod's rigs and NOT to retail, which is why it is not listed here and why
# auditing it against retail would report nonsense.
CARD_SETS = {
    "hd": ["valve_hd", "gearbox_hd", "bshift_hd"],
    "gearbox": ["gearbox"],
}

DEFAULT_MODELS = r"D:\SteamLibrary\steamapps\common\Half-Life"


class Finding(object):
    __slots__ = ("card", "severity", "what")

    def __init__(self, card, severity, what):
        self.card = card
        self.severity = severity
        self.what = what


def parse_card(path):
    """What the card declares, as flat data. Not a reimplementation of the
    parser - only the declarations this tool can check against a model."""
    out = {
        "weapon": None, "body": None,
        "match": {}, "joints": [], "synths": [], "points": [],
    }
    joint = None

    for lineno, raw in enumerate(open(path, encoding="utf-8", errors="replace"), 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        tok = line.split()
        key = tok[0].lower()

        if key == "weapon" and len(tok) > 1:
            out["weapon"] = tok[1]

        elif key == "body" and len(tok) > 1:
            out["body"] = (line.split(None, 1)[1].strip().strip('"'), lineno)

        elif key == "match":
            for i in range(1, len(tok) - 1):
                k = tok[i].lower()
                if k in ("bones", "seqs"):
                    try:
                        out["match"][k] = int(tok[i + 1])
                    except ValueError:
                        pass
                elif k == "hash":
                    try:
                        out["match"]["hash"] = int(tok[i + 1], 16)
                    except ValueError:
                        pass

        elif key == "joint":
            m = re.search(r'bone\s+"([^"]+)"', line)
            t = re.search(r"travel\s+(-?[\d.]+)", line)
            joint = {
                "index": tok[1] if len(tok) > 1 else "?",
                "bone": m.group(1) if m else None,
                "travel": float(t.group(1)) if t else None,
                "line": lineno,
            }
            out["joints"].append(joint)

        elif key == "synth":
            m = re.search(r'from\s+"([^"]+)"', line)
            out["synths"].append({
                "name": tok[1] if len(tok) > 1 else "?",
                "src": m.group(1) if m else None,
                "line": lineno,
            })

        elif key in ("grab", "control", "chamber", "eject", "strike", "pry"):
            # Every one of these carries a position, spelled `point x y z` or
            # `at x y z`. Taken from the line rather than from a grammar,
            # because this tool must not become a second parser that can
            # disagree with the real one about what a card says.
            m = re.search(r"(?:point|at)\s+(-?[\d.]+)\s+(-?[\d.]+)\s+(-?[\d.]+)", line)
            if m:
                out["points"].append({
                    "kind": key,
                    "pos": tuple(float(m.group(i)) for i in (1, 2, 3)),
                    "line": lineno,
                })

    return out


def find_models(weapon, models_dir, dirs):
    """Every copy of this model name, because ONE NAME IS SEVERAL FILES.

    `v_9mmhandgun.mdl` exists in valve_hd, gearbox_hd and bshift_hd and they
    are different rigs - which is the whole reason a card binds by fingerprint
    rather than by filename, and why two cards in this directory name the same
    weapon. A tool that took the first file it found reported three cards as
    broken on its first run; they were fine, and it was comparing the Blue
    Shift cards against Valve's model.
    """
    out = []
    for d in dirs:
        p = os.path.join(models_dir, d, "models", weapon + ".mdl")
        if os.path.exists(p):
            out.append((d, p))
    return out


def audit(card_path, models_dir, card_set):
    name = os.path.basename(card_path)
    f = []
    decl = parse_card(card_path)

    if not decl["weapon"]:
        f.append(Finding(name, "FAIL", "no `weapon` line, so nothing can be looked up"))
        return f

    found = find_models(decl["weapon"], models_dir, CARD_SETS[card_set])

    if not found:
        f.append(Finding(name, "SKIP", "no model for %s under %s"
                         % (decl["weapon"], "/".join(CARD_SETS[card_set]))))
        return f

    # The one it BINDS to, which is the only one its numbers describe. A card
    # whose fingerprint matches none of them is the interesting failure.
    want = decl["match"]
    bound = None
    tried = []

    for d, path in found:
        mm = V.Model(path)
        nb, ns, h = mm.fingerprint()
        tried.append("%s(%d/%d/0x%08x)" % (d, nb, ns, h))

        if ( want.get("bones") in (None, nb) and want.get("seqs") in (None, ns)
                and want.get("hash") in (None, h) ):
            bound = (d, mm)
            break

    if not bound:
        f.append(Finding(name, "FAIL",
                         "match bones %s seqs %s hash %s binds to none of: %s"
                         % (want.get("bones"), want.get("seqs"),
                            ("0x%08x" % want["hash"]) if "hash" in want else None,
                            ", ".join(tried))))
        return f

    m = bound[1]
    names = set(b.name for b in m.bones)
    synth_names = set(s["name"] for s in decl["synths"] if s["name"])

    # The fingerprint is already proved: finding the bound model above IS
    # the check, and a card that binds to nothing returned early.

    # ---- every bone the card names ---------------------------------------
    if decl["body"]:
        b, ln = decl["body"]
        if b not in names:
            f.append(Finding(name, "FAIL", 'line %d: body bone "%s" is not in the model'
                             % (ln, b)))

    for s in decl["synths"]:
        if s["src"] and s["src"] not in names:
            f.append(Finding(name, "FAIL", 'line %d: synth %s carves from "%s", which is not in the model'
                             % (s["line"], s["name"], s["src"])))

    for j in decl["joints"]:
        if not j["bone"]:
            continue
        if j["bone"] in synth_names:
            continue        # a synthetic bone exists only after surgery
        if j["bone"] not in names:
            f.append(Finding(name, "FAIL", 'line %d: joint %s names bone "%s", which is not in the model'
                             % (j["line"], j["index"], j["bone"])))

    # ---- every declared travel -------------------------------------------
    motion = m.bone_motion()
    byname = dict((b.name, b) for b in m.bones)

    for j in decl["joints"]:
        if j["travel"] is None or not j["bone"] or j["bone"] in synth_names:
            continue
        b = byname.get(j["bone"])
        if not b:
            continue
        s = motion[b.index]
        rng = max(s["pos_max"][a] - s["pos_min"][a] for a in range(3))
        rot = max(s["rot_max"][a] - s["rot_min"][a] for a in range(3))

        # A hinge's travel is declared in radians and its position barely
        # moves, so comparing it against a positional range says nothing.
        if rot * 4.0 > rng:
            continue

        if j["travel"] > rng + TRAVEL_SLACK:
            f.append(Finding(name, "FAIL",
                             "line %d: joint %s declares travel %.2f on \"%s\", which moves only %.2f"
                             % (j["line"], j["index"], j["travel"], j["bone"], rng)))
        elif rng > 0.2 and j["travel"] < rng - TRAVEL_SLACK:
            f.append(Finding(name, "note",
                             "line %d: joint %s declares travel %.2f, bone \"%s\" ranges %.2f"
                             % (j["line"], j["index"], j["travel"], j["bone"], rng)))

    # ---- every declared point --------------------------------------------
    bi = byname[decl["body"][0]].index if ( decl["body"] and decl["body"][0] in byname ) else None

    if bi is not None:
        verts = body_verts(m, bi)
        if verts:
            lo = [min(p[a] for p in verts) for a in range(3)]
            hi = [max(p[a] for p in verts) for a in range(3)]

            for pt in decl["points"]:
                out_by = 0.0
                for a in range(3):
                    if pt["pos"][a] < lo[a]:
                        out_by = max(out_by, lo[a] - pt["pos"][a])
                    elif pt["pos"][a] > hi[a]:
                        out_by = max(out_by, pt["pos"][a] - hi[a])

                if out_by > POINT_SLACK:
                    # NAME THE BONE IT WOULD FIT, which turns a complaint into
                    # a fix. The one real failure this tool found on its first
                    # clean run was the HD shotgun declaring `body
                    # "R_Arm_bone"` while every position in it was measured in
                    # `Reciever` - and knowing that took half an hour of
                    # looking. The answer was in the model the whole time.
                    fits = []
                    for ob in m.bones:
                        if ob.index == bi:
                            continue
                        ov = body_verts(m, ob.index)
                        if len(ov) < 20:
                            continue
                        olo = [min(q[a] for q in ov) for a in range(3)]
                        ohi = [max(q[a] for q in ov) for a in range(3)]
                        if all( olo[a] - POINT_SLACK <= pt["pos"][a] <= ohi[a] + POINT_SLACK
                                for a in range(3) ):
                            fits.append((len(ov), ob.name))

                    fits.sort(reverse=True)
                    hint = ""
                    if fits:
                        hint = " - but it IS inside \"%s\" (%d verts)" % (fits[0][1], fits[0][0])

                    f.append(Finding(name, "FAIL",
                                     "line %d: %s point %.2f %.2f %.2f is %.1f units outside body \"%s\"%s"
                                     % (pt["line"], pt["kind"], pt["pos"][0], pt["pos"][1],
                                        pt["pos"][2], out_by, decl["body"][0], hint)))

                # A point at the origin is almost always a placeholder nobody
                # came back to - which is how two cards shipped controls at
                # 0 0 0.
                if pt["kind"] == "control" and pt["pos"] == (0.0, 0.0, 0.0):
                    f.append(Finding(name, "note",
                                     "line %d: control at 0 0 0 - placeholder?" % pt["line"]))

    return f


def main(argv):
    models_dir = DEFAULT_MODELS
    if "--models" in argv:
        i = argv.index("--models")
        models_dir = argv[i + 1]
        del argv[i:i + 2]

    quiet = "--quiet" in argv
    if quiet:
        argv.remove("--quiet")

    here = os.path.dirname(os.path.abspath(__file__))
    jobs = []

    if "--all" in argv or not argv:
        for cset in sorted(CARD_SETS):
            d = os.path.join(here, "cards", cset)
            if not os.path.isdir(d):
                continue
            for fn in sorted(os.listdir(d)):
                if fn.endswith(".card"):
                    jobs.append((os.path.join(d, fn), cset))
    else:
        for a in argv:
            cset = os.path.basename(os.path.dirname(os.path.abspath(a)))
            jobs.append((a, cset if cset in CARD_SETS else "hd"))

    if not os.path.isdir(models_dir):
        print("no model directory at %s - pass --models DIR" % models_dir)
        return 2

    all_f = []
    for path, cset in jobs:
        all_f.extend(audit(path, models_dir, cset))

    fails = [x for x in all_f if x.severity == "FAIL"]
    notes = [x for x in all_f if x.severity == "note"]
    skips = [x for x in all_f if x.severity == "SKIP"]

    last = None
    for x in fails + ([] if quiet else notes):
        if x.card != last:
            print("\n%s" % x.card)
            last = x.card
        print("  %-4s %s" % (x.severity, x.what))

    if skips and not quiet:
        print("\nskipped (no model on disk):")
        for x in skips:
            print("  %-28s %s" % (x.card, x.what))

    print("\n%d cards audited: %d FAIL, %d note, %d skipped"
          % (len(jobs), len(fails), len(notes), len(skips)))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
