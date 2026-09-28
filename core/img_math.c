/*
 * Simple TeX as Unicode text (docs/images.md "Math", the terminal's
 * `math_unicode` setting): x^2 -> x², a_i -> aᵢ, \sqrt{x} -> √x,
 * \frac{a}{b} -> a/b, \alpha -> α, \le -> ≤. Anything this does not know
 * (an unknown macro, a superscript letter Unicode has no form for, an
 * environment) makes the whole formula "not simple": the caller keeps the
 * source. Plain C, no allocation; declared in core/img.cch.
 */
#include <stddef.h>
#include <string.h>

size_t rtx_math_unicode(const char *src, size_t n, char *out, size_t cap);

typedef struct {
    const char *s;
    size_t i, n;
    char *o;
    size_t k, cap;
    int bad;
    int depth;
} MuSt;

static void mu_put(MuSt *m, const char *t) {
    size_t l = strlen(t);
    if (m->k + l + 1 > m->cap) {
        m->bad = 1;
        return;
    }
    memcpy(m->o + m->k, t, l);
    m->k += l;
}

static void mu_putc(MuSt *m, char c) {
    char b[2] = {c, 0};
    mu_put(m, b);
}

typedef struct {
    const char *name;
    const char *u;
} MuSym;

/* Macros that are one symbol. */
static const MuSym mu_syms[] = {
    {"alpha", "\xce\xb1"}, {"beta", "\xce\xb2"}, {"gamma", "\xce\xb3"}, {"delta", "\xce\xb4"},
    {"epsilon", "\xcf\xb5"}, {"varepsilon", "\xce\xb5"}, {"zeta", "\xce\xb6"}, {"eta", "\xce\xb7"},
    {"theta", "\xce\xb8"}, {"vartheta", "\xcf\x91"}, {"iota", "\xce\xb9"}, {"kappa", "\xce\xba"},
    {"lambda", "\xce\xbb"}, {"mu", "\xce\xbc"}, {"nu", "\xce\xbd"}, {"xi", "\xce\xbe"},
    {"pi", "\xcf\x80"}, {"varpi", "\xcf\x96"}, {"rho", "\xcf\x81"}, {"varrho", "\xcf\xb1"},
    {"sigma", "\xcf\x83"}, {"varsigma", "\xcf\x82"}, {"tau", "\xcf\x84"}, {"upsilon", "\xcf\x85"},
    {"phi", "\xcf\x95"}, {"varphi", "\xcf\x86"}, {"chi", "\xcf\x87"}, {"psi", "\xcf\x88"},
    {"omega", "\xcf\x89"}, {"Gamma", "\xce\x93"}, {"Delta", "\xce\x94"}, {"Theta", "\xce\x98"},
    {"Lambda", "\xce\x9b"}, {"Xi", "\xce\x9e"}, {"Pi", "\xce\xa0"}, {"Sigma", "\xce\xa3"},
    {"Upsilon", "\xce\xa5"}, {"Phi", "\xce\xa6"}, {"Psi", "\xce\xa8"}, {"Omega", "\xce\xa9"},
    {"cdot", "\xc2\xb7"}, {"times", "\xc3\x97"}, {"div", "\xc3\xb7"}, {"pm", "\xc2\xb1"},
    {"mp", "\xe2\x88\x93"}, {"le", "\xe2\x89\xa4"}, {"leq", "\xe2\x89\xa4"},
    {"ge", "\xe2\x89\xa5"}, {"geq", "\xe2\x89\xa5"}, {"ne", "\xe2\x89\xa0"},
    {"neq", "\xe2\x89\xa0"}, {"approx", "\xe2\x89\x88"}, {"equiv", "\xe2\x89\xa1"},
    {"sim", "\xe2\x88\xbc"}, {"simeq", "\xe2\x89\x83"}, {"propto", "\xe2\x88\x9d"},
    {"infty", "\xe2\x88\x9e"}, {"to", "\xe2\x86\x92"}, {"rightarrow", "\xe2\x86\x92"},
    {"leftarrow", "\xe2\x86\x90"}, {"gets", "\xe2\x86\x90"}, {"Rightarrow", "\xe2\x87\x92"},
    {"Leftarrow", "\xe2\x87\x90"}, {"leftrightarrow", "\xe2\x86\x94"},
    {"Leftrightarrow", "\xe2\x87\x94"}, {"iff", "\xe2\x87\x94"}, {"implies", "\xe2\x87\x92"},
    {"mapsto", "\xe2\x86\xa6"}, {"in", "\xe2\x88\x88"}, {"notin", "\xe2\x88\x89"},
    {"ni", "\xe2\x88\x8b"}, {"subset", "\xe2\x8a\x82"}, {"subseteq", "\xe2\x8a\x86"},
    {"supset", "\xe2\x8a\x83"}, {"supseteq", "\xe2\x8a\x87"}, {"cup", "\xe2\x88\xaa"},
    {"cap", "\xe2\x88\xa9"}, {"setminus", "\xe2\x88\x96"}, {"forall", "\xe2\x88\x80"},
    {"exists", "\xe2\x88\x83"}, {"nexists", "\xe2\x88\x84"}, {"partial", "\xe2\x88\x82"},
    {"nabla", "\xe2\x88\x87"}, {"sum", "\xe2\x88\x91"}, {"prod", "\xe2\x88\x8f"},
    {"int", "\xe2\x88\xab"}, {"oint", "\xe2\x88\xae"}, {"emptyset", "\xe2\x88\x85"},
    {"varnothing", "\xe2\x88\x85"}, {"ldots", "\xe2\x80\xa6"}, {"dots", "\xe2\x80\xa6"},
    {"cdots", "\xe2\x8b\xaf"}, {"circ", "\xe2\x88\x98"}, {"prime", "\xe2\x80\xb2"},
    {"perp", "\xe2\x8a\xa5"}, {"parallel", "\xe2\x88\xa5"}, {"neg", "\xc2\xac"},
    {"lnot", "\xc2\xac"}, {"land", "\xe2\x88\xa7"}, {"wedge", "\xe2\x88\xa7"},
    {"lor", "\xe2\x88\xa8"}, {"vee", "\xe2\x88\xa8"}, {"oplus", "\xe2\x8a\x95"},
    {"otimes", "\xe2\x8a\x97"}, {"ll", "\xe2\x89\xaa"}, {"gg", "\xe2\x89\xab"},
    {"hbar", "\xe2\x84\x8f"}, {"ell", "\xe2\x84\x93"}, {"Re", "\xe2\x84\x9c"},
    {"Im", "\xe2\x84\x91"}, {"aleph", "\xe2\x84\xb5"}, {"angle", "\xe2\x88\xa0"},
    {"deg", "deg"}, {"sin", "sin"}, {"cos", "cos"}, {"tan", "tan"}, {"log", "log"},
    {"ln", "ln"}, {"exp", "exp"}, {"lim", "lim"}, {"max", "max"}, {"min", "min"},
    {"det", "det"}, {"gcd", "gcd"}, {"mod", "mod"}, {"lbrace", "{"}, {"rbrace", "}"},
    {"langle", "\xe2\x9f\xa8"}, {"rangle", "\xe2\x9f\xa9"}, {"lfloor", "\xe2\x8c\x8a"},
    {"rfloor", "\xe2\x8c\x8b"}, {"lceil", "\xe2\x8c\x88"}, {"rceil", "\xe2\x8c\x89"},
    {"vert", "|"}, {"mid", "|"}, {"|", "\xe2\x80\x96"}, {"quad", "  "}, {"qquad", "    "},
    {",", " "}, {";", " "}, {":", " "}, {"!", ""}, {" ", " "}, {"{", "{"}, {"}", "}"},
    {"%", "%"}, {"$", "$"}, {"#", "#"}, {"&", "&"}, {"_", "_"},
    {NULL, NULL},
};

