/*
 * cctext-render's JavaScript host over QuickJS (cr_js.h; docs/images.md,
 * "Mermaid").
 */
#if !defined(_WIN32)
#define _GNU_SOURCE
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "quickjs.h"

#include "cr_js.h"
#include "cr_svg.h"

struct CrJs {
    char name[32];
    const uint8_t *bc;
    size_t bc_n;
    CrJsPolicy pol;
    JSRuntime *rt;
    JSContext *ctx;
    size_t heap;           /* bytes allocated through our malloc functions */
    size_t gc_mark;        /* heap after the last cycle collection */
    double deadline;       /* interrupt: ms on the monotonic clock (0: none) */
    int interrupted;
    CrJsStats st;
};

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
}

/* ---- allocator: QuickJS's accounting with an O(1) heap size ---------- *
 * A 16-byte header holds each block's size, so the live heap is a
 * counter (JS_ComputeMemoryUsage walks every object: ~5 ms on 20 MB) and
 * nothing depends on malloc_usable_size. */
#define CR_HDR 16

static void *js_m(JSMallocState *s, size_t n)
{
    CrJs *e = s->opaque;
    uint8_t *p;
    if (n > s->malloc_limit || s->malloc_size + n > s->malloc_limit) return NULL;
    p = malloc(n + CR_HDR);
    if (!p) return NULL;
    memcpy(p, &n, sizeof n);
    s->malloc_count++;
    s->malloc_size += n + CR_HDR;
    e->heap = s->malloc_size;
    return p + CR_HDR;
}

static void js_f(JSMallocState *s, void *ptr)
{
    CrJs *e = s->opaque;
    uint8_t *p;
    size_t n;
    if (!ptr) return;
    p = (uint8_t *)ptr - CR_HDR;
    memcpy(&n, p, sizeof n);
    s->malloc_count--;
    s->malloc_size -= n + CR_HDR;
    e->heap = s->malloc_size;
    free(p);
}

static void *js_r(JSMallocState *s, void *ptr, size_t n)
{
    CrJs *e = s->opaque;
    uint8_t *p, *q;
    size_t old;
    if (!ptr) return n ? js_m(s, n) : NULL;
    if (!n) {
        js_f(s, ptr);
        return NULL;
    }
    p = (uint8_t *)ptr - CR_HDR;
    memcpy(&old, p, sizeof old);
    if (n > old && s->malloc_size + (n - old) > s->malloc_limit) return NULL;
    q = realloc(p, n + CR_HDR);
    if (!q) return NULL;
    memcpy(q, &n, sizeof n);
    s->malloc_size = s->malloc_size - old + n;
    e->heap = s->malloc_size;
    return q + CR_HDR;
}

static size_t js_u(const void *ptr)
{
    size_t n;
    if (!ptr) return 0;
    memcpy(&n, (const uint8_t *)ptr - CR_HDR, sizeof n);
    return n;
}

static const JSMallocFunctions g_mf = {js_m, js_f, js_r, js_u};

/* ---- host functions ---------------------------------------------------- */

static int interrupt_cb(JSRuntime *rt, void *opaque)
{
    CrJs *e = opaque;
    (void)rt;
    if (e->deadline > 0 && now_ms() > e->deadline) {
        e->interrupted = 1;
        return 1;
    }
    return 0;
}

static JSValue js_print(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    for (int i = 0; i < argc; i++) {
        const char *s = JS_ToCString(ctx, argv[i]);
        if (s) {
            fprintf(stderr, "%s%s", i ? " " : "", s);
            JS_FreeCString(ctx, s);
        }
    }
    fputc('\n', stderr);
    return JS_UNDEFINED;
}

static double arg_num(JSContext *ctx, int argc, JSValueConst *argv, int i, double d)
{
    double v = d;
    if (i < argc && JS_ToFloat64(ctx, &v, argv[i]) != 0) v = d;
    if (!(v == v) || v < 0 || v > 1e6) v = d; /* NaN, negative, absurd */
    return v;
}

/* __hostMeasure(text, px, weight, family, italic) -> advance in px */
static JSValue js_measure(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    const char *s, *fam = NULL;
    size_t n = 0;
    double r = 0;
    (void)this_val;
    if (argc < 1) return JS_NewFloat64(ctx, 0);
    s = JS_ToCStringLen(ctx, &n, argv[0]);
    if (argc > 3) fam = JS_ToCString(ctx, argv[3]);
    if (s)
        r = cr_svg_measure(s, n, arg_num(ctx, argc, argv, 1, 16), (int)arg_num(ctx, argc, argv, 2, 400),
                           argc > 4 ? JS_ToBool(ctx, argv[4]) > 0 : 0, fam ? fam : "sans-serif");
    if (s) JS_FreeCString(ctx, s);
    if (fam) JS_FreeCString(ctx, fam);
    return JS_NewFloat64(ctx, r);
}

