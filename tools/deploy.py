"""Put a built engine, the built game DLLs and the matching cards into a play install.

    python tools/deploy.py <install-root> [--arch 32|64] [--dry-run]

This exists because a first run was nearly made against binaries five days old.
Three separate things have to arrive together for the VR weapon path to do
anything at all, and each one being absent fails silently and differently:

  xash.dll              the engine. Without today's build, single player never
                        negotiates the hand channel and the server never steps
                        a weapon.
  valve/dlls/hl.dll     the server game DLL. Valve's own is what ships; if it
                        is still in place, none of our weapon code exists.
  valve/cl_dlls/
    client.dll          the client game DLL, which is where prediction lives.
  <gamedir>/vr/cards/   the cards. Without them every weapon takes the vanilla
                        path by design, so the whole simulator is inert and
                        nothing looks wrong.

ARCHITECTURE MATTERS AND IS EASY TO GET WRONG. A 64-bit engine can only load
64-bit game DLLs, and Half-Life's own hl.dll is 32-bit - so the 64-bit install
cannot play Half-Life at all and exists to prove the engine, OpenXR and
rendering come up. Play is 32-bit. This script defaults to 32 for that reason
and will not mix the two.

Everything it overwrites is backed up beside itself first, once per run.
"""
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE_REPO = os.path.dirname(HERE)
HLSDK = os.path.join(os.path.dirname(ENGINE_REPO), 'hlsdk-portable')

# EVERY module the engine build produces, not just xash.dll.
#
# This list used to hold only xash.dll and the two game DLLs, and that cost a
# play session: a brand-new engine was deployed on top of a renderer five days
# old and a filesystem module five WEEKS old, and the result was 7,508
# GL_INVALID_OPERATION errors in one run. These pieces share interface structs
# and are versioned together by being built together; deploying one without the
# others is an ABI mismatch waiting to be blamed on whatever changed last.
#
# game_launch/xash3d.exe is included for the same reason - it is the host
# process and loads xash.dll.
def _engine(build):
    return [
        (os.path.join(ENGINE_REPO, build, 'engine', 'xash.dll'), 'xash.dll'),
        (os.path.join(ENGINE_REPO, build, 'ref', 'gl', 'ref_gl.dll'), 'ref_gl.dll'),
        (os.path.join(ENGINE_REPO, build, 'filesystem', 'filesystem_stdio.dll'), 'filesystem_stdio.dll'),
        (os.path.join(ENGINE_REPO, build, '3rdparty', 'mainui', 'menu.dll'), 'menu.dll'),
        (os.path.join(ENGINE_REPO, build, '3rdparty', 'vgui_support', 'vgui_support.dll'), 'vgui_support.dll'),
        (os.path.join(ENGINE_REPO, build, 'game_launch', 'xash3d.exe'), 'xash3d.exe'),
    ]


def _game(build):
    return [
        (os.path.join(HLSDK, build, 'dlls', 'Release', 'hl.dll'), os.path.join('valve', 'dlls', 'hl.dll')),
        (os.path.join(HLSDK, build, 'cl_dll', 'Release', 'client.dll'), os.path.join('valve', 'cl_dlls', 'client.dll')),
    ]


PIECES = {
    '32': _engine('build') + _game('build'),
    '64': _engine('build64') + _game('build64'),
}


def mtime(p):
    try:
        import time
        return time.strftime('%Y-%m-%d %H:%M', time.localtime(os.path.getmtime(p)))
    except OSError:
        return 'absent'


def main(argv):
    if not argv or argv[0].startswith('-'):
        print(__doc__)
        return 2

    root = os.path.abspath(argv[0])
    arch = '32'
    if '--arch' in argv:
        arch = argv[argv.index('--arch') + 1]
    dry = '--dry-run' in argv

    if arch not in PIECES:
        print('--arch must be 32 or 64')
        return 2
    if not os.path.isdir(root):
        print('not a directory: %s' % root)
        return 2

    print('deploy %s-bit -> %s%s' % (arch, root, '   (DRY RUN)' if dry else ''))
    print()

    missing = [src for src, _ in PIECES[arch] if not os.path.isfile(src)]
    if missing:
        print('NOT BUILT - build these first, then re-run:')
        for m in missing:
            print('   %s' % m)
        print()
        if arch == '32':
            print('   engine:    cd %s && python waf build' % ENGINE_REPO)
        else:
            print('   engine:    cd %s && WAFLOCK=.lock-waf_win32_build64 python waf build' % ENGINE_REPO)
        print('   game DLLs: cmake --build %s --config Release'
              % os.path.join(HLSDK, 'build' if arch == '32' else 'build64'))
        return 1

    for src, rel in PIECES[arch]:
        dst = os.path.join(root, rel)
        print('%-28s %s  ->  %s' % (os.path.basename(rel), mtime(src), mtime(dst)))
        if dry:
            continue
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        if os.path.isfile(dst):
            shutil.copyfile(dst, dst + '.bak-predeploy')
        shutil.copyfile(src, dst)

    print()
    print('cards:')
    cmd = [sys.executable, os.path.join(HERE, 'vrcard', 'install_cards.py'), root]
    if dry:
        cmd.append('--dry-run')
    r = subprocess.run(cmd, capture_output=True, text=True)
    for line in r.stdout.splitlines():
        if line.startswith('installed') or '/  ' in line or 'AMBIGUOUS' in line:
            print('   ' + line)

    print()
    if dry:
        print('dry run; nothing written.')
    else:
        print('deployed. Overwritten files kept as *.bak-predeploy.')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