/* Superscript / subscript forms: the ASCII character, its UTF-8 form. */
static const char *const mu_sup[][2] = {
    {"0", "\xe2\x81\xb0"}, {"1", "\xc2\xb9"}, {"2", "\xc2\xb2"}, {"3", "\xc2\xb3"},
    {"4", "\xe2\x81\xb4"}, {"5", "\xe2\x81\xb5"}, {"6", "\xe2\x81\xb6"}, {"7", "\xe2\x81\xb7"},
    {"8", "\xe2\x81\xb8"}, {"9", "\xe2\x81\xb9"}, {"+", "\xe2\x81\xba"}, {"-", "\xe2\x81\xbb"},
    {"=", "\xe2\x81\xbc"}, {"(", "\xe2\x81\xbd"}, {")", "\xe2\x81\xbe"}, {"n", "\xe2\x81\xbf"},
    {"i", "\xe2\x81\xb1"}, {"a", "\xe1\xb5\x83"}, {"b", "\xe1\xb5\x87"}, {"c", "\xe1\xb6\x9c"},
    {"d", "\xe1\xb5\x88"}, {"e", "\xe1\xb5\x89"}, {"f", "\xe1\xb6\xa0"}, {"g", "\xe1\xb5\x8d"},
    {"h", "\xca\xb0"}, {"j", "\xca\xb2"}, {"k", "\xe1\xb5\x8f"}, {"l", "\xcb\xa1"},
    {"m", "\xe1\xb5\x90"}, {"o", "\xe1\xb5\x92"}, {"p", "\xe1\xb5\x96"}, {"r", "\xca\xb3"},
    {"s", "\xcb\xa2"}, {"t", "\xe1\xb5\x97"}, {"u", "\xe1\xb5\x98"}, {"v", "\xe1\xb5\x9b"},
    {"w", "\xca\xb7"}, {"x", "\xcb\xa3"}, {"y", "\xca\xb8"}, {"z", "\xe1\xb6\xbb"},
    {"T", "\xe1\xb5\x80"}, {"*", "*"}, {"'", "\xe2\x80\xb2"}, {NULL, NULL},
};
static const char *const mu_sub[][2] = {
    {"0", "\xe2\x82\x80"}, {"1", "\xe2\x82\x81"}, {"2", "\xe2\x82\x82"}, {"3", "\xe2\x82\x83"},
    {"4", "\xe2\x82\x84"}, {"5", "\xe2\x82\x85"}, {"6", "\xe2\x82\x86"}, {"7", "\xe2\x82\x87"},
    {"8", "\xe2\x82\x88"}, {"9", "\xe2\x82\x89"}, {"+", "\xe2\x82\x8a"}, {"-", "\xe2\x82\x8b"},
    {"=", "\xe2\x82\x8c"}, {"(", "\xe2\x82\x8d"}, {")", "\xe2\x82\x8e"}, {"a", "\xe2\x82\x90"},
    {"e", "\xe2\x82\x91"}, {"h", "\xe2\x82\x95"}, {"i", "\xe1\xb5\xa2"}, {"j", "\xe2\xb1\xbc"},
    {"k", "\xe2\x82\x96"}, {"l", "\xe2\x82\x97"}, {"m", "\xe2\x82\x98"}, {"n", "\xe2\x82\x99"},
    {"o", "\xe2\x82\x92"}, {"p", "\xe2\x82\x9a"}, {"r", "\xe1\xb5\xa3"}, {"s", "\xe2\x82\x9b"},
    {"t", "\xe2\x82\x9c"}, {"u", "\xe1\xb5\xa4"}, {"v", "\xe1\xb5\xa5"}, {"x", "\xe2\x82\x93"},
    {NULL, NULL},
};

