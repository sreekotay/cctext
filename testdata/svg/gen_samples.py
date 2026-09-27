#!/usr/bin/env python3
"""Regenerate testdata/svg/samples: our own content only (licence-safe).

  matplotlib-*.svg   matplotlib (svg.fonttype 'none': <text> with DejaVu family
                     names, which cctext-render maps to Noto), seeded data
  mermaid-*.svg      the *.mmd sources beside them, rendered by Mermaid 11 with
                     htmlLabels off (MERMAID_RENDER: a command reading .mmd on
                     stdin and writing SVG on stdout; the renderer prototype was
                     used). Skipped when unset.
  drawio-*.svg       hand-written in draw.io's export shape (<switch> with a
                     <foreignObject> label and a <text> fallback); not generated.

Then the reference PNGs: testdata/svg/ref/<name>.png, rendered by
bin/cctext-render --png at scale 1 (tests/svg_smoke.ccs compares the
helper's output against them within a tolerance).

    python3 testdata/svg/gen_samples.py [--refs-only]
"""
import glob
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
S = os.path.join(HERE, 'samples')


def matplotlib_samples():
    import matplotlib
    matplotlib.use('svg')
    import matplotlib.pyplot as plt
    import numpy as np
    plt.rcParams['svg.fonttype'] = 'none'
    plt.rcParams['svg.hashsalt'] = 'cctext'
    plt.rcParams['svg.id'] = None
    x = np.linspace(0, 6.3, 120)
    fig, ax = plt.subplots(figsize=(4.8, 3.0), dpi=72)
    ax.plot(x, np.sin(x), label='sin x')
    ax.plot(x, np.cos(x), '--', label='cos x')
    ax.fill_between(x, np.sin(x), alpha=.2)
    ax.set_title('Two waves')
    ax.legend()
    ax.grid(True, alpha=.3)
    fig.tight_layout()
    fig.savefig(os.path.join(S, 'matplotlib-lines.svg'), metadata={'Date': None, 'Creator': None})
    plt.close(fig)
    r = np.random.RandomState(7)
    fig, ax = plt.subplots(figsize=(4.0, 3.2), dpi=72)
    ax.scatter(r.randn(60), r.randn(60), c=np.arange(60), cmap='viridis', alpha=.7)
    ax.bar([-1, 0, 1], [1.0, 2.0, 1.5], alpha=.3, hatch='//')
    ax.set_xlabel('x')
    ax.set_ylabel('y')
    fig.tight_layout()
    fig.savefig(os.path.join(S, 'matplotlib-scatter.svg'), metadata={'Date': None, 'Creator': None})
    plt.close(fig)


def mermaid_samples():
    cmd = os.environ.get('MERMAID_RENDER')
    if not cmd:
        print('MERMAID_RENDER unset: keeping the checked-in mermaid-*.svg')
        return
    for src in sorted(glob.glob(os.path.join(S, 'mermaid-*.mmd'))):
        out = subprocess.run(cmd, shell=True, input=open(src, 'rb').read(), capture_output=True, check=True)
        open(src[:-4] + '.svg', 'wb').write(out.stdout)


def refs():
    """Through the protocol, exactly as the editor asks: size, then pixels at
    the natural box (ceil of the CSS size)."""
    import math
    sys.path.insert(0, os.path.join(ROOT, 'bench'))
    from render_client import Helper, save_png
    os.makedirs(os.path.join(HERE, 'ref'), exist_ok=True)
    h = Helper()
    for f in sorted(glob.glob(os.path.join(S, '*.svg'))):
        name = os.path.basename(f)[:-4]
        src = open(f, 'rb').read()
        sz = h.render(src, size_only=True)
        r = h.render(src, box=(math.ceil(sz['w']), math.ceil(sz['h'])))
        if 'error' in r:
            sys.exit('%s: %r' % (name, r['error']))
        save_png(r, os.path.join(HERE, 'ref', name + '.png'))
        print('ref', name, r['pw'], r['ph'])
    h.close()


if __name__ == '__main__':
    if '--refs-only' not in sys.argv:
        matplotlib_samples()
        mermaid_samples()
    refs()
