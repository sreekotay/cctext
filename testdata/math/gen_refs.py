#!/usr/bin/env python3
"""Regenerate testdata/math/ref/<name>.png and mml/<name>.mml from
formulas.json: each formula rendered by our own renderer
(bin/cctext-render --math: QuickJS + MathJax 4 + lunasvg; display style,
em 20 px, black, over white, scale 1), and its MathML as MathJax's own TeX
input writes it. tests/math_smoke.ccs compares the sandboxed helper's
output against the PNGs within a tolerance, and renders each .mml through
the MathML input to check that both inputs draw the same picture
(docs/images.md "Math").

Entries whose name starts with "error" or "undefined" are error cases:
no reference. The formulas are our own content.

    python3 testdata/math/gen_refs.py [path/to/cctext-render]
"""
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))


def main(argv):
    exe = argv[1] if len(argv) > 1 else os.path.join(ROOT, 'bin', 'cctext-render')
    ref = os.path.join(HERE, 'ref')
    mml = os.path.join(HERE, 'mml')
    os.makedirs(ref, exist_ok=True)
    os.makedirs(mml, exist_ok=True)
    formulas = json.load(open(os.path.join(HERE, 'formulas.json'), encoding='utf-8'))
    with tempfile.TemporaryDirectory() as tmp:
        for name, tex in formulas:
            if name.startswith(('error', 'undefined')):
                continue
            src = os.path.join(tmp, name + '.tex')
            with open(src, 'w', encoding='utf-8') as f:
                f.write(tex)
            png = os.path.join(ref, name + '.png')
            subprocess.run([exe, '--math', src, png, 'display', 'em=20'], check=True,
                           stderr=subprocess.DEVNULL)
            subprocess.run([exe, '--math', src, os.path.join(mml, name + '.mml'), 'display'],
                           check=True, stderr=subprocess.DEVNULL)
            print(png)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