/* __hostFontMetrics(px, weight, family) -> [ascent, descent] in px */
static JSValue js_metrics(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    const char *fam = argc > 2 ? JS_ToCString(ctx, argv[2]) : NULL;
    double a = 0, d = 0;
    JSValue r;
    (void)this_val;
    cr_svg_font_metrics(arg_num(ctx, argc, argv, 0, 16), (int)arg_num(ctx, argc, argv, 1, 400), 0,
                        fam ? fam : "sans-serif", &a, &d);
    if (fam) JS_FreeCString(ctx, fam);
    r = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, r, 0, JS_NewFloat64(ctx, a));
    JS_SetPropertyUint32(ctx, r, 1, JS_NewFloat64(ctx, d));
    return r;
}

static void exc_text(JSContext *ctx, char *out, size_t cap)
{
    JSValue ex = JS_GetException(ctx);
    const char *s = JS_ToCString(ctx, ex);
    snprintf(out, cap, "%s", s ? s : "exception");
    if (s) JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, ex);
}

/* ---- build step ---------------------------------------------------------- */

int cr_js_compile(const char *name, const char *src, size_t n, uint8_t **out, size_t *out_n,
                  char *err, size_t errcap)
{
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx;
    JSValue fn;
    uint8_t *bc;
    size_t bn = 0;
    int rc = 1;
    *out = NULL;
    *out_n = 0;
    if (!rt) {
        snprintf(err, errcap, "no memory");
        return 1;
    }
    JS_SetMaxStackSize(rt, 64u << 20);
    JS_SetStripInfo(rt, JS_STRIP_SOURCE); /* keep line numbers, drop the source text */
    ctx = JS_NewContext(rt);
    fn = JS_Eval(ctx, src, n, name, JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(fn)) {
        exc_text(ctx, err, errcap);
    } else {
        bc = JS_WriteObject(ctx, &bn, fn, JS_WRITE_OBJ_BYTECODE);
        if (bc) {
            *out = malloc(bn ? bn : 1);
            if (*out) {
                memcpy(*out, bc, bn);
                *out_n = bn;
                rc = 0;
            }
            js_free(ctx, bc);
        }
        if (rc) snprintf(err, errcap, "cannot write bytecode");
    }
    JS_FreeValue(ctx, fn);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return rc;
}

/* ---- engines ------------------------------------------------------------- */

CrJs *cr_js_new(const char *name, const uint8_t *bc, size_t n, const CrJsPolicy *policy)
{
    CrJs *e = calloc(1, sizeof *e);
    if (!e) return NULL;
    snprintf(e->name, sizeof e->name, "%s", name);
    e->bc = bc;
    e->bc_n = n;
    e->pol = *policy;
    return e;
}

void cr_js_stop(CrJs *e)
{
    if (!e || !e->rt) return;
    JS_FreeContext(e->ctx);
    JS_FreeRuntime(e->rt);
    e->ctx = NULL;
    e->rt = NULL;
    e->heap = 0;
    e->st.jobs = 0;
}

int cr_js_start(CrJs *e, char *err, size_t errcap)
{
    double t0 = now_ms();
    JSValue g, fn, r;
    if (e->rt) return 0;
    e->heap = 0;
    e->rt = JS_NewRuntime2(&g_mf, e);
    if (!e->rt) {
        snprintf(err, errcap, "%s: no memory for a runtime", e->name);
        return -1;
    }
    JS_SetMemoryLimit(e->rt, e->pol.heap_max);
    JS_SetMaxStackSize(e->rt, 4u << 20);
    JS_SetInterruptHandler(e->rt, interrupt_cb, e);
    e->ctx = JS_NewContext(e->rt);
    if (!e->ctx) {
        JS_FreeRuntime(e->rt);
        e->rt = NULL;
        snprintf(err, errcap, "%s: no memory for a context", e->name);
        return -1;
    }
    g = JS_GetGlobalObject(e->ctx);
    JS_SetPropertyStr(e->ctx, g, "print", JS_NewCFunction(e->ctx, js_print, "print", 1));
    JS_SetPropertyStr(e->ctx, g, "__hostMeasure",
                      JS_NewCFunction(e->ctx, js_measure, "__hostMeasure", 5));
    JS_SetPropertyStr(e->ctx, g, "__hostFontMetrics",
                      JS_NewCFunction(e->ctx, js_metrics, "__hostFontMetrics", 3));
    JS_FreeValue(e->ctx, g);
    /* The bytecode came from this build's own pack, checked against the
     * hash compiled into the helper (cctext-render.c) before this call:
     * JS_ReadObject is not hardened against hostile input. */
    fn = JS_ReadObject(e->ctx, e->bc, e->bc_n, JS_READ_OBJ_BYTECODE);
    r = JS_IsException(fn) ? fn : JS_EvalFunction(e->ctx, fn);
    if (JS_IsException(r)) {
        char m[400];
        exc_text(e->ctx, m, sizeof m);
        snprintf(err, errcap, "%s: engine init failed: %s", e->name, m);
        cr_js_stop(e);
        return -1;
    }
    JS_FreeValue(e->ctx, r);
    e->st.jobs = 0;
    e->st.starts++;
    e->gc_mark = e->heap;
    e->st.last_start_ms = now_ms() - t0;
    return 0;
}

