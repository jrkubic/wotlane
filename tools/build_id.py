#!/usr/bin/env python3
# ABOUTME: wotm.exe build identity for the netcode: hash of what decides the simulation (native/, runtime patches, PS2Recomp
# ABOUTME: commit, recompiled game, build folder). Writes wotm.buildid next to the exe; the server requires a match.
"""Usage: python tools/build_id.py [--runtime <folder>] [--build build_dev] [--write]

Two machines only simulate identically with the same code. The compiler is left out: game code builds with strict FP, so MSVC on
Windows and clang on the Mac produce the same states, and the server's per-frame state hash stops any match where they don't. Paths,
dates and file line endings (CRLF or LF) are left out, so two clones in different folders with the same code give the same id.
Called by tools/build_runtime.ps1 after every build; wotm.exe reads wotm.buildid and sends it to the server (wotm_sync.inc).
"""
import argparse
import hashlib
import json
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent


def text_digest(path):
    """sha256 of the contents with CRLF turned into LF (git may check out either)."""
    return hashlib.sha256(path.read_bytes().replace(b'\r\n', b'\n')).hexdigest()


def tree_digest(root, skip_dirs=()):
    """sha256 of the files in a folder: each one's relative path and contents, in order; folders named in skip_dirs (at any
    level) are left out. Files go in the order of their relative path strings: Path objects sort differently on Windows
    (case-insensitive) and POSIX (by parts), which would give the same files a different id on each OS."""
    h = hashlib.sha256()
    if root.exists():
        for rel, p in sorted((p.relative_to(root).as_posix(), p) for p in root.rglob('*')):
            if not p.is_file() or any(part in skip_dirs for part in rel.split('/')[:-1]):
                continue
            h.update(rel.encode() + b'\0' + text_digest(p).encode() + b'\n')
    return h.hexdigest()


def recomp_digest(retail, runtime):
    """The recompiled game (~90 MB): the hash is cached in <runtime>/.wotm_recomp_hash as long as the folder doesn't change."""
    files = [p for p in retail.rglob('*') if p.is_file()] if retail.exists() else []
    key = [len(files), sum(p.stat().st_size for p in files), max((p.stat().st_mtime_ns for p in files), default=0)]
    cache = runtime / '.wotm_recomp_hash'
    try:
        saved = json.loads(cache.read_text())
        if saved['key'] == key:
            return saved['hash']
    except (OSError, ValueError, KeyError):
        pass
    digest = tree_digest(retail)
    try:
        cache.write_text(json.dumps({'key': key, 'hash': digest}))
    except OSError:
        pass
    return digest


def ps2recomp_head(runtime):
    try:
        return subprocess.run(['git', '-C', str(runtime / 'PS2Recomp'), 'rev-parse', 'HEAD'], capture_output=True, text=True,
                              check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return 'unknown'


def compute(repo, runtime, build, ps2recomp_commit=None):
    parts = {
        'native': tree_digest(repo / 'native', skip_dirs=('build', '__pycache__')),   # build outputs (net, tests)
        'patches': tree_digest(repo / 'tools' / 'runtime_patch'),
        'ps2recomp': ps2recomp_commit or ps2recomp_head(runtime),
        'recomp': recomp_digest(repo / 'recomp' / 'retail', runtime),
        'build': build,   # build_dev (no LTCG) and build (LTCG) generate different code
    }
    return 'wl-' + hashlib.sha256(json.dumps(parts, sort_keys=True).encode()).hexdigest()[:16]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--runtime', default=None)
    ap.add_argument('--build', default='build_dev')
    ap.add_argument('--write', action='store_true', help='write <runtime>/<build>/ps2xRuntime/wotm.buildid')
    a = ap.parse_args()
    saved = REPO / '.wotm_recomp_dir'
    runtime = Path(a.runtime) if a.runtime else (Path(saved.read_text().splitlines()[0].strip()) if saved.exists() else REPO / 'runtime')
    bid = compute(REPO, runtime, a.build)
    if a.write:
        out = runtime / a.build / 'ps2xRuntime' / 'wotm.buildid'
        out.write_text(bid + '\n')
    print(bid)
    return 0


if __name__ == '__main__':
    sys.exit(main())
