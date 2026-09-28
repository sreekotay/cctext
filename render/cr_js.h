/*
 * cctext-render's JavaScript host (QuickJS, third_party/quickjs;
 * docs/images.md, "Mermaid"). One engine per bundle (Mermaid now; math
 * later), started lazily from bytecode that the build step compiled
 * (cr_js_compile) and the helper read from its pack before lockdown.
 *
 * An engine is a QuickJS runtime + context with no OS bindings (no
 * quickjs-libc: no files, sockets, timers or threads). The bundle's own
 * shims (render/js/rt_shim.js) supply what the libraries need. The host
 * adds three globals: print (stderr), __hostMeasure(text, px, weight,
 * family, italic) and __hostFontMetrics(px, weight, family), both from
 * the bundled faces the rasterizer draws with (cr_svg.h).
 *
 * Budget: an interrupt handler ends a call past its deadline. Recycling:
 * the runtime is freed after a failed or timed-out call, when the heap
 * passes the policy's ceiling, and optionally every N calls; the next
 * call starts a fresh one. The cycle collector runs only after the heap
 * grew by gc_growth since its last run (it walks every object).
 */
#ifndef CR_JS_H
#define CR_JS_H

#include <stddef.h>
#include <stdint.h>

typedef struct CrJs CrJs;

typedef struct {
    size_t heap_max;       /* JS heap limit (allocation fails past it) */
    size_t recycle_heap;   /* restart after a call leaves the heap above this */
    unsigned recycle_jobs; /* ... or after this many calls (0: never) */
    size_t gc_growth;      /* cycle collection after this much heap growth (0: every call) */
} CrJsPolicy;

typedef struct {
    unsigned starts;       /* runtimes created */
    unsigned recycles;     /* runtimes freed by the policy (not counting the first start) */
    unsigned jobs;         /* calls on the live runtime */
    unsigned total_jobs;
    unsigned gcs;
    size_t heap;           /* live JS heap, bytes */
    double last_start_ms;  /* bytecode load + evaluation, last start */
    double gc_ms;          /* cycle collections, total */
} CrJsStats;

/* Build step: compile `src` (a classic script) to bytecode. The result is
 * malloc'd. Debug source text is stripped; line numbers stay. 0 = ok, else
 * a message in err. */
int cr_js_compile(const char *name, const char *src, size_t n, uint8_t **out, size_t *out_n,
                  char *err, size_t errcap);

/* An engine over bytecode bc[0, n) (kept by reference: it must outlive
 * the engine). Nothing starts until the first call. */
CrJs *cr_js_new(const char *name, const uint8_t *bc, size_t n, const CrJsPolicy *policy);

/* Start now (the first call does it otherwise). 0 = ok. */
int cr_js_start(CrJs *e, char *err, size_t errcap);

/* Call the global function `fn` with string arguments; a returned
 * promise is settled by pumping jobs and queued timers. Returns the
 * result as a malloc'd UTF-8 string, or NULL with a message in err
 * (*timed_out set when the budget ended it). */
char *cr_js_call(CrJs *e, const char *fn, int argc, const char *const *argv, uint32_t budget_ms,
                 char *err, size_t errcap, int *timed_out);

/* Source modules a bundle loads on demand (math: MathJax's extensions and
 * dynamic font files): the engine gets a global __hostLoad(key) that asks
 * `get` for the source (UTF-8, NUL-terminated at src[n]; 0 = ok, else a
 * message in err), evaluates it as a global script, then calls `put`
 * (the source may be freed). __hostLoad returns true, false when there is
 * no such module, or throws the script's error. */
typedef int (*CrJsSrcGet)(void *ctx, const char *key, const char **src, size_t *n, char *err,
                          size_t errcap);
typedef void (*CrJsSrcPut)(void *ctx, const char *key);
void cr_js_set_loader(CrJs *e, CrJsSrcGet get, CrJsSrcPut put, void *ctx);

/* Free the runtime now (the next call starts a fresh one). */
void cr_js_stop(CrJs *e);
void cr_js_stats(const CrJs *e, CrJsStats *out);

#endif /* CR_JS_H */
