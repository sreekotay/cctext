#!/usr/bin/env python3
"""Turn the Rust `regex` crate's TOML test data into a compact fixture for
tests/rx_conform.ccs (the cctext regex engine's conformance runner).

    git clone --depth 1 https://github.com/rust-lang/regex /tmp/regex
    python3 tests/rx_conformance/convert.py /tmp/regex/testdata \
        > tests/rx_conformance/rust_regex.fix

With --fowler the testdata/fowler/*.toml files (AT&T testregex data, not
MIT/Apache) are converted instead; that fixture is not committed:

    python3 tests/rx_conformance/convert.py --fowler /tmp/regex/testdata \
        > /tmp/fowler.fix
    bin/rx_conform /tmp/fowler.fix

The pattern is translated from Rust syntax to the engine's Oniguruma
flavour: `^` / `$` outside (?m) become `\\A` / `\\z`, `.` under (?s)
becomes `(?m:.)` (Onig's dotall), (?m) / (?s) / (?u) leave the flag
groups, (?U) swaps greedy and lazy, `(?P<n>` becomes `(?<n>`, `\\u{..}`
/ `\\U..` become `\\x{..}`, and a `\\xNN` >= 0x80 under (?-u) becomes the
raw byte. A test the runner cannot express is kept with a reason.

Fixture: one test per line, tab-separated, each field with `\\`, tab,
newline, CR, controls, DEL and non-UTF-8 bytes as `\\xNN`:

    id  kind  flags  pattern  haystack  lo  hi  matches

kind: `run` (compare every engine path), `reject` (Rust refuses to
compile the pattern), `unsup:<why>` (syntax the engine deliberately
lacks: the runner checks that compile fails), `skip:<why>` (not
expressible here: API differences, Rust-only syntax the engine would
read differently). flags: `i` case-insensitive, `a` anchored, `8` Rust's
utf8 iteration (no empty match inside a scalar), `1` first match only,
`A` the pattern is in Rust's ASCII mode (unicode = false or (?-u)).
matches: `;`-separated matches, each `,`-separated group spans `s-e`
(`_` = group did not take part).
"""
import glob
import os
import re
import sys
import tomllib

RUST_FILES_SKIP = set()


def esc(b: bytes) -> str:
    out = []
    try:
        s = b.decode('utf-8')
    except UnicodeDecodeError:
        s = None
    if s is not None:
        for ch in s:
            o = ord(ch)
            if ch == '\\' or o < 0x20 or o == 0x7F:
                out.append('\\x%02X' % o)
            else:
                out.append(ch)
        return ''.join(out)
    for x in b:
        if x == 0x5C or x < 0x20 or x >= 0x7F:
            out.append('\\x%02X' % x)
        else:
            out.append(chr(x))
    return ''.join(out)


def unescape(s: str) -> bytes:
    """bstr-style unescape (regex-test `unescape = true`)."""
    out = bytearray()
    b = s.encode('utf-8')
    i = 0
    while i < len(b):
        c = b[i]
        if c != 0x5C or i + 1 >= len(b):
            out.append(c)
            i += 1
            continue
        d = chr(b[i + 1])
        if d == 'x' and i + 3 < len(b) + 0 and re.match(rb'[0-9A-Fa-f]{2}', b[i + 2:i + 4]):
            out.append(int(b[i + 2:i + 4], 16))
            i += 4
            continue
        m = {'n': 10, 'r': 13, 't': 9, '0': 0, '\\': 0x5C}
        if d in m:
            out.append(m[d])
            i += 2
            continue
        out.append(c)
        i += 1
    return bytes(out)


class Skip(Exception):
    def __init__(self, kind, why):
        super().__init__(why)
        self.kind = kind
        self.why = why