static int mu_letter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

/* A macro name at m->s[m->i] (after the backslash): letters, or one other
 * character. */
static size_t mu_name(MuSt *m, char *nm, size_t cap) {
    size_t j = m->i, l;
    if (j >= m->n) return 0;
    if (mu_letter(m->s[j]))
        while (j < m->n && mu_letter(m->s[j])) j++;
    else
        j++;
    l = j - m->i;
    if (l + 1 > cap) return 0;
    memcpy(nm, m->s + m->i, l);
    nm[l] = 0;
    m->i = j;
    return l;
}

static void mu_skip_ws(MuSt *m) {
    while (m->i < m->n && (m->s[m->i] == ' ' || m->s[m->i] == '\t' || m->s[m->i] == '\n'))
        m->i++;
}

static void mu_expr(MuSt *m, size_t end, int stop_brace);

/* One argument: a {group} or a single token, rendered into a scratch
 * buffer (for scripts and fractions). */
static int mu_arg(MuSt *m, char *buf, size_t cap, int *complex) {
    MuSt t = *m;
    size_t start;
    t.o = buf;
    t.k = 0;
    t.cap = cap;
    t.depth = m->depth + 1;
    if (t.depth > 32) {
        m->bad = 1;
        return 0;
    }
    mu_skip_ws(&t);
    if (t.i >= t.n) {
        m->bad = 1;
        return 0;
    }
    start = t.i;
    if (t.s[t.i] == '{') {
        t.i++;
        mu_expr(&t, t.n, 1);
        if (t.i >= t.n || t.s[t.i] != '}') t.bad = 1;
        else t.i++;
        *complex = t.i - start > 3;
    } else if (t.s[t.i] == '\\') {
        char nm[32];
        const MuSym *y;
        t.i++;
        if (!mu_name(&t, nm, sizeof nm)) t.bad = 1;
        for (y = mu_syms; !t.bad && y->name; y++)
            if (strcmp(y->name, nm) == 0) break;
        if (!t.bad && y->name) mu_put(&t, y->u);
        else t.bad = 1;
        *complex = 0;
    } else {
        /* one character (a whole UTF-8 sequence) */
        size_t j = t.i + 1;
        while (j < t.n && ((unsigned char)t.s[j] & 0xC0) == 0x80) j++;
        if (t.s[t.i] == '}' || t.s[t.i] == '^' || t.s[t.i] == '_') t.bad = 1;
        else if (t.k + (j - t.i) + 1 <= t.cap) {
            memcpy(t.o + t.k, t.s + t.i, j - t.i);
            t.k += j - t.i;
        } else
            t.bad = 1;
        t.i = j;
        *complex = 0;
    }
    buf[t.k] = 0;
    m->i = t.i;
    if (t.bad) m->bad = 1;
    return !t.bad;
}

