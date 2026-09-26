#!/usr/bin/env python3
"""Case generators for the Rust-regex oracle (tests/rx_conformance/oracle).
Writes `name \\t pattern \\t haystack` lines (\\xNN escapes) on stdout.

    gen_cases.py re2 search_test.cc      RE2's re2/testing/search_test.cc
                                         regexps x texts, each plain and
                                         with (?m) / (?i) / (?s)
    gen_cases.py fuzz SEED COUNT         random patterns in the syntax both
                                         engines share, random haystacks
    gen_cases.py inner SEED COUNT        reverse inner shapes (X+ \\s+ lit Y*,
                                         X+lit, .*lit) on haystacks with
                                         newlines and planted literals

Pipeline (see bench/../README in tests/rx_conformance/convert.py):

    curl -sSfLO https://raw.githubusercontent.com/google/re2/main/re2/testing/search_test.cc
    python3 tests/rx_conformance/gen_cases.py re2 search_test.cc > re2.tsv
    cargo run --release --manifest-path tests/rx_conformance/oracle/Cargo.toml < re2.tsv > re2.toml
    python3 tests/rx_conformance/convert.py re2.toml > re2.fix
    bin/rx_conform re2.fix
"""
import ast
import random
import re
import sys


def esc(s: str) -> str:
    out = []
    for ch in s:
        o = ord(ch)
        if ch == '\\' or o < 0x20 or o == 0x7F:
            out.append('\\x%02X' % o)
        else:
            out.append(ch)
    return ''.join(out)


def c_string(lit: str) -> bytes:
    # C string literal body -> bytes (C escapes are a subset of Python's
    # bytes-literal escapes for this file).
    return ast.literal_eval('b"' + lit + '"')


def re2(path):
    src = open(path, encoding='utf-8').read()
    body = src[src.index('simple_tests[] = {'):]
    body = body[:body.index('\n};')]
    pairs = re.findall(r'\{\s*"((?:[^"\\]|\\.)*)"\s*,\s*"((?:[^"\\]|\\.)*)"\s*\}', body)
    k = 0
    for rp, tx in pairs:
        try:
            p = c_string(rp).decode('utf-8')
            t = c_string(tx).decode('utf-8')
        except (UnicodeDecodeError, ValueError, SyntaxError):
            continue
        for pre in ('', '(?m)', '(?i)', '(?s)'):
            print('re2/%03d%s\t%s\t%s' % (k, pre.strip('(?)') and '-' + pre.strip('(?)') or '',
                                           esc(pre + p), esc(t)))
        k += 1


# Both engines agree on these scalars' classes (word / space / case).
ALPHA = ['a', 'b', 'c', 'a', 'b', 'A', 'B', ' ', '\n', '-', '_', '1', '9',
         'é', 'É', 'δ', 'Δ', '中', '.', 'x']


def gen_atom(r, depth):
    k = r.randrange(22 if depth < 3 else 15)
    if k < 6:
        c = r.choice(ALPHA)
        return '\\.' if c == '.' else ('\\n' if c == '\n' else c)
    if k == 6:
        return '.'
    if k == 7:
        return r.choice(['[abc]', '[^a\\n]', '[a-c]', '[^b]', '[a-cA]', '[\\d_]', '[^\\s]',
                         '[éδ]', '[-a]', '[[:digit:]x]', '[[:space:]b]'])
    if k == 8:
        return r.choice(['\\d', '\\w', '\\s', '\\D', '\\W', '\\S'])
    if k == 9:
        return r.choice(['\\b', '\\B', '^', '$', '\\A', '\\z'])
    if k == 10:
        return r.choice(['ab', 'abc', 'ba', 'Ab', 'éδ'])
    if k < 15:
        return gen_atom(r, depth + 1) + gen_atom(r, depth + 1)
    if k < 18:
        return '(' + gen_alt(r, depth + 1) + ')'
    if k < 20:
        return '(?:' + gen_alt(r, depth + 1) + ')'
    return '(?' + r.choice(['i', 's', 'm', '-i']) + ':' + gen_alt(r, depth + 1) + ')'


