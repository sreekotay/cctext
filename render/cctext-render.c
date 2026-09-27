/*
 * cctext-render: the sandboxed renderer helper (docs/images.md,
 * "Renderer"). Step 1 of the renderer plan: SVG through lunasvg. The
 * protocol and the pack already hold the later kinds (math, Mermaid).
 *
 *   cctext-render [--pack PATH] [--no-sandbox]    serve requests on stdin / stdout
 *   cctext-render --build-pack MANIFEST OUT       build step: fonts -> zlib pack
 *   cctext-render --png IN.svg OUT.png [SCALE]    development: render a file
 *   cctext-render --version
 *
 * Startup: close every fd above 2, read the pack, inflate and register the
 * fonts, warm the engine once (lazy statics initialise before lockdown),
 * then lock down (sandbox_*.c) and say hello. After lockdown the process
 * reads stdin, writes stdout / stderr and allocates memory; nothing else.
 * The editor (core/img_svg.c) enforces the time budget: it kills and
 * respawns a helper that overruns.
 *
 * Built with -DCR_SELFTEST (bin/cctext-render-selftest, tests only), the
 * environment variable CR_SELFTEST names an escape to attempt right after
 * lockdown (socket, exec, fork, mmapx, readfd, thread, open), and a payload
 * that starts with "<!--cr-selftest:hang-->" / ":crash-->" / ":slow-->" /
 * ":slowpx-->" hangs, crashes, answers after 300 ms, or sends pixels after
 * 1.5 s. The release build has none of it.
 */
#if !defined(_WIN32)
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#define cr_read _read
#define cr_write _write
#else
#include <unistd.h>
#define cr_read read
#define cr_write write
#endif
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#include "cr_pack.h"
#include "cr_proto.h"
#include "cr_svg.h"

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "cctext-render: the wire protocol is little-endian"
#endif

int cr_sandbox_lockdown(size_t mem_limit, unsigned cpu_seconds);

#define CR_VERSION "cctext-render 1 (lunasvg 3.5.0 cf3594d + cctext patches, plutovg 1.3.3)"
/* Address space and CPU seconds for the whole process life. The editor's
 * per-request time budget is far tighter; RLIMIT_CPU only ends a helper
 * that has worked for this long in total (the editor respawns it). */
#define CR_MEM_LIMIT ((size_t)2 << 30)
#define CR_CPU_SECONDS 600u

static int g_disabled;   /* no lockdown on this platform: refuse everything */

/* ---- I/O -------------------------------------------------------------- */

static int read_full(void *p, size_t n)
{
    uint8_t *b = p;
    while (n) {
        long r = (long)cr_read(0, b, (unsigned)(n > (1u << 30) ? (1u << 30) : n));
        if (r <= 0) {
            if (r < 0 && errno == EINTR) continue;
            return -1;
        }
        b += r;
        n -= (size_t)r;
    }
    return 0;
}

static int write_full(const void *p, size_t n)
{
    const uint8_t *b = p;
    while (n) {
        long r = (long)cr_write(1, b, (unsigned)(n > (1u << 30) ? (1u << 30) : n));
        if (r <= 0) {
            if (r < 0 && errno == EINTR) continue;
            return -1;
        }
        b += r;
        n -= (size_t)r;
    }
    return 0;
}

static void send_reply(uint32_t id, uint32_t type, const void *a, size_t an, const void *b,
                       size_t bn)
{
    CrReply h = {CR_MAGIC_REP, id, type, (uint32_t)(an + bn)};
    if (write_full(&h, sizeof h) || (an && write_full(a, an)) || (bn && write_full(b, bn)))
        exit(0); /* the editor went away */
}

static void send_error(uint32_t id, uint32_t code, const char *msg)
{
    send_reply(id, CR_R_ERROR, &code, 4, msg, strlen(msg));
}

/* ---- the pack ----------------------------------------------------------- */

static CrPack g_pack;

static uint8_t *slurp(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    long len;
    uint8_t *b;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (len = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0 ||
        len > (256l << 20)) {
        fclose(f);
        return NULL;
    }
    b = malloc((size_t)len + 1);
    if (b && fread(b, 1, (size_t)len, f) != (size_t)len) {
        free(b);
        b = NULL;
    }
    fclose(f);
    if (b) *n = (size_t)len;
    return b;
}

