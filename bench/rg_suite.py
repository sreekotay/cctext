#!/usr/bin/env python3
"""ripgrep's benchsuite Linux-tree queries (and the identifiers of the
search-index simulation): rg vs cctext project search, without and with
the search index.

The patterns and rg flags are ripgrep's `benchsuite/benchsuite`
(BurntSushi/ripgrep) `linux_*` benchmarks. rg runs with `--hidden`
(cctext lists dot files); both honor .gitignore (the tree is a git
checkout). cctext runs `cctext --batch --safe --project DIR -c
'project-search ...'` with RTX_SAFE_HOME pointing at a scratch dir; the
index is built once with `project-index`.

Counts: rg prints matching lines; cctext prints one row per match, so
its count is the distinct (path, line) pairs. `\\p{Greek}` is not in
cctext's regex dialect: those two rows run the Greek and Coptic plus
Greek Extended blocks as a class, for both tools. The index column
must equal the no-index column (INDEX MISMATCH otherwise).

Per query, with the index: bytes read as a % of the indexed bytes, files
read (opened), files read by chunks, and the warm time split: the walk
(wall), the search (wall), and the lanes' summed stat vs read + scan.

  bench/rg_suite.py --linux ~/linux --cctext bin/cctext [--runs 5] [--safe DIR] [--pct N]
                    [--only NAME] [--no-rg] [--build] [--update]

--build: index build time and peak RSS with one lane and with all lanes.
--update: `project-index --update` after appending a line to 1 and to
100 files (restored afterwards), against a full build.

Cold: the page cache is dropped first (root only; otherwise the column
says "n/a" and every run is warm). Warm: best of --runs.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile
import time

GREEK = r'[\x{0370}-\x{03FF}\x{1F00}-\x{1FFF}]'
GREEK_RG = r'[\x{0370}-\x{03FF}\x{1F00}-\x{1FFF}]'

# name, pattern, regex?, icase?, word?, rg extra flags
QUERIES = [
    ('linux_literal_default', 'PM_RESUME', False, False, False, []),
    ('linux_literal', 'PM_RESUME', False, False, False, ['-n']),
    ('linux_literal_casei', 'PM_RESUME', False, True, False, ['-n', '-i']),
    ('linux_re_literal_suffix', '[A-Z]+_RESUME', True, False, False, ['-n']),
    ('linux_word', 'PM_RESUME', False, False, True, ['-n', '-w']),
    ('linux_unicode_greek*', GREEK, True, False, False, ['-n']),
    ('linux_unicode_greek_casei*', GREEK, True, True, False, ['-n', '-i']),
    ('linux_unicode_word', r'\wAh', True, False, False, ['-n']),
    ('linux_alternates', 'ERR_SYS|PME_TURN_OFF|LINK_REQ_RST|CFG_BME_EVT', True, False, False,
     ['-n']),
    ('linux_alternates_casei', 'ERR_SYS|PME_TURN_OFF|LINK_REQ_RST|CFG_BME_EVT', True, True,
     False, ['-n', '-i']),
    ('linux_no_literal', r'\w{5}\s+\w{5}\s+\w{5}\s+\w{5}\s+\w{5}', True, False, False, ['-n']),
]

# The simulation's identifiers and phrases (/tmp/sim/work/queries.py).
SIM = ['pthread_mutex_timedlock', 'crc32c_le', 'regmap_update_bits_base', 'iommu_map_sg',
       'kfree_skb_list', 'hrtimer_forward_now', 'SLAB_HWCACHE_ALIGN', 'kmalloc_array',
       'copy_from_user', 'spin_lock_irqsave', 'should never happen', 'for the time being',
       'flibbertigibbet']
QUERIES += [('sim ' + s, s, False, False, False, ['-n', '-F']) for s in SIM]


def drop_caches():
    try:
        subprocess.run(['sync'], check=False)
        with open('/proc/sys/vm/drop_caches', 'w') as f:
            f.write('3\n')
        return True
    except OSError:
        return False


def timed(cmd, env=None, cwd=None):
    """(seconds, stdout, 0)."""
    t0 = time.perf_counter()
    p = subprocess.run(cmd, env=env, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    return time.perf_counter() - t0, p.stdout, 0


def timed_rss(cmd, env=None):
    """Wall seconds, stdout and peak RSS (KiB) via /usr/bin/time -v when present."""
    if os.path.exists('/usr/bin/time'):
        t0 = time.perf_counter()
        p = subprocess.run(['/usr/bin/time', '-v'] + cmd, env=env, stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE)
        t = time.perf_counter() - t0
        m = re.search(rb'Maximum resident set size \(kbytes\): (\d+)', p.stderr)
        return t, p.stdout, int(m.group(1)) if m else 0
    # no GNU time: a python parent reports its only child's peak
    wrap = ('import resource, subprocess, sys; r = subprocess.run(sys.argv[1:]); '
            'sys.stderr.write("maxrss %d\\n" % resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss)')
    t0 = time.perf_counter()
    p = subprocess.run([sys.executable, '-c', wrap] + cmd, env=env, stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE)
    t = time.perf_counter() - t0
    m = re.search(rb'maxrss (\d+)', p.stderr)
    return t, p.stdout, int(m.group(1)) if m else 0


def rg_cmd(q, tree):
    name, pat, regex, icase, word, flags = q
    args = ['rg', '--hidden', '--no-heading', '--with-filename'] + flags
    if '-n' not in args:
        args.append('-n')  # counts need line numbers; the default run adds them too
    return args + ['-e', pat.replace(GREEK, GREEK_RG), tree]


MAX_SIZE = 1000000000


def cc_cmd(q, tree, cctext, stats=False):
    name, pat, regex, icase, word, flags = q
    opts = []
    if regex:
        opts.append('--regex')
    if icase:
        opts.append('--icase')
    if word:
        opts.append('--word')
    if stats:
        opts.append('--stats')
    line = 'project-search --max-size %d %s -- \'%s\'' % (MAX_SIZE, ' '.join(opts), pat)
    return [cctext, '--batch', '--safe', '--project', tree, '-c', line]


def rg_count(out, tree):
    seen = set()
    for ln in out.decode('utf-8', 'replace').splitlines():
        parts = ln.split(':', 2)
        if len(parts) >= 2 and parts[1].isdigit():
            path = os.path.relpath(parts[0], tree)
            seen.add((path, parts[1]))
    return seen


def cc_count(out):
    seen = set()
    summary = []
    for ln in out.decode('utf-8', 'replace').splitlines():
        if ln.startswith('#'):
            summary.append(ln)
            continue
        parts = ln.split(':', 3)
        if len(parts) >= 3 and parts[1].isdigit():
            seen.add((parts[0], parts[1]))
    return seen, summary


def cc_rows(out):
    """Every match row (path:line:col: preview) — the exact comparison."""
    return [ln for ln in out.decode('utf-8', 'replace').splitlines() if not ln.startswith('#')]


def parse_stats(summ):
    """bytes, files covered, skipped, chunked, walk ms, search ms, stat, read, scan ms."""
    r = {}
    for s in summ:
        m = re.search(r'; (\d+) files, (\d+) bytes in (\d+) ms', s)
        if m:
            r['files'], r['bytes'] = int(m.group(1)), int(m.group(2))
            m2 = re.search(r'\((\d+) binary', s)
            r['bin'] = int(m2.group(1)) if m2 else 0
            m2 = re.search(r'; (\d+) skipped by the index', s)
            r['skip'] = int(m2.group(1)) if m2 else 0
            m2 = re.search(r'; (\d+) read by chunks', s)
            r['chunk'] = int(m2.group(1)) if m2 else 0
        m = re.search(r'# time: walk (\d+) ms, search (\d+) ms; lanes \(summed\) stat ([\d.]+) ms,'
                      r' read ([\d.]+) ms, scan ([\d.]+) ms', s)
        if m:
            r['walk'], r['search'] = int(m.group(1)), int(m.group(2))
            r['stat'], r['read'], r['scan'] = map(float, m.groups()[2:])
    return r


def build_bench(tree, cctext, safe, pct):
    env = dict(os.environ, RTX_SAFE_HOME=safe)
    extra = ' --pct %d' % pct if pct else ''
    for lanes in (1, 0):
        best = None
        for _ in range(3):
            t, out, rss = timed_rss([cctext, '--batch', '--safe', '--project', tree, '-c',
                                     'project-index' + extra +
                                     (' --lanes %d' % lanes if lanes else '')], env=env)
            if best is None or t < best[0]:
                best = (t, out, rss)
        t, out, rss = best
        b = [ln for ln in out.decode().splitlines() if ln.startswith('# build')]
        print('# build, %s: %.2f s wall (process, walk included), peak RSS %.0f MiB; %s' % (
            'one lane' if lanes == 1 else 'all lanes', t, rss / 1024.0, b[0][2:] if b else ''))


def update_bench(tree, cctext, safe, pct):
    env = dict(os.environ, RTX_SAFE_HOME=safe)
    extra = ' --pct %d' % pct if pct else ''
    files = []
    for d, _, fs in os.walk(os.path.join(tree, 'drivers')):
        for f in sorted(fs):
            if f.endswith('.c'):
                files.append(os.path.join(d, f))
        if len(files) > 400:
            break
    files = files[::4][:100]
    subprocess.run([cctext, '--batch', '--safe', '--project', tree, '-c', 'project-index' + extra],
                   env=env, stdout=subprocess.PIPE)
    time.sleep(0.3)
    for n in (1, 100):
        saved = []
        for f in files[:n]:
            with open(f, 'rb') as fh:
                saved.append((f, fh.read(), os.stat(f)))
            with open(f, 'ab') as fh:
                fh.write(b'/* rg_suite update probe qjxprobe */\n')
        time.sleep(0.3)
        t, out, _ = timed([cctext, '--batch', '--safe', '--project', tree, '-c',
                           'project-index --update' + extra], env=env)
        u = [ln for ln in out.decode().splitlines() if ln.startswith('# update') or
             ln.startswith('# build')]
        print('# update, %d changed file%s: %.2f s wall (process, walk included); %s' % (
            n, '' if n == 1 else 's', t, u[0][2:] if u else out.decode()))
        t2, out2, _ = timed(cc_cmd(('probe', 'qjxprobe', False, False, False, []), tree, cctext),
                            env=env)
        got = len([ln for ln in out2.decode().splitlines() if not ln.startswith('#')])
        print('#   probe search after the update: %d of %d matches' % (got, n))
        for f, data, st in saved:
            with open(f, 'wb') as fh:
                fh.write(data)
            os.utime(f, ns=(st.st_atime_ns, st.st_mtime_ns))
        time.sleep(0.3)
        subprocess.run([cctext, '--batch', '--safe', '--project', tree, '-c',
                        'project-index --update' + extra], env=env, stdout=subprocess.PIPE)


def main():
    global MAX_SIZE
    ap = argparse.ArgumentParser()
    ap.add_argument('--linux', required=True)
    ap.add_argument('--cctext', required=True)
    ap.add_argument('--runs', type=int, default=5)
    ap.add_argument('--safe', default=None)
    ap.add_argument('--only', default=None)
    ap.add_argument('--no-rg', action='store_true')
    ap.add_argument('--build', action='store_true')
    ap.add_argument('--update', action='store_true')
    ap.add_argument('--pct', type=int, default=0, help='index budget (project-index --pct)')
    ap.add_argument('--max-size', type=int, default=MAX_SIZE,
                    help='cctext search size cap (8388608 = the default; the simulation\'s corpus)')
    a = ap.parse_args()
    MAX_SIZE = a.max_size
    tree = os.path.abspath(a.linux)
    cctext = os.path.abspath(a.cctext)
    safe = a.safe or tempfile.mkdtemp(prefix='rg_suite_safe_')
    env_off = dict(os.environ, RTX_SAFE_HOME=safe, RTX_SEARCH_INDEX='0')
    env_on = dict(os.environ, RTX_SAFE_HOME=safe)
    if a.build:
        build_bench(tree, cctext, safe, a.pct)
    if a.update:
        update_bench(tree, cctext, safe, a.pct)
    can_drop = drop_caches()
    t, out, _ = timed([cctext, '--batch', '--safe', '--project', tree, '-c',
                       'project-index' + (' --pct %d' % a.pct if a.pct else '')], env=env_on)
    print('# index build: %.2f s' % t)
    corpus = 0
    for ln in out.decode().splitlines():
        print(ln)
        m = re.search(r'files, (\d+) bytes indexed', ln)
        if m:
            corpus = int(m.group(1))
    print()
    print('| query | rg lines | cctext lines | rg warm | no index warm | index warm '
          '| bytes read | files read (chunked) | walk / search ms | lanes stat / read+scan ms |'
          ' no index: lanes stat / read+scan ms |')
    print('|---|---|---|---|---|---|---|---|---|---|---|')
    tot = {'on': 0, 'off': 0, 'rg': 0}
    fracs = []
    for q in QUERIES:
        if a.only and a.only not in q[0]:
            continue
        rows = {}
        tags = [('off', cc_cmd(q, tree, cctext, stats=True), env_off),
                ('on', cc_cmd(q, tree, cctext, stats=True), env_on)]
        if not a.no_rg:
            tags.insert(0, ('rg', rg_cmd(q, tree), None))
        for _ in range(a.runs):
            # interleaved: a noisy machine hits each column alike
            for tag, cmd, env in tags:
                t, out, _ = timed(cmd, env=env)
                old = rows.get(tag)
                if old is None or t < old[0]:
                    rows[tag] = (t, out)
        ccs, summ = cc_count(rows['off'][1])
        ons, summ_on = cc_count(rows['on'][1])
        rgs = rg_count(rows['rg'][1], tree) if 'rg' in rows else set()
        note = ''
        if cc_rows(rows['on'][1]) != cc_rows(rows['off'][1]):
            note = ' INDEX MISMATCH (%d vs %d)' % (len(ons), len(ccs))
        so, sf = parse_stats(summ_on), parse_stats(summ)
        frac = 100.0 * so.get('bytes', 0) / corpus if corpus else 0
        fracs.append(frac)
        read = so.get('files', 0) - so.get('skip', 0)
        for k in tot:
            if k in rows:
                tot[k] += rows[k][0]

        def ms(x):
            return '%.0f ms' % (x * 1000)

        print('| %s | %s | %d%s | %s | %s | %s | %.1f %% | %d (%d) | %d / %d | %.0f / %.0f |'
              ' %.0f / %.0f |' % (
                  q[0], len(rgs) if 'rg' in rows else '-', len(ccs), note,
                  ms(rows['rg'][0]) if 'rg' in rows else '-', ms(rows['off'][0]),
                  ms(rows['on'][0]), frac, read, so.get('chunk', 0), so.get('walk', 0),
                  so.get('search', 0), so.get('stat', 0), so.get('read', 0) + so.get('scan', 0),
                  sf.get('stat', 0), sf.get('read', 0) + sf.get('scan', 0)))
        if 'rg' in rows and rgs != ccs and os.environ.get('RG_SUITE_DIFF'):
            for x in sorted(rgs - ccs)[:10]:
                print('   rg only: %s:%s' % x, file=sys.stderr)
            for x in sorted(ccs - rgs)[:10]:
                print('   cctext only: %s:%s' % x, file=sys.stderr)
        sys.stdout.flush()
    if fracs:
        fs = sorted(fracs)
        med = fs[len(fs) // 2] if len(fs) % 2 else (fs[len(fs) // 2 - 1] + fs[len(fs) // 2]) / 2
        print()
        print('# bytes read with the index: median %.1f %%, mean %.1f %% of the indexed bytes;'
              ' warm totals: index %.2f s, no index %.2f s%s' % (
                  med, sum(fs) / len(fs), tot['on'], tot['off'],
                  ', rg %.2f s' % tot['rg'] if not a.no_rg else ''))


if __name__ == '__main__':
    main()
