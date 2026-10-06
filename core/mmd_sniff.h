/* Mermaid source files (docs/images.md "Mermaid files").
 *
 * `.mermaid` is always a Mermaid diagram. `.mmd` is ambiguous: Mermaid
 * Live saves diagrams as `.mmd`, and so does MultiMarkdown. The document
 * decides by its first bytes (core/document.ccs rtx_doc_tm): blank lines,
 * `%%` comments / `%%{init}%%` directives and a `---` front-matter block
 * are skipped, and when the first word is a Mermaid diagram keyword (the
 * detector list of Mermaid 12, third_party/mermaid) it is Mermaid, else
 * Markdown. One bounded read, never a scan. */
#ifndef RTX_MMD_SNIFF_H
#define RTX_MMD_SNIFF_H

#include <stddef.h>

/* Bytes the sniff reads at most (a front matter longer than this leaves
 * the decision to the rest of the rules: not Mermaid). */
#define RTX_MMD_PROBE 4096

enum {
    RTX_MMD_UNSURE = 0,    /* nothing to go by yet (empty, only comments or
                            * blank lines, or a first word still being typed
                            * that begins a keyword): keep the last answer */
    RTX_MMD_MERMAID = 1,
    RTX_MMD_MARKDOWN = 2
};

/* b[0, n): the document's first bytes (at most RTX_MMD_PROBE); `whole`:
 * that is the whole document (a word running to its end may still grow). */
int rtx_mmd_sniff(const char *b, size_t n, int whole);

/* 1 when w[0, n) is a word Mermaid 12 opens a diagram with (graph,
 * flowchart, sequenceDiagram, stateDiagram-v2, xychart-beta, ...). */
int rtx_mmd_keyword(const char *w, size_t n);

/* 1 when `path`'s file name ends in `.mmd` (any case): the ambiguous one. */
int rtx_mmd_path_ambiguous(const char *path);

#endif