def translate(pat: str, unicode: bool) -> (bytes, bool):
    """Rust regex syntax -> the engine's Onig flavour. Returns (bytes,
    ascii_mode_used). Raises Skip."""
    p = pat
    n = len(p)
    out = bytearray()
    ascii_used = not unicode
    # flag state: m (multi-line), s (dot-all), u (unicode), U (swap greed), x
    st = [{'m': False, 's': False, 'u': unicode, 'U': False, 'x': False}]
    i = 0

    def emit(s):
        if isinstance(s, str):
            out.extend(s.encode('utf-8'))
        else:
            out.extend(s)

    def hexcp(j):
        """Rust \\x / \\u / \\U at p[j] == letter. Returns (cp, next, braced)."""
        letter = p[j]
        k = j + 1
        if k < n and p[k] == '{':
            e = p.find('}', k)
            if e < 0:
                raise Skip('reject', 'bad escape')
            v = p[k + 1:e]
            if not re.fullmatch(r'[0-9A-Fa-f]{1,8}', v):
                return None, e + 1
            return int(v, 16), e + 1
        want = {'x': 2, 'u': 4, 'U': 8}[letter]
        v = p[k:k + want]
        if not re.fullmatch(r'[0-9A-Fa-f]{%d}' % want, v):
            return None, k
        return int(v, 16), k + want

    def scalar_escape(j, in_class):
        """p[j] is the char after a backslash. Returns (bytes, next) for a
        scalar / class escape or None when not handled here."""
        c = p[j]
        f = st[-1]
        if c in 'xuU':
            cp, k = hexcp(j)
            if cp is None:
                raise Skip('reject', 'bad hex escape')
            if cp > 0x10FFFF or 0xD800 <= cp <= 0xDFFF:
                if not f['u'] and cp <= 0xFF:
                    pass
                else:
                    raise Skip('reject', 'invalid code point')
            if not f['u'] and cp >= 0x80:
                if in_class:
                    raise Skip('skip', 'byte class (?-u:[\\x80-\\xFF])')
                return bytes([cp]), k
            if cp < 0x80 and not (0x30 <= cp <= 0x39 or 0x41 <= cp <= 0x5A or 0x61 <= cp <= 0x7A):
                return ('\\x{%X}' % cp).encode(), k
            if cp < 0x80:
                return bytes([cp]), k
            return ('\\x{%X}' % cp).encode(), k
        return None

    def quant_tail(j):
        """After a quantifier at p[..j): copy lazy marker, apply (?U)."""
        lazy = False
        if j < n and p[j] == '?':
            lazy = True
            j += 1
        if st[-1]['U']:
            lazy = not lazy
        if lazy:
            emit('?')
        return j

    while i < n:
        c = p[i]
        f = st[-1]
        if f['x'] and c in ' \t\n\r\f\v':
            i += 1
            continue
        if f['x'] and c == '#':
            while i < n and p[i] != '\n':
                i += 1
            continue
        if c == '\\':
            if i + 1 >= n:
                raise Skip('reject', 'trailing backslash')
            d = p[i + 1]
            if d == 'b' and i + 2 < n and p[i + 2] == '{':
                raise Skip('skip', 'Rust-only \\b{start}/{end}')
            if d in '<>':
                raise Skip('skip', 'Rust-only \\< \\>')
            if d in 'pP':
                emit('\\' + d)
                i += 2
                raise Skip('unsup', 'Unicode property \\p / \\P')
            if d in 'bBdDwWsS' and not f['u']:
                ascii_used = True
            r = scalar_escape(i + 1, False)
            if r is not None:
                emit(r[0])
                i = r[1]
                continue
            emit('\\' + d)
            i += 2
            continue
        if c == '[':
            i = translate_class(p, i, st, emit, scalar_escape)
            if not st[-1]['u']:
                ascii_used = True
            continue
        if c == '(':
            if p.startswith('(?P<', i):
                emit('(?<')
                i += 4
                st.append(dict(st[-1]))
                continue
            if p.startswith('(?<', i) or p.startswith('(?:', i):
                emit(p[i:i + 3])
                i += 3
                st.append(dict(st[-1]))
                continue
            if p.startswith('(?', i):
                j = i + 2
                neg = False
                g = dict(st[-1])
                keep = ''
                while j < n and p[j] not in ':)':
                    fl = p[j]
                    if fl == '-':
                        neg = True
                        keep += '-'
                    elif fl in 'ix':
                        g['x' if fl == 'x' else 'i'] = not neg
                        keep += fl
                    elif fl in 'msuU':
                        g[fl] = not neg
                    elif fl == 'R':
                        raise Skip('skip', 'Rust-only (?R) CRLF mode')
                    else:
                        raise Skip('reject', 'bad flag')
                    j += 1
                if j >= n:
                    raise Skip('reject', 'bad group')
                keep = keep.rstrip('-')
                if not g['u']:
                    ascii_used = True
                if p[j] == ')':
                    if keep:
                        emit('(?' + keep + ')')
                    st[-1] = g
                    i = j + 1
                    continue
                emit('(?' + keep + ':')
                st.append(g)
                i = j + 1
                continue
            emit('(')
            st.append(dict(st[-1]))
            i += 1
            continue
        if c == ')':
            if len(st) > 1:
                st.pop()
            emit(')')
            i += 1
            continue
        if c == '^':
            emit('^' if f['m'] else '\\A')
            i += 1
            continue
        if c == '$':
            emit('$' if f['m'] else '\\z')
            i += 1
            continue
        if c == '.':
            emit('(?m:.)' if f['s'] else '.')
            i += 1
            continue
        if c in '*+?':
            emit(c)
            i = quant_tail(i + 1)
            continue
        if c == '{':
            m = re.match(r'\{\s*(\d*)\s*(,\s*(\d*)\s*)?\}', p[i:])
            if m and (m.group(1) or m.group(3)):
                a = m.group(1)
                if m.group(2):
                    emit('{%s,%s}' % (a, m.group(3) or ''))
                else:
                    emit('{%s}' % a)
                i = quant_tail(i + m.end())
                continue
            raise Skip('reject', 'bad repetition')
        emit(c)
        i += 1
    return bytes(out), ascii_used