def gen_quant(r, a, depth):
    if r.random() < 0.6 or a in ('^', '$', '\\b', '\\B', '\\A', '\\z'):
        return a
    q = r.choice(['*', '+', '?', '{2}', '{1,3}', '{0,2}', '{2,}'])
    if r.random() < 0.3:
        q += '?'
    return a + q


def gen_cat(r, depth):
    return ''.join(gen_quant(r, gen_atom(r, depth), depth) for _ in range(r.randint(1, 3)))


def gen_alt(r, depth):
    n = 1 if r.random() < 0.7 else r.randint(2, 3)
    return '|'.join(gen_cat(r, depth) for _ in range(n))


def fuzz(seed, count):
    r = random.Random(seed)
    for k in range(count):
        p = gen_alt(r, 0)
        if r.random() < 0.15:
            p = r.choice(['(?i)', '(?m)', '(?s)']) + p
        h = ''.join(r.choice(ALPHA) for _ in range(r.randint(0, 14)))
        print('fuzz/%d-%d\t%s\t%s' % (seed, k, esc(p), esc(h)))


# Reverse inner / reverse suffix shapes: an unbounded prefix (that may
# take a newline) before a required literal, e.g. `\w+\s+Holmes\s+\w+`,
# `[a-z]+_x\(`, `.*ab`, on longer haystacks with newlines and planted
# literals (overlapping ones too).
INNER_X = ['\\w', '[a-c]', '[a-z]', '\\S', '\\d', '[^\\n]', '.', '[^x]', '[ab]', '\\w\\w',
           '[a-zé]', '(?:ab|a)', '\\D', '[^ ]']
INNER_SEP = ['\\s+', '\\s*', ' ', '\\s', '\\s+\\b', '[ \\n]+', '\\W+', '-', '', '\\s{1,2}',
             '(?:\\s|,)+', '[^a-z]+']
INNER_LIT = ['Holmes', 'ab', 'abab', 'aab', 'xy', '_x\\(', 'é', 'δΔ', '中', 'Ab', 'x',
             '(?:ab|ba)', '(?:Holmes|Watson)', '(?i:hol)', 'b\\n', 'a-']
INNER_Y = ['', '\\w*', '\\s+\\w+', '[0-9]+', 'x?', '\\b', '$', '(?:ab)*', '.*', '\\s*\\w{2}']
INNER_Q = ['+', '*', '+?', '{2,}', '{1,3}', '', '*?']
INNER_TEXT = ['a', 'b', 'c', 'x', 'y', ' ', ' ', '\n', '\n', '-', '_', '(', ',', '1', '9', 'é',
              'δ', 'Δ', '中', 'A', 'H', 'Holmes', 'Watson', 'ab', 'ba', 'hol', 'HOL', 'xy', '_x(']


def inner(seed, count):
    r = random.Random(seed)
    for k in range(count):
        x = r.choice(INNER_X) + r.choice(INNER_Q)
        if r.random() < 0.3:
            x = r.choice(['^', '\\b', '(', '(?:']) + x
            if x[0] == '(':
                x += ')'
        sep = r.choice(INNER_SEP)
        if r.random() < 0.25:
            sep = sep + r.choice(INNER_X) + r.choice(INNER_Q)
        p = x + sep + r.choice(INNER_LIT) + r.choice(INNER_Y)
        if r.random() < 0.15:
            p = r.choice(['(?i)', '(?m)', '(?s)']) + p
        h = ''.join(r.choice(INNER_TEXT) for _ in range(r.randint(0, 60)))
        print('inner/%d-%d\t%s\t%s' % (seed, k, esc(p), esc(h)))


def main():
    a = sys.argv[1:]
    if len(a) == 2 and a[0] == 're2':
        re2(a[1])
    elif len(a) == 3 and a[0] == 'fuzz':
        fuzz(int(a[1]), int(a[2]))
    elif len(a) == 3 and a[0] == 'inner':
        inner(int(a[1]), int(a[2]))
    else:
        sys.stderr.write(__doc__)
        return 2
    return 0


if __name__ == '__main__':
    sys.exit(main())