/* <dir of this executable>/cctext-render.pack */
static void default_pack(char *out, size_t n)
{
    char exe[3072];
    size_t len = 0;
    snprintf(out, n, "cctext-render.pack");
#if defined(__linux__)
    {
        long r = (long)readlink("/proc/self/exe", exe, sizeof exe - 1);
        if (r <= 0) return;
        len = (size_t)r;
        exe[len] = 0;
    }
#elif defined(__APPLE__)
    {
        uint32_t sz = sizeof exe;
        if (_NSGetExecutablePath(exe, &sz) != 0) return;
        len = strlen(exe);
    }
#else
    (void)exe;
    return;
#endif
    while (len && exe[len - 1] != '/') len--;
    if (!len) return;
    exe[len] = 0;
    snprintf(out, n, "%scctext-render.pack", exe);
}

/* Entries "font:<family>:<bold>:<italic>". */
static int fonts_init(void)
{
    int nf = 0;
    for (int i = 0; i < g_pack.n; i++) {
        CrAsset *a = &g_pack.e[i];
        char fam[256];
        int bold = 0, italic = 0;
        const char *p, *c1;
        const uint8_t *d;
        if (strncmp(a->name, "font:", 5) != 0) continue;
        p = a->name + 5;
        c1 = strchr(p, ':');
        if (!c1 || (size_t)(c1 - p) >= sizeof fam) continue;
        memcpy(fam, p, (size_t)(c1 - p));
        fam[c1 - p] = 0;
        if (sscanf(c1 + 1, "%d:%d", &bold, &italic) != 2) continue;
        d = cr_asset_data(a);
        if (d && cr_svg_add_font(fam, bold, italic, d, a->raw_len) == 0) nf++;
    }
    return nf;
}

/* Parse + draw once before lockdown: font-cache sorting and any other lazy
 * first-use work happens while the process may still do anything. */
static void warm_up(void)
{
    static const char svg[] =
        "<svg xmlns='http://www.w3.org/2000/svg' width='8' height='8'>"
        "<rect width='8' height='8' fill='#888' stroke='#000' stroke-dasharray='1'/>"
        "<text x='1' y='7' font-family='serif' font-weight='bold' font-size='6'>A</text>"
        "</svg>";
    float w = 0, h = 0;
    uint8_t px[8 * 8 * 4];
    cr_svg *d = cr_svg_parse(svg, sizeof svg - 1, &w, &h);
    if (d) {
        cr_svg_render(d, 1, 1, 8, 8, 0, px);
        cr_svg_free(d);
    }
}

/* ---- requests ----------------------------------------------------------- */

