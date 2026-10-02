"""Install weapon cards into a game installation, choosing by fingerprint.

    python install_cards.py <install-root> [--dry-run]

An install root is a directory holding gamedirs - `valve`, `gearbox`, `bshift`
and so on. For every `v_*.mdl` in each gamedir, this measures the model and
installs the one card in `cards/` whose fingerprint actually matches it, at
`<gamedir>/vr/cards/<basename>.card`.

CHOOSING BY FINGERPRINT RATHER THAN BY DIRECTORY NAME IS THE WHOLE POINT. Our
cards are filed under cards/hd, cards/gearbox and cards/valve, but those names
describe where they were measured, not where they belong: the 18 under cards/hd
match retail valve_hd, gearbox_hd and bshift_hd alike, while the 12 under
cards/valve match the Half-Life VR Mod's rigs and nothing retail. Copying by
directory name would put a card next to a model it declines.

ONE CARD PER WEAPON PER GAMEDIR, and that is a hard limit, not a choice here:
the game DLL looks a card up at exactly `vr/cards/<model basename>.card`
(vr_weapon_game.cpp:263), so a gamedir cannot hold both the SD and the HD card
for the same weapon. Whichever rig is actually installed is the one to serve. A
player who toggles HD models after installing cards will silently fall back to
vanilla weapons for anything whose rig changed - re-run this after any such
change.
"""
import glob
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vrcardgen

HERE = os.path.dirname(os.path.abspath(__file__))
CARDS = os.path.join(HERE, 'cards')


def read_match(path):
    """The fingerprint a card declares, or None."""
    try:
        txt = open(path, encoding='utf-8', errors='ignore').read()
    except OSError:
        return None
    for line in txt.splitlines():
        s = line.strip()
        if not s.startswith('match'):
            continue
        t = s.split()
        bones = seqs = None
        h = None
        i = 1
        while i + 1 < len(t) + 1 and i < len(t):
            if t[i] == 'bones' and i + 1 < len(t):
                bones = int(t[i + 1]); i += 2
            elif t[i] == 'seqs' and i + 1 < len(t):
                seqs = int(t[i + 1]); i += 2
            elif t[i] == 'hash' and i + 1 < len(t):
                h = int(t[i + 1], 16); i += 2
            else:
                i += 1
        if bones is not None and h is not None:
            return (bones, seqs or 0, h)
    return None


def load_cards():
    out = []
    for p in sorted(glob.glob(os.path.join(CARDS, '*', '*.card'))):
        m = read_match(p)
        if m:
            out.append((p, m))
    return out


def matches(card_fp, model_fp):
    """VRCard_Matches, same rule as vr_card.cpp:383 - bones and hash must
    agree; seqs only when the card states one."""
    cb, cs, ch = card_fp
    mb, ms, mh = model_fp
    if cb != mb:
        return False
    if cs and cs != ms:
        return False
    return ch == mh


def main(argv):
    if not argv:
        print(__doc__)
        return 2
    root = os.path.abspath(argv[0])
    dry = '--dry-run' in argv

    # WHERE THE MODELS ARE READ FROM, which is not always where the cards go.
    #
    # We ship our own install and point the engine at a retail Half-Life with
    # -rodir, so the rigs the engine actually loads live in the RETAIL tree -
    # and fingerprinting our install tree instead binds every card to whatever
    # stale content happens to sit there. That is not hypothetical: it bound
    # all twelve mod cards and skipped all eighteen HD ones for weeks, because
    # a set of Half-Life VR Mod rigs had been left in the install's models
    # directory and shadowed retail.
    #
    # So: fingerprint one tree, install into another.
    models_root = root
    for i, a in enumerate(argv):
        if a == '--models-from' and i + 1 < len(argv):
            models_root = os.path.abspath(argv[i + 1])
    if not os.path.isdir(models_root):
        print('not a directory: %s' % models_root)
        return 2

    if not os.path.isdir(root):
        print('not a directory: %s' % root)
        return 2

    cards = load_cards()
    print('%d cards with a fingerprint, under %s' % (len(cards), CARDS))
    print('install root: %s%s' % (root, '   (DRY RUN)' if dry else ''))
    if models_root != root:
        print('models read from: %s' % models_root)
    print()

    installed = skipped = ambiguous = 0
    for gamedir in sorted(os.listdir(root)):
        if not os.path.isdir(os.path.join(root, gamedir)):
            continue

        # THE ENGINE'S OWN PRECEDENCE, not ours to invent. FS_AddGameHierarchy
        # mounts <gamedir>_hd AFTER <gamedir>, and a later search path wins, so
        # with fs_mount_hd set the HD rig is the one loaded. Matching that here
        # is the whole point: a card chosen against a rig the engine will not
        # load is a card that silently never binds.
        models = {}
        for sub in (gamedir, gamedir + '_hd'):
            mdir = os.path.join(models_root, sub, 'models')
            if not os.path.isdir(mdir):
                continue
            for mp in sorted(glob.glob(os.path.join(mdir, 'v_*.mdl'))):
                models[os.path.basename(mp)] = mp
        models = [models[k] for k in sorted(models)]
        if not models:
            continue

        hits, misses = [], []
        for mp in models:
            name = os.path.splitext(os.path.basename(mp))[0]
            try:
                fp = vrcardgen.Model(mp).fingerprint()
            except Exception as exc:
                misses.append((name, 'unreadable: %s' % exc))
                continue
            found = [c for c, cfp in cards if matches(cfp, fp)]
            if not found:
                misses.append((name, 'no card matches %d/%d/%08x' % fp))
                continue
            if len(found) > 1:
                # two cards claiming one rig is an authoring error worth saying
                ambiguous += 1
                misses.append((name, 'AMBIGUOUS: %s' % ', '.join(
                    os.path.relpath(f, CARDS) for f in found)))
                continue
            hits.append((name, found[0]))

        print('%s/  %d models, %d carded' % (gamedir, len(models), len(hits)))
        if hits and not dry:
            dest = os.path.join(root, gamedir, 'vr', 'cards')
            os.makedirs(dest, exist_ok=True)
        for name, src in hits:
            rel = os.path.relpath(src, CARDS)
            if dry:
                print('    would install %-22s <- %s' % (name + '.card', rel))
            else:
                shutil.copyfile(src, os.path.join(root, gamedir, 'vr', 'cards', name + '.card'))
                print('    %-22s <- %s' % (name + '.card', rel))
            installed += 1
        for name, why in misses:
            print('    -- %-20s %s' % (name, why))
            skipped += 1
        print()

    print('installed %d, no card for %d%s' % (
        installed, skipped, ', %d AMBIGUOUS' % ambiguous if ambiguous else ''))
    print()
    print('A weapon with no card behaves exactly as it always has, so the')
    print('skipped ones are safe - they are simply not in VR yet.')
    return 1 if ambiguous else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
