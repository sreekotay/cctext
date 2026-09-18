#!/usr/bin/env python3
"""ccc 0.4.0-415 passes --root as the compiler install prefix.

Local headers then fail: they resolve outside that root and outside the
unit directory. Rewrite --root to this repo (the directory that contains
build.cc) and add it as an include path.
"""
import os
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


def main():
    root = repo_root()
    lower = os.path.expanduser("~/.local/bin/cclower_cc")
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