def translate_class(p, i, st, emit, scalar_escape):
    """Copy a bracket class starting at p[i] == '['. Returns next index."""
    n = len(p)
    depth = 0
    j = i
    first = True
    while j < n:
        c = p[j]
        if c == '[':
            if depth > 0 and p.startswith('[:', j):
                e = p.find(':]', j)
                if e < 0:
                    raise Skip('reject', 'bad posix')
                emit(p[j:e + 2])
                j = e + 2
                first = False
                continue
            depth += 1
            emit('[')
            j += 1
            if j < n and p[j] == '^':
                emit('^')
                j += 1
            first = True
            while j < n and p[j] == ']' and first:
                emit('\\]')
                j += 1
                first = False
            first = False
            continue
        if c == ']':
            depth -= 1
            emit(']')
            j += 1
            if depth == 0:
                return j
            continue
        if c == '\\':
            if j + 1 >= n:
                raise Skip('reject', 'bad class escape')
            d = p[j + 1]
            if d in 'pP':
                raise Skip('unsup', 'Unicode property \\p / \\P')
            r = scalar_escape(j + 1, True)
            if r is not None:
                emit(r[0])
                j = r[1]
                continue
            emit('\\' + d)
            j += 2
            continue
        if p.startswith('--', j) or p.startswith('~~', j):
            raise Skip('skip', 'Rust-only class set operation -- / ~~')
        if c == '&' and p.startswith('&&', j):
            emit('&&')
            j += 2
            continue
        if st[-1]['x'] and c in ' \t\n':
            j += 1
            continue
        emit(c)
        j += 1
    raise Skip('reject', 'unclosed class')


def spans_of(m):
    if isinstance(m, dict):
        if 'spans' in m:
            return m['spans']
        return [m['span']]
    if m and isinstance(m[0], list):
        return m
    if m and m[0] is None:
        return m
    return [m]


