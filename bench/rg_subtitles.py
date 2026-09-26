#!/usr/bin/env python3
"""ripgrep benchsuite's OpenSubtitles single-file queries (subtitles_en_* /
subtitles_ru_*: literal, literal_casei, literal_word, alternate,
alternate_casei, surrounding_words, no_literal) through cctext's regex
engine and in-document find, against rg on the same file. Match counts
must agree; the table gives MB/s.

    ./make.shcc @smoke   (or build rx_perf + find_perf)
    python3 bench/rg_subtitles.py [--bin bin] [--dir /tmp/rg_subs] [--mib 256]
                                  [--find-mib 32] [--trials 5] [--only en_lit]
                                  [--keep]

  rx     bin/rx_perf FILE PATTERN FLAGS: the engine alone, one thread, the
         whole file as one text, every match (no cap). rg searches a single
         file on one thread too.
  find   bin/find_perf FILE --query Q OPTS: the editor's Ctrl-F path (open
         not timed; 2 MiB blocks on the dest-live lanes). Its list stops at
         RTX_FIND_MAX (32768) hits, so it runs on a --find-mib prefix.
  rg     rg --count-matches, wall time of the process (file in page cache).

Corpus: benchsuite's URLs (object.pouta.csc.fi), streamed and cut at whole
lines; when that host is unreachable, rebar's OpenSubtitles samples
(BurntSushi/rebar benchmarks/haystacks/opensubtitles/{en,ru}-sampled.txt,
random lines of the same corpus) repeated to size — the same text, but
with far more hits per MiB than the full corpus. The corpora are deleted
afterwards unless --keep.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import time
import urllib.request
import zlib

URLS = {
    'en': 'https://object.pouta.csc.fi/OPUS-OpenSubtitles/v2016/mono/en.txt.gz',
    'ru': 'https://object.pouta.csc.fi/OPUS-OpenSubtitles/v2016/mono/ru.txt.gz',
}
REBAR = 'https://github.com/BurntSushi/rebar'
REBAR_DIR = 'benchmarks/haystacks/opensubtitles'

EN_ALT = 'Sherlock Holmes|John Watson|Irene Adler|Inspector Lestrade|Professor Moriarty'
RU_ALT = ('Шерлок Холмс|Джон Уотсон|Ирен Адлер|инспектор Лестрейд|'
          'профессор Мориарти')
NOLIT = r'\w{5}\s+\w{5}\s+\w{5}\s+\w{5}\s+\w{5}\s+\w{5}\s+\w{5}'

# name, lang, pattern, rg flags, find opts, rx_perf pattern (None = same), rx flags
QUERIES = [
    ('en_literal', 'en', 'Sherlock Holmes', [], '', None, 'l'),
    ('en_literal_casei', 'en', 'Sherlock Holmes', ['-i'], 'i', None, 'il'),
    ('en_literal_word', 'en', 'Sherlock Holmes', ['-w'], 'w', r'\bSherlock Holmes\b', ''),
    ('en_alternate', 'en', EN_ALT, [], 'r', None, ''),
    ('en_alternate_casei', 'en', EN_ALT, ['-i'], 'ri', None, 'i'),
    ('en_surrounding_words', 'en', r'\w+\s+Holmes\s+\w+', [], 'r', None, ''),
    ('en_no_literal', 'en', NOLIT, [], 'r', None, ''),
    ('ru_literal', 'ru', 'Шерлок Холмс', [], '', None, 'l'),
    ('ru_literal_casei', 'ru', 'Шерлок Холмс', ['-i'], 'i', None, 'il'),
    ('ru_literal_word', 'ru', 'Шерлок Холмс', ['-w'], 'w', r'\bШерлок Холмс\b', ''),
    ('ru_alternate', 'ru', RU_ALT, [], 'r', None, ''),
    ('ru_alternate_casei', 'ru', RU_ALT, ['-i'], 'ri', None, 'i'),
    ('ru_surrounding_words', 'ru', r'\w+\s+Холмс\s+\w+', [], 'r', None, ''),
    ('ru_no_literal', 'ru', NOLIT, [], 'r', None, ''),
]


def log(*a):
    print(*a, file=sys.stderr, flush=True)


def fetch_stream(lang, dst, want):
    """Stream the gzip and keep the first `want` bytes (whole lines)."""
    d = zlib.decompressobj(16 + zlib.MAX_WBITS)
    got = bytearray()
    with urllib.request.urlopen(URLS[lang], timeout=60) as r:
        while len(got) < want:
            chunk = r.read(1 << 20)
            if not chunk:
                break
            got += d.decompress(chunk)
    got = bytes(got[:want])
    got = got[:got.rfind(b'\n') + 1]
    with open(dst, 'wb') as f:
        f.write(got)
    return 'OpenSubtitles v2016 %s.txt.gz, first %d bytes' % (lang, len(got))


def fetch_rebar(work):
    root = os.path.join(work, 'rebar')
    if not os.path.isdir(os.path.join(root, REBAR_DIR)):
        subprocess.run(['git', 'clone', '-q', '--depth', '1', '--filter=blob:none', '--sparse',
                        REBAR, root], check=True)
        subprocess.run(['git', '-C', root, 'sparse-checkout', 'set', REBAR_DIR], check=True)
    return os.path.join(root, REBAR_DIR)


def repeat_to(src, dst, want):
    b = open(src, 'rb').read()
    n = 0
    with open(dst, 'wb') as f:
        while n + len(b) <= want:
            f.write(b)
            n += len(b)
    return n


def corpus(work, lang, mib, find_mib):
    big = os.path.join(work, '%s.txt' % lang)
    small = os.path.join(work, '%s.find.txt' % lang)
    try:
        src = fetch_stream(lang, big, mib << 20)
    except Exception as e:  # blocked host, no network
        log('%s: %s unreachable (%s); using rebar samples' % (lang, URLS[lang], e))
        d = fetch_rebar(work)
        s = os.path.join(d, '%s-sampled.txt' % lang)
        repeat_to(s, big, mib << 20)
        src = 'rebar %s-sampled.txt (%d bytes) repeated' % (lang, os.path.getsize(s))
    # find prefix: whole lines
    with open(big, 'rb') as f:
        b = f.read(find_mib << 20)
    with open(small, 'wb') as f:
        f.write(b[:b.rfind(b'\n') + 1])
    return big, small, src


def rg_count(path, pat, flags, trials):
    best = None
    count = 0
    for _ in range(trials):
        t0 = time.perf_counter()
        r = subprocess.run(['rg', '--count-matches'] + flags + ['--', pat, path],
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        t = time.perf_counter() - t0
        if r.returncode not in (0, 1):
            raise RuntimeError('rg: %s' % r.stderr.decode())
        count = int(r.stdout.strip() or 0)
        best = t if best is None or t < best else best
    return count, best * 1000.0


RESULT = re.compile(r'ms=([0-9.]+) MBps=([0-9]+) hits=([0-9]+)(\s+capped)?')


def cc_run(argv, trials):
    env = dict(os.environ, RTX_PERF_TRIALS=str(trials))
    r = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
    m = RESULT.search(r.stdout.decode())
    if r.returncode or not m:
        raise RuntimeError('%s: %s%s' % (argv[0], r.stdout.decode(), r.stderr.decode()))
    return int(m.group(3)), float(m.group(1)), bool(m.group(4))


def mbps(nbytes, ms):
    return nbytes / 1048576.0 / (ms / 1000.0) if ms > 0 else 0.0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--bin', default='bin')
    ap.add_argument('--dir', default='/tmp/rg_subs')
    ap.add_argument('--mib', type=int, default=256)
    ap.add_argument('--find-mib', type=int, default=32)
    ap.add_argument('--trials', type=int, default=5)
    ap.add_argument('--only', default='')
    ap.add_argument('--keep', action='store_true')
    a = ap.parse_args()
    rx_perf = os.path.join(a.bin, 'rx_perf')
    find_perf = os.path.join(a.bin, 'find_perf')
    for b in (rx_perf, find_perf):
        if not os.access(b, os.X_OK):
            log('missing %s (build rx_perf / find_perf first)' % b)
            return 2
    if not shutil.which('rg'):
        log('rg not on PATH')
        return 2
    os.makedirs(a.dir, exist_ok=True)
    files = {}
    bad = 0
    try:
        rows = []
        for name, lang, pat, rgf, fopts, rxpat, rxfl in QUERIES:
            if a.only and a.only not in name:
                continue
            if lang not in files:
                files[lang] = corpus(a.dir, lang, a.mib, a.find_mib)
                log('%s corpus: %s' % (lang, files[lang][2]))
            big, small, _ = files[lang]
            nb, ns = os.path.getsize(big), os.path.getsize(small)
            rgc, rgms = rg_count(big, pat, rgf, a.trials)
            rxc, rxms, _ = cc_run([rx_perf, big, rxpat or pat, rxfl], a.trials)
            rgc2, rgms2 = rg_count(small, pat, rgf, a.trials)
            fc, fms, capped = cc_run([find_perf, small, '--query', pat, fopts], a.trials)
            note = ''
            if rgc != rxc or rgc2 != fc:
                # rg is line-oriented: \s / \W / [^..] never take a newline
                # there. A match may span lines in the editor, as in rg -U.
                rgc, rgmsu = rg_count(big, pat, rgf + ['-U'], a.trials)
                rgc2, _ = rg_count(small, pat, rgf + ['-U'], 1)
                note = ' U(%.0f)' % mbps(nb, rgmsu)
            ok1 = rgc == rxc
            ok2 = rgc2 == fc and not capped
            bad += (not ok1) + (not ok2)
            rows.append((name, mbps(nb, rgms), mbps(nb, rxms), rgc, rxc, ok1,
                         mbps(ns, rgms2), mbps(ns, fms), rgc2, fc, ok2, capped, note))
            log('%s done' % name)
        print('%d MiB corpus (rx vs rg), %d MiB prefix (find vs rg); MB/s, best of %d'
              % (a.mib, a.find_mib, a.trials))
        print('%-22s %8s %8s %9s %9s  |%8s %8s %8s %8s' %
              ('query', 'rg', 'rx', 'rg n', 'rx n', 'rg', 'find', 'rg n', 'find n'))
        for (name, r1, x1, c1, d1, ok1, r2, f2, c2, e2, ok2, capped, note) in rows:
            print('%-22s %8.0f %8.0f %9d %9d%s |%8.0f %8.0f %8d %8d%s%s' %
                  (name, r1, x1, c1, d1, ' ' if ok1 else '!', r2, f2, c2, e2,
                   '' if ok2 else (' capped' if capped else ' !'), note))
        print('U(n): counts are rg -U (multiline; n = its MB/s): rg alone never lets '
              '\\s / \\W / [^..] take a newline')
        print('counts: %s' % ('all equal' if not bad else '%d mismatches (!)' % bad))
    finally:
        if not a.keep:
            for big, small, _ in files.values():
                for p in (big, small):
                    if os.path.exists(p):
                        os.remove(p)
            shutil.rmtree(os.path.join(a.dir, 'rebar'), ignore_errors=True)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