/* Settle promise `r` (owned): run pending jobs, then queued timers, until
 * it resolves or rejects, nothing is left to run, or the budget ends. */
static JSValue settle(CrJs *e, JSValue r, JSValue drain, char *err, size_t errcap, int *failed)
{
    JSContext *ctx = e->ctx;
    *failed = 0;
    for (;;) {
        JSContext *c1;
        int k, ran = 0;
        JSPromiseStateEnum st;
        while ((k = JS_ExecutePendingJob(e->rt, &c1)) > 0) ran = 1;
        if (k < 0) {
            exc_text(ctx, err, errcap);
            *failed = 1;
            return r;
        }
        st = JS_PromiseState(ctx, r);
        if (st == JS_PROMISE_FULFILLED) {
            JSValue v = JS_PromiseResult(ctx, r);
            JS_FreeValue(ctx, r);
            return v;
        }
        if (st == JS_PROMISE_REJECTED) {
            JSValue v = JS_PromiseResult(ctx, r);
            const char *s = JS_ToCString(ctx, v);
            snprintf(err, errcap, "%s", s ? s : "rejected");
            if (s) JS_FreeCString(ctx, s);
            JS_FreeValue(ctx, v);
            *failed = 1;
            return r;
        }
        if (JS_IsFunction(ctx, drain)) {
            JSValue d = JS_Call(ctx, drain, JS_UNDEFINED, 0, NULL);
            int32_t n = 0;
            if (JS_IsException(d)) {
                exc_text(ctx, err, errcap);
                *failed = 1;
                return r;
            }
            JS_ToInt32(ctx, &n, d);
            JS_FreeValue(ctx, d);
            if (n > 0) ran = 1;
        }
        if (e->interrupted || (e->deadline > 0 && now_ms() > e->deadline)) {
            e->interrupted = 1;
            snprintf(err, errcap, "timeout");
            *failed = 1;
            return r;
        }
        if (!ran) {
            snprintf(err, errcap, "the render never finished (a promise nothing settles)");
            *failed = 1;
            return r;
        }
    }
}

char *cr_js_call(CrJs *e, const char *fn, int argc, const char *const *argv, uint32_t budget_ms,
                 char *err, size_t errcap, int *timed_out)
{
    JSContext *ctx;
    JSValue g, f, drain, r, args[8];
    char *out = NULL;
    int failed = 0;
    size_t mem;
    *timed_out = 0;
    err[0] = 0;
    if (argc > 8) argc = 8;
    if (cr_js_start(e, err, errcap) != 0) return NULL;
    ctx = e->ctx;
    e->interrupted = 0;
    e->deadline = budget_ms ? now_ms() + budget_ms : 0;
    for (int i = 0; i < argc; i++) args[i] = JS_NewString(ctx, argv[i]);
    g = JS_GetGlobalObject(ctx);
    f = JS_GetPropertyStr(ctx, g, fn);
    drain = JS_GetPropertyStr(ctx, g, "__drainTimers");
    r = JS_Call(ctx, f, JS_UNDEFINED, argc, (JSValueConst *)args);
    if (JS_IsException(r)) {
        exc_text(ctx, err, errcap);
        failed = 1;
    } else if (JS_IsObject(r) && JS_PromiseState(ctx, r) != (JSPromiseStateEnum)-1) {
        r = settle(e, r, drain, err, errcap, &failed);
    }
    if (!failed) {
        size_t n = 0;
        const char *s = JS_ToCStringLen(ctx, &n, r);
        if (s) {
            out = malloc(n + 1);
            if (out) {
                memcpy(out, s, n);
                out[n] = 0;
            } else {
                snprintf(err, errcap, "out of memory");
            }
            JS_FreeCString(ctx, s);
        } else {
            exc_text(ctx, err, errcap);
        }
    }
    if (e->interrupted) {
        *timed_out = 1;
        snprintf(err, errcap, "timeout");
    }
    e->deadline = 0;
    for (int i = 0; i < argc; i++) JS_FreeValue(ctx, args[i]);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, f);
    JS_FreeValue(ctx, drain);
    JS_FreeValue(ctx, g);
    e->st.jobs++;
    e->st.total_jobs++;
    /* A failed or interrupted call may leave the realm in any state
     * (half-built globals, a caught interrupt inside a library): start
     * over. Heap growth and the job count bound slow leaks. */
    mem = e->heap;
    if (!out || *timed_out || mem > e->pol.recycle_heap ||
        (e->pol.recycle_jobs && e->st.jobs >= e->pol.recycle_jobs)) {
        cr_js_stop(e);
        e->st.recycles++;
        return out;
    }
    if (!e->pol.gc_growth || mem > e->gc_mark + e->pol.gc_growth) {
        double t = now_ms();
        JS_RunGC(e->rt);
        e->gc_mark = e->heap;
        e->st.gcs++;
        e->st.gc_ms += now_ms() - t;
    }
    return out;
}

void cr_js_stats(const CrJs *e, CrJsStats *out)
{
    *out = e->st;
    out->heap = e->heap;
}