def convert(path, prefix):
    d = tomllib.load(open(path, 'rb'))
    rows = []
    for t in d['test']:
        name = prefix + t['name']
        hay = t['haystack']
        hb = unescape(hay) if t.get('unescape') else hay.encode('utf-8')
        unicode = t.get('unicode', True)
        utf8 = t.get('utf8', True)
        flags = ''
        if t.get('case-insensitive'):
            flags += 'i'
        if t.get('anchored'):
            flags += 'a'
        if utf8:
            flags += '8'
        if t.get('match-limit') == 1:
            flags += '1'
        lo, hi = t.get('bounds', [0, len(hb)])
        kind = 'run'
        pat = t['regex']
        pb = b''
        why = None
        ms = []
        try:
            if isinstance(pat, list):
                raise Skip('skip', 'regex set')
            mk = t.get('match-kind', 'leftmost-first')
            sk = t.get('search-kind', 'leftmost')
            if mk != 'leftmost-first':
                raise Skip('skip', 'match-kind ' + mk)
            if sk != 'leftmost':
                raise Skip('skip', 'search-kind ' + sk)
            lt = t.get('line-terminator')
            if lt is not None and lt != '\n':
                raise Skip('skip', 'line-terminator')
            if hi != len(hb):
                raise Skip('skip', 'bounds end before haystack end')
            try:
                pb, asc = translate(pat, unicode)
            except Skip as e:
                if e.kind == 'unsup':
                    pb = pat.encode('utf-8')
                raise
            if asc or prefix.startswith('regex-lite/'):
                flags += 'A'  # regex-lite is ASCII-only too
            if t.get('compiles', True) is False:
                kind = 'reject'
            for m in t['matches']:
                sp = spans_of(m)
                ms.append(','.join('_' if not s else '%d-%d' % (s[0], s[1]) for s in sp))
        except Skip as e:
            raw = pat.encode('utf-8') if isinstance(pat, str) else b''
            if t.get('compiles', True) is False:
                kind = 'reject'
                pb = pb or raw
            elif e.kind == 'unsup':
                kind = 'unsup:' + e.why
                pb = raw
            elif e.kind == 'skip':
                kind = 'skip:' + e.why
                pb = raw
            else:
                kind = 'skip:translator: ' + e.why
                pb = raw
        rows.append('\t'.join([esc(name.encode()), kind, flags, esc(pb), esc(hb),
                               str(lo), str(hi), ';'.join(ms)]))
    return rows


def main():
    args = sys.argv[1:]
    fowler = False
    if args and args[0] == '--fowler':
        fowler = True
        args = args[1:]
    if len(args) != 1:
        sys.stderr.write(__doc__)
        return 2
    root = args[0]
    if os.path.isfile(root):
        files = [root]
    elif fowler:
        files = sorted(glob.glob(os.path.join(root, 'fowler', '*.toml')))
    else:
        files = sorted(glob.glob(os.path.join(root, '*.toml')))
    print('# cctext rx conformance fixture, generated by tests/rx_conformance/convert.py')
    if os.path.isfile(root):
        print('# from %s (expected answers: the Rust regex crate)' % os.path.basename(root))
    elif fowler:
        print('# from rust-lang/regex testdata/fowler (AT&T testregex data; not for commit)')
    else:
        print('# from rust-lang/regex testdata (MIT OR Apache-2.0, Copyright (c) 2014 The Rust')
        print('# Project Developers); see tests/rx_conformance/LICENSE-regex-testdata.')
    print('# id\tkind\tflags\tpattern\thaystack\tlo\thi\tmatches')
    for f in files:
        base = os.path.splitext(os.path.basename(f))[0]
        prefix = ('fowler/' if fowler else '') + base + '/'
        if os.path.isfile(root):
            prefix = ''
        for r in convert(f, prefix):
            print(r)
    return 0


if __name__ == '__main__':
    sys.exit(main())
