#!/usr/bin/env python3
"""ccc 0.4.0-415 passes --root as the compiler install prefix.

Local headers then fail: they resolve outside that root and outside the
unit directory. Rewrite --root to this repo (the directory that contains
build.cc) and add it as an include path.

cclower_cc rewrites every face a unit includes into the shared --h-root
in place (fopen "wb", then write). A host compile of another unit that
opens the same `.h` meanwhile reads it empty or cut short (`unknown type
name 'RtxRx'`, a different face and target each run). The faces are
lowered into a private stage instead and renamed into --h-root.
"""
import filecmp
import os
import shutil
import subprocess
import sys
import tempfile


def repo_root():
    d = os.getcwd()
    while True:
        if os.path.isfile(os.path.join(d, "build.cc")):
            return d
        parent = os.path.dirname(d)
        if parent == d:
            return os.getcwd()
        d = parent


def find_lowerer():
    """cclower_cc from the same install as ccc (Docker: /opt/ccc/bin,
    cc-install.sh: $PREFIX/bin), not a fixed ~/.local path.

    Order: $CCLOWER_CC; next to `ccc` on PATH ($CCC first, resolved through
    symlinks); `cclower_cc` on PATH; ~/.local/bin as a last resort."""
    env = os.environ.get("CCLOWER_CC")
    if env:
        return env
    here = os.path.realpath(__file__)
    for name in (os.environ.get("CCC"), "ccc"):
        if not name:
            continue
        ccc = shutil.which(name)
        if not ccc:
            continue
        for d in (os.path.dirname(ccc), os.path.dirname(os.path.realpath(ccc))):
            cand = os.path.join(d, "cclower_cc")
            if os.access(cand, os.X_OK) and os.path.realpath(cand) != here:
                return cand
    cand = shutil.which("cclower_cc")
    if cand and os.path.realpath(cand) != here:
        return cand
    cand = os.path.expanduser("~/.local/bin/cclower_cc")
    if os.access(cand, os.X_OK):
        return cand
    sys.stderr.write("cclower_root.py: cclower_cc not found next to ccc or on PATH "
                     "(set CCLOWER_CC)\n")
    sys.exit(127)


def place_headers(stage, h_root):
    """Rename each staged file over its --h-root twin; same bytes stay put."""
    for d, _, files in os.walk(stage):
        rel_dir = os.path.relpath(d, stage)
        dst_dir = os.path.normpath(os.path.join(h_root, rel_dir))
        for name in files:
            src = os.path.join(d, name)
            dst = os.path.join(dst_dir, name)
            if os.path.isfile(dst) and filecmp.cmp(src, dst, shallow=False):
                continue
            os.makedirs(dst_dir, exist_ok=True)
            os.replace(src, dst)


def main():
    root = repo_root()
    lower = find_lowerer()
    out = []
    h_root = None
    args = sys.argv[1:]
    i = 0
    while i < len(args):
        if args[i] == "--root" and i + 1 < len(args):
            out.extend(["--root", root])
            i += 2
            continue
        if args[i] == "--h-root" and i + 1 < len(args):
            h_root = args[i + 1]
            i += 2
            continue
        out.append(args[i])
        i += 1
    out.extend(["-I", root])
    stage = None
    if h_root:
        os.makedirs(h_root, exist_ok=True)
        parent = os.path.dirname(os.path.abspath(h_root))
        stage = tempfile.mkdtemp(prefix=".hstage-", dir=parent)
        out.extend(["--h-root", stage])
    try:
        rc = subprocess.call([lower] + out)
        if stage:
            place_headers(stage, h_root)
    finally:
        if stage:
            shutil.rmtree(stage, ignore_errors=True)
    sys.exit(rc)


if __name__ == "__main__":
    main()
