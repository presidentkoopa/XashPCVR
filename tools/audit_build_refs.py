"""Check that every source file a build file names is actually in the repo.

This exists because the same bug shipped twice in this project. Both times a
module's .cpp was referenced by a committed build file while the file itself was
never added, so a fresh clone could not build at all - and both times the build
was verified in a working tree that still had the untracked file sitting in it,
which proves nothing about the tree anybody else gets.

    python tools/audit_build_refs.py <repo> [<repo> ...]

Exit code 1 if any referenced source is on disk but absent from the repo. Give
it both halves of the project to check them together:

    python tools/audit_build_refs.py . ../hlsdk-portable

Submodule trees are skipped - their files belong to their own repos and are
correctly absent from the parent. Files referenced but found nowhere are
reported separately and are usually benign: waf probes for system headers
(stdint.h, alloca.h) and optional dependencies (openxr.h, avcodec.h).
"""
import os
import re
import subprocess
import sys

BACKSLASH = chr(92)
SRC = re.compile(r'[A-Za-z0-9_./' + BACKSLASH + BACKSLASH + r'-]+\.(?:c|cpp|h|hpp|cc)\b')
BUILD_SUFFIX = ('CMakeLists.txt', '.cmake', '.bat', '.vcxproj', '.mak', 'wscript', '.py', '.sh')

# waifulib is waf's own toolchain support; it names compiler test fragments and
# cross-build sources that are not ours and never in this tree.
SKIP_PREFIX = ('3rdparty/', 'scripts/waifulib/')


def git(repo, *args):
    r = subprocess.run(['git', '-C', repo, *args], capture_output=True, text=True)
    return r.stdout.splitlines()


def submodule_dirs(repo):
    out = git(repo, 'config', '--file', '.gitmodules', '--get-regexp', 'path')
    dirs = set()
    for line in out:
        parts = line.split(None, 1)
        if len(parts) == 2:
            # keep the top component: 3rdparty/vorbis/vorbis-src -> 3rdparty/vorbis
            dirs.add(parts[1].strip().split('/')[0])
    return dirs


def audit(repo):
    repo = os.path.abspath(repo)
    print('=' * 24, repo)
    tracked = git(repo, 'ls-files')
    if not tracked:
        print('  not a git repo, or empty')
        return 0
    tracked_base = {os.path.basename(t) for t in tracked}
    skip_dirs = submodule_dirs(repo) | {'3rdparty'}

    ondisk = set()
    for root, dirs, files in os.walk(repo):
        dirs[:] = [d for d in dirs if d != '.git' and d not in skip_dirs]
        for f in files:
            ondisk.add(f)

    buildfiles = [t for t in tracked
                  if (t.endswith(BUILD_SUFFIX) or os.path.basename(t).startswith('Makefile'))
                  and not t.startswith(SKIP_PREFIX)]

    untracked_hits, missing_hits = {}, {}
    nrefs = 0
    for bf in buildfiles:
        try:
            txt = open(os.path.join(repo, bf), 'r', errors='ignore').read()
        except OSError:
            continue
        for m in SRC.finditer(txt):
            nrefs += 1
            name = os.path.basename(m.group(0).replace(BACKSLASH, '/'))
            if name in tracked_base:
                continue
            bucket = untracked_hits if name in ondisk else missing_hits
            bucket.setdefault(name, set()).add(bf)

    print('%d build files, %d source references' % (len(buildfiles), nrefs))

    if untracked_hits:
        print()
        print('  !! ON DISK BUT NOT IN THE REPO -- a fresh clone cannot build:')
        for name, bfs in sorted(untracked_hits.items()):
            print('     %s' % name)
            for bf in sorted(bfs):
                print('         referenced by %s' % bf)
    else:
        print('  ok: every referenced source is in the repo')

    if missing_hits:
        print()
        print('  -- referenced but nowhere on disk (system headers, optional deps, generated):')
        for name, bfs in sorted(missing_hits.items()):
            print('     %-26s <- %s' % (name, ', '.join(sorted(bfs))))

    print()
    return len(untracked_hits)


if __name__ == '__main__':
    targets = sys.argv[1:] or ['.']
    bad = sum(audit(r) for r in targets)
    if bad:
        print('FAIL: %d referenced source file(s) missing from a repo' % bad)
    else:
        print('PASS')
    sys.exit(1 if bad else 0)
