#!/usr/bin/env python3
"""Regenerate testdata/mermaid/ref/<type>.png: each *.mmd here rendered by
our own renderer (bin/cctext-render --mermaid: QuickJS + mermaid.min.js +
lunasvg, mermaid's default theme, over white, scale 1). tests/mermaid_smoke
compares the sandboxed helper's output against these within a tolerance,
and against the Chromium renders in chromium/ by structure (size and a
coarse ink map; see docs/images.md "Mermaid").

The sources are our own content. chromium/<type>.nohtml.svg are the same
sources rendered by mermaid 12.0.0 in headless Chromium with htmlLabels
off (the renderer evaluation's references; not regenerated here: that
needs a browser).

    python3 testdata/mermaid/gen_refs.py [path/to/cctext-render]
"""
import glob
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))


def main(argv):
    exe = argv[1] if len(argv) > 1 else os.path.join(ROOT, 'bin', 'cctext-render')
    out = os.path.join(HERE, 'ref')
    os.makedirs(out, exist_ok=True)
    env = dict(os.environ, TZ='UTC')  # the sandboxed helper cannot read a zone: UTC
    for src in sorted(glob.glob(os.path.join(HERE, '*.mmd'))):
        name = os.path.splitext(os.path.basename(src))[0]
        png = os.path.join(out, name + '.png')
        subprocess.run([exe, '--mermaid', src, png], check=True, env=env)
        print(png)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