/* A script (^ / _): every character of the argument must have a form. */
static void mu_script(MuSt *m, int sup) {
    char a[128];
    int cx = 0;
    size_t j;
    if (!mu_arg(m, a, sizeof a, &cx)) return;
    for (j = 0; a[j]; j++) {
        const char *const(*t)[2] = sup ? mu_sup : mu_sub;
        char c[2] = {a[j], 0};
        int k, found = 0;
        if (c[0] == ' ') continue;
        for (k = 0; t[k][0]; k++)
            if (strcmp(t[k][0], c) == 0) {
                mu_put(m, t[k][1]);
                found = 1;
                break;
            }
        if (!found) {
            m->bad = 1;
            return;
        }
    }
}

static void mu_expr(MuSt *m, size_t end, int stop_brace) {
    while (!m->bad && m->i < end) {
        char c = m->s[m->i];
        if (c == '}') {
            if (stop_brace) return;
            m->bad = 1;
            return;
        }
        if (c == '{') {
            MuSt t = *m;
            if (++m->depth > 32) {
                m->bad = 1;
                return;
            }
            m->i++;
            mu_expr(m, end, 1);
            m->depth--;
            if (m->i >= end || m->s[m->i] != '}') {
                m->bad = 1;
                return;
            }
            m->i++;
            (void)t;
            continue;
        }
        if (c == '^' || c == '_') {
            m->i++;
            mu_script(m, c == '^');
            continue;
        }
        if (c == '&' || c == '#' || c == '~' || c == '%') {
            if (c == '~') {
                mu_putc(m, ' ');
                m->i++;
                continue;
            }
            m->bad = 1;
            return;
        }
        if (c == '\\') {
            char nm[32];
            const MuSym *y;
            m->i++;
            if (!mu_name(m, nm, sizeof nm)) {
                m->bad = 1;
                return;
            }
            if (!strcmp(nm, "left") || !strcmp(nm, "right") || !strcmp(nm, "displaystyle") ||
                !strcmp(nm, "textstyle") || !strcmp(nm, "limits") || !strcmp(nm, "nolimits") ||
                !strcmp(nm, "big") || !strcmp(nm, "Big") || !strcmp(nm, "bigg") ||
                !strcmp(nm, "Bigg")) {
                /* sizing: the delimiter that follows stays ("." is none) */
                mu_skip_ws(m);
                if (m->i < end && m->s[m->i] == '.') m->i++;
                continue;
            }
            if (!strcmp(nm, "frac") || !strcmp(nm, "dfrac") || !strcmp(nm, "tfrac")) {
                char a[256], b[256];
                int ca = 0, cb = 0;
                if (!mu_arg(m, a, sizeof a, &ca) || !mu_arg(m, b, sizeof b, &cb)) return;
                /* a/b; a side longer than one symbol gets parentheses */
                ca = strlen(a) > 4 || strpbrk(a, "+-= ") != NULL;
                cb = strlen(b) > 4 || strpbrk(b, "+-= ") != NULL;
                if (ca) mu_putc(m, '(');
                mu_put(m, a);
                if (ca) mu_putc(m, ')');
                mu_putc(m, '/');
                if (cb) mu_putc(m, '(');
                mu_put(m, b);
                if (cb) mu_putc(m, ')');
                continue;
            }
            if (!strcmp(nm, "sqrt")) {
                char a[256];
                int ca = 0;
                mu_skip_ws(m);
                if (m->i < end && m->s[m->i] == '[') {
                    /* \sqrt[3]{x}: ∛ / ∜, else not simple */
                    if (m->i + 2 < end && m->s[m->i + 2] == ']' &&
                        (m->s[m->i + 1] == '3' || m->s[m->i + 1] == '4')) {
                        mu_put(m, m->s[m->i + 1] == '3' ? "\xe2\x88\x9b" : "\xe2\x88\x9c");
                        m->i += 3;
                    } else {
                        m->bad = 1;
                        return;
                    }
                } else {
                    mu_put(m, "\xe2\x88\x9a");
                }
                if (!mu_arg(m, a, sizeof a, &ca)) return;
                ca = strlen(a) > 4 || strpbrk(a, "+-= /") != NULL;
                if (ca) mu_putc(m, '(');
                mu_put(m, a);
                if (ca) mu_putc(m, ')');
                continue;
            }
            if (!strcmp(nm, "mathbb")) {
                /* the common number sets */
                static const char *const bb[][2] = {
                    {"R", "\xe2\x84\x9d"}, {"N", "\xe2\x84\x95"}, {"Z", "\xe2\x84\xa4"},
                    {"Q", "\xe2\x84\x9a"}, {"C", "\xe2\x84\x82"}, {"P", "\xe2\x84\x99"},
                    {"H", "\xe2\x84\x8d"}, {NULL, NULL}};
                char a[16];
                int ca = 0, k;
                if (!mu_arg(m, a, sizeof a, &ca)) return;
                for (k = 0; bb[k][0]; k++)
                    if (!strcmp(bb[k][0], a)) break;
                if (!bb[k][0]) {
                    m->bad = 1;
                    return;
                }
                mu_put(m, bb[k][1]);
                continue;
            }
            if (!strcmp(nm, "mathrm") || !strcmp(nm, "text") || !strcmp(nm, "mathit") ||
                !strcmp(nm, "operatorname") || !strcmp(nm, "textrm") || !strcmp(nm, "mbox") ||
                !strcmp(nm, "mathbf") || !strcmp(nm, "boldsymbol")) {
                char a[256];
                int ca = 0;
                if (!mu_arg(m, a, sizeof a, &ca)) return;
                mu_put(m, a);
                continue;
            }
            for (y = mu_syms; y->name; y++)
                if (strcmp(y->name, nm) == 0) break;
            if (!y->name) {
                m->bad = 1;
                return;
            }
            {
                /* A control word eats the blanks after it (TeX); keep one
                 * where the source had one on both sides ("x \le y"), and
                 * after an operator name before a letter ("\sin x"). */
                int before = m->k > 0 && m->o[m->k - 1] == ' ';
                int after = m->i < end && (m->s[m->i] == ' ' || m->s[m->i] == '\t');
                mu_put(m, y->u);
                mu_skip_ws(m);
                if ((before && after && m->i < end) ||
                    (mu_letter(y->u[0]) && m->i < end &&
                     (mu_letter(m->s[m->i]) || m->s[m->i] == '\\')))
                    mu_putc(m, ' ');
            }
            continue;
        }
        if (c == '\n' || c == '\t') c = ' ';
        {
            size_t j = m->i + 1;
            while (j < end && ((unsigned char)m->s[j] & 0xC0) == 0x80) j++;
            if (m->k + (j - m->i) + 1 > m->cap) {
                m->bad = 1;
                return;
            }
            if (c == ' ') {
                /* TeX drops blanks in math: one at most, only between words */
                if (m->k && m->o[m->k - 1] != ' ') mu_putc(m, ' ');
            } else {
                memcpy(m->o + m->k, m->s + m->i, j - m->i);
                m->k += j - m->i;
            }
            m->i = j;
        }
    }
}

size_t rtx_math_unicode(const char *src, size_t n, char *out, size_t cap) {
    MuSt m;
    if (!src || !out || cap < 2 || !n || n > 512) return 0;
    memset(&m, 0, sizeof m);
    m.s = src;
    m.n = n;
    m.o = out;
    m.cap = cap;
    mu_expr(&m, n, 0);
    if (m.bad || m.i != n) {
        out[0] = 0;
        return 0;
    }
    while (m.k && m.o[m.k - 1] == ' ') m.k--;
    {
        size_t s = 0;
        while (s < m.k && m.o[s] == ' ') s++;
        if (s) {
            memmove(m.o, m.o + s, m.k - s);
            m.k -= s;
        }
    }
    out[m.k] = 0;
    return m.k;
}
