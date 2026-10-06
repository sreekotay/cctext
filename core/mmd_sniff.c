/* Mermaid or MultiMarkdown: the `.mmd` sniff (mmd_sniff.h). */
#include "mmd_sniff.h"

#include <string.h>

/* Mermaid 12.0.0's diagram detectors (third_party/mermaid/mermaid.min.js,
 * each `detector: t => /^\s*word/.test(t)`), plus zenuml (an external
 * diagram Mermaid knows by name) and the documented spellings of the
 * beta ones. A word, whole: `graph` opens a diagram, `graphs` does not. */
static const char *const mmd_kw[] = {
    "graph", "flowchart", "flowchart-elk", "sequenceDiagram", "classDiagram",
    "classDiagram-v2", "stateDiagram", "stateDiagram-v2", "erDiagram", "journey",
    "gantt", "pie", "quadrantChart", "requirementDiagram", "requirement", "gitGraph",
    "C4Context", "C4Container", "C4Component", "C4Dynamic", "C4Deployment", "mindmap",
    "timeline", "zenuml", "sankey", "sankey-beta", "xychart", "xychart-beta", "block",
    "block-beta", "packet", "packet-beta", "kanban", "architecture", "architecture-beta",
    "radar", "radar-beta", "treemap", "treemap-beta", "info", "agentflow-beta",
    "swimlane-beta", "eventmodeling", "venn-beta", "usecase-beta", "cynefin-beta",
    "treeView-beta", NULL};

/* The ones Mermaid matches in any case (`/i` detectors). */
static const char *const mmd_kw_i[] = {"ishikawa", "ishikawa-beta", "wardley-beta",
                                       "railroad-beta", "railroad-ebnf-beta",
                                       "railroad-abnf-beta", "railroad-peg-beta", NULL};

static int mmd_lower(int c) {
    return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c;
}

/* 1: equal; with `prefix`, w is a proper-or-equal prefix of k. */
static int mmd_cmp(const char *w, size_t n, const char *k, int icase, int prefix) {
    size_t i, kn = strlen(k);
    if (prefix ? n > kn : n != kn) return 0;
    for (i = 0; i < n; i++) {
        int a = (unsigned char)w[i], b = (unsigned char)k[i];
        if (icase) {
            a = mmd_lower(a);
            b = mmd_lower(b);
        }
        if (a != b) return 0;
    }
    return 1;
}

static int mmd_find(const char *w, size_t n, int prefix) {
    int i;
    if (!w || !n) return 0;
    for (i = 0; mmd_kw[i]; i++)
        if (mmd_cmp(w, n, mmd_kw[i], 0, prefix)) return 1;
    for (i = 0; mmd_kw_i[i]; i++)
        if (mmd_cmp(w, n, mmd_kw_i[i], 1, prefix)) return 1;
    return 0;
}

int rtx_mmd_keyword(const char *w, size_t n) {
    return mmd_find(w, n, 0);
}

static int mmd_word_ch(int c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '-';
}

/* The end of the line at i (the '\n' or n). */
static size_t mmd_eol(const char *b, size_t n, size_t i) {
    const char *nl = i < n ? (const char *)memchr(b + i, '\n', n - i) : NULL;
    return nl ? (size_t)(nl - b) : n;
}

/* Line [i, e) is `---` (trailing blanks allowed). */
static int mmd_dashes(const char *b, size_t i, size_t e) {
    size_t k = i;
    while (k < e && b[k] == '-') k++;
    if (k - i != 3) return 0;
    while (k < e && (b[k] == ' ' || b[k] == '\t' || b[k] == '\r')) k++;
    return k == e;
}

int rtx_mmd_sniff(const char *b, size_t n, int whole) {
    size_t i = 0;
    int front = 0; /* a front-matter block was skipped: a second `---` is a word */
    if (!b) return RTX_MMD_UNSURE;
    if (n > RTX_MMD_PROBE) {
        n = RTX_MMD_PROBE;
        whole = 0;
    }
    if (n >= 3 && (unsigned char)b[0] == 0xEF && (unsigned char)b[1] == 0xBB &&
        (unsigned char)b[2] == 0xBF)
        i = 3;
    while (i < n) {
        size_t e = mmd_eol(b, n, i), k = i, w;
        while (k < e && (b[k] == ' ' || b[k] == '\t' || b[k] == '\r' || b[k] == '\f')) k++;
        if (k == e) {
            /* A blank line (or the probe's last, partial one). */
            i = e + 1;
            continue;
        }
        if (e - k >= 2 && b[k] == '%' && b[k + 1] == '%') {
            /* A comment or a %%{init: ...}%% directive. */
            if (e == n && !whole) return RTX_MMD_MARKDOWN;
            i = e + 1;
            continue;
        }
        if (!front && k == i && mmd_dashes(b, i, e)) {
            /* Front matter (Mermaid's config / title, or MultiMarkdown's
             * metadata): up to the closing `---`. */
            size_t j = e + 1;
            front = 1;
            for (;;) {
                size_t je;
                if (j >= n) return whole ? RTX_MMD_UNSURE : RTX_MMD_MARKDOWN;
                je = mmd_eol(b, n, j);
                if (je == n && !whole) return RTX_MMD_MARKDOWN;
                if (mmd_dashes(b, j, je)) break;
                j = je + 1;
            }
            i = mmd_eol(b, n, j) + 1;
            continue;
        }
        /* The first word decides. */
        w = k;
        while (w < e && mmd_word_ch((unsigned char)b[w])) w++;
        if (w == k) return RTX_MMD_MARKDOWN;
        if (mmd_find(b + k, w - k, 0)) return RTX_MMD_MERMAID;
        /* A word still being typed at the end of the document that begins
         * a keyword (`seq`, `flowch`) is neither yet. */
        if (w == n && whole && mmd_find(b + k, w - k, 1)) return RTX_MMD_UNSURE;
        return RTX_MMD_MARKDOWN;
    }
    return whole ? RTX_MMD_UNSURE : RTX_MMD_MARKDOWN;
}

int rtx_mmd_path_ambiguous(const char *path) {
    const char *base, *dot, *bs;
    if (!path) return 0;
    base = strrchr(path, '/');
    base = base ? base + 1 : path;
    bs = strrchr(base, '\\');
    if (bs) base = bs + 1;
    dot = strrchr(base, '.');
    return dot && mmd_cmp(dot + 1, strlen(dot + 1), "mmd", 1, 0);
}
