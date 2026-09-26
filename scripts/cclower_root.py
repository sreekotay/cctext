#!/usr/bin/env python3
"""ccc 0.4.0-415 passes --root as the compiler install prefix.

Local headers then fail: they resolve outside that root and outside the
unit directory. Rewrite --root to this repo (the directory that contains
build.cc) and add it as an include path.
"""
import os
import shutil
import subprocess
import sys


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


def main():
    root = repo_root()
    lower = find_lowerer()
    out = []
    args = sys.argv[1:]
    i = 0
    while i < len(args):
        if args[i] == "--root" and i + 1 < len(args):
            out.extend(["--root", root])
            i += 2
            continue
        out.append(args[i])
        i += 1
    out.extend(["-I", root])
    rc = subprocess.call([lower] + out)
    sys.exit(rc)


if __name__ == "__main__":
    main()