#ifdef CR_SELFTEST
static void selftest_spin(long ms)
{
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    do clock_gettime(CLOCK_MONOTONIC, &t1);
    while ((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000 < ms);
}

/* ":slowpx-->" delays only pixel requests (1.5 s): a size arrives, then
 * the pixels much later (ui_svg_test: layout stable across the gap). */
static void selftest_payload(const char *src, size_t n, int size_only)
{
    static const char pre[] = "<!--cr-selftest:";
    if (n < sizeof pre - 1 || memcmp(src, pre, sizeof pre - 1) != 0) return;
    src += sizeof pre - 1;
    if (strncmp(src, "hang-->", 7) == 0)
        for (volatile unsigned long i = 0;; i++) {
        }
    if (strncmp(src, "crash-->", 8) == 0) abort();
    if (strncmp(src, "slow-->", 7) == 0) selftest_spin(300);
    if (strncmp(src, "slowpx-->", 9) == 0 && !size_only) selftest_spin(1500);
}
#endif

static void handle_svg(const CrReq *q, const char *src)
{
    float w = 0, h = 0, sx, sy;
    uint64_t cap = q->max_px && q->max_px < CR_PIXELS_HARD_MAX ? q->max_px : CR_PIXELS_HARD_MAX;
    uint32_t pw, ph;
    CrSize sz;
    cr_svg *doc = cr_svg_parse(src, q->len, &w, &h);
    if (!doc) {
        send_error(q->id, CR_E_PARSE, "not an SVG document this renderer reads");
        return;
    }
    if (!(w > 0) || !(h > 0) || !isfinite(w) || !isfinite(h) || w > 1e7f || h > 1e7f) {
        cr_svg_free(doc);
        send_error(q->id, CR_E_EMPTY, "no size (width / height / viewBox)");
        return;
    }
    if (q->box_w && q->box_h) {
        pw = q->box_w;
        ph = q->box_h;
        sx = (float)pw / w;
        sy = (float)ph / h;
    } else {
        float s = q->scale > 0 && q->scale <= 64 && isfinite(q->scale) ? q->scale : 1.0f;
        double fw = ceil((double)w * s), fh = ceil((double)h * s);
        sx = sy = s;
        pw = fw < 1 ? 1 : fw > 4e9 ? 0xffffffffu : (uint32_t)fw;
        ph = fh < 1 ? 1 : fh > 4e9 ? 0xffffffffu : (uint32_t)fh;
    }
    sz.w_css = w;
    sz.h_css = h;
    sz.baseline_css = 0;
    sz.px_w = pw;
    sz.px_h = ph;
    if (q->flags & CR_F_SIZE_ONLY) {
        cr_svg_free(doc);
        send_reply(q->id, CR_R_SIZE, &sz, sizeof sz, NULL, 0);
        return;
    }
    if (pw > CR_SIDE_HARD_MAX || ph > CR_SIDE_HARD_MAX || (uint64_t)pw * ph > cap) {
        cr_svg_free(doc);
        send_reply(q->id, CR_R_SIZE, &sz, sizeof sz, NULL, 0);
        send_error(q->id, CR_E_PIXELS, "output over the pixel limit");
        return;
    }
    send_reply(q->id, CR_R_SIZE, &sz, sizeof sz, NULL, 0);
    {
        size_t nb = (size_t)pw * ph * 4;
        uint8_t *buf = malloc(nb);
        uint32_t hdr[3] = {pw, ph, pw * 4};
        if (!buf) {
            cr_svg_free(doc);
            send_error(q->id, CR_E_NOMEM, "out of memory");
            return;
        }
        cr_svg_render(doc, sx, sy, pw, ph, q->bg, buf);
        cr_svg_free(doc);
        send_reply(q->id, CR_R_PIXELS, hdr, sizeof hdr, buf, nb);
        free(buf);
    }
}

static void serve(int sandboxed)
{
    uint32_t hello[3] = {CR_PROTO_VERSION, g_disabled ? 0u : (1u << CR_KIND_SVG),
                         (sandboxed ? CR_H_SANDBOXED : 0u) | (g_disabled ? CR_H_DISABLED : 0u)};
    send_reply(0, CR_R_HELLO, hello, sizeof hello, NULL, 0);
    for (;;) {
        CrReq q;
        char *src;
        if (read_full(&q, sizeof q) != 0) exit(0);
        if (q.magic != CR_MAGIC_REQ) {
            fprintf(stderr, "cctext-render: bad frame magic 0x%08x; exiting\n", q.magic);
            exit(3);
        }
        if (q.kind == CR_KIND_QUIT) exit(0);
        if (q.len > CR_INPUT_HARD_MAX) {
            fprintf(stderr, "cctext-render: frame of %u bytes over the %u limit; exiting\n", q.len,
                    CR_INPUT_HARD_MAX);
            exit(3);
        }
        src = malloc((size_t)q.len + 1);
        if (!src) exit(4);
        if (read_full(src, q.len) != 0) exit(0);
        src[q.len] = 0;
#ifdef CR_SELFTEST
        selftest_payload(src, q.len, (q.flags & CR_F_SIZE_ONLY) != 0);
#endif
        if (g_disabled)
            send_error(q.id, CR_E_DISABLED, "renderer disabled on this platform (no sandbox)");
        else if (q.kind == CR_KIND_SVG)
            handle_svg(&q, src);
        else
            send_error(q.id, CR_E_KIND, "kind not in this renderer");
        free(src);
    }
}

#ifdef CR_SELFTEST
#include <sys/mman.h>
static int selftest(const char *t)
{
    /* The policy: a file open fails soft, then the named escape must kill
     * the process (SIGSYS) before "survived" prints. */
    FILE *f = fopen("/etc/passwd", "r");
    fprintf(stderr, "[selftest] fopen(/etc/passwd) -> %s (errno %d)\n", f ? "OPENED" : "denied",
            f ? 0 : errno);
    if (!strcmp(t, "open")) return f ? 1 : (errno == EACCES ? 0 : 2);
    if (!strcmp(t, "socket")) syscall(SYS_socket, 2, 1, 0);
    if (!strcmp(t, "exec")) {
        char *a[] = {"/bin/true", NULL};
        execve(a[0], a, NULL);
    }
    if (!strcmp(t, "fork")) (void)fork();
    if (!strcmp(t, "mmapx")) (void)mmap(0, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (!strcmp(t, "readfd")) {
        char c;
        if (read(3, &c, 1) < 0) fprintf(stderr, "[selftest] read(3) failed\n");
    }
    if (!strcmp(t, "thread")) syscall(SYS_clone, 0x50f00, 0, 0, 0, 0);
    fprintf(stderr, "[selftest] %s survived\n", t);
    return 9;
}
#endif

static void close_extra_fds(void)
{
#if defined(__linux__)
#ifdef SYS_close_range
    if (syscall(SYS_close_range, 3u, ~0u, 0u) == 0) return;
#endif
    for (int fd = 3; fd < 1024; fd++) (void)close(fd);
#elif defined(__APPLE__)
    for (int fd = 3; fd < 1024; fd++) (void)close(fd);
#endif
}

int main(int argc, char **argv)
{
    char pack_path[4096];
    const char *pack = NULL;
    int sandbox = 1;
    size_t pn = 0;
    uint8_t *pb;
#if defined(_WIN32)
    _setmode(0, _O_BINARY);
    _setmode(1, _O_BINARY);
#endif
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--build-pack") && i + 2 < argc)
            return cr_pack_build(argv[i + 1], argv[i + 2]);
        if (!strcmp(argv[i], "--version")) {
            printf("%s\n", CR_VERSION);
            return 0;
        }
        if (!strcmp(argv[i], "--no-sandbox")) sandbox = 0;
        else if (!strcmp(argv[i], "--pack") && i + 1 < argc) pack = argv[++i];
        else if (!strcmp(argv[i], "--png") && i + 2 < argc) {
            /* development only: loads fonts, renders a file, no sandbox */
            const char *in = argv[i + 1], *out = argv[i + 2];
            float s = i + 3 < argc ? (float)atof(argv[i + 3]) : 1.0f;
            default_pack(pack_path, sizeof pack_path);
            pb = slurp(pack ? pack : pack_path, &pn);
            if (!pb || cr_pack_open(&g_pack, pb, pn) != 0) {
                fprintf(stderr, "cctext-render: cannot read the pack\n");
                return 2;
            }
            fonts_init();
            if (cr_svg_png(in, out, s > 0 ? s : 1.0f) != 0) {
                fprintf(stderr, "cctext-render: cannot render %s\n", in);
                return 1;
            }
            return 0;
        } else {
            fprintf(stderr, "usage: cctext-render [--pack PATH] | --build-pack MANIFEST OUT | "
                            "--png IN OUT [SCALE] | --version\n");
            return 2;
        }
    }
    close_extra_fds();
    if (!pack) {
        default_pack(pack_path, sizeof pack_path);
        pack = pack_path;
    }
    pb = slurp(pack, &pn); /* kept: font faces point into the inflated copies */
    if (!pb || cr_pack_open(&g_pack, pb, pn) != 0) {
        fprintf(stderr, "cctext-render: cannot read pack %s\n", pack);
        return 2;
    }
    if (fonts_init() < 1) {
        fprintf(stderr, "cctext-render: no fonts in %s\n", pack);
        return 2;
    }
    warm_up();
    if (sandbox) {
        if (cr_sandbox_lockdown(CR_MEM_LIMIT, CR_CPU_SECONDS) != 0) {
#if defined(__linux__) || defined(__APPLE__)
            fprintf(stderr, "cctext-render: lockdown failed; refusing to run\n");
            return 2;
#else
            g_disabled = 1; /* no sandbox here: say so and refuse to render */
            sandbox = 0;
#endif
        }
    }
#ifdef CR_SELFTEST
    if (getenv("CR_SELFTEST") && getenv("CR_SELFTEST")[0]) return selftest(getenv("CR_SELFTEST"));
#endif
    serve(sandbox);
    return 0;
}
