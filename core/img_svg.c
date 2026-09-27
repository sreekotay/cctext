/*
 * SVG through the sandboxed renderer helper (img_svg.h; docs/images.md,
 * "Renderer"): the helper process, its protocol (render/cr_proto.h), and
 * the content-hash size / pixel cache.
 */
#if !defined(_WIN32)
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "img_svg.h"
#include "../render/cr_proto.h"

#if defined(_WIN32)

/* No sandbox launcher on Windows yet (AppContainer): no helper, so every
 * SVG is a placeholder ("SVG renderer not available on this platform"). */
int rtx_svg_size(const uint8_t *b, size_t n, const RtxSvgCfg *cfg, const _Atomic int *cancel,
                 float *w, float *h) {
    (void)b; (void)n; (void)cfg; (void)cancel; (void)w; (void)h;
    return RTX_SVG_ENONE;
}
int rtx_svg_render(const uint8_t *b, size_t n, const RtxSvgCfg *cfg, uint32_t pw, uint32_t ph,
                   const _Atomic int *cancel, uint8_t **out) {
    (void)b; (void)n; (void)cfg; (void)pw; (void)ph; (void)cancel;
    *out = NULL;
    return RTX_SVG_ENONE;
}
void rtx_svg_shutdown(void) {}
const char *rtx_svg_helper_path(void) { return ""; }
void rtx_svg_stats(RtxSvgStats *o) { memset(o, 0, sizeof *o); }
void rtx_svg_forget_memory(void) {}

#else /* POSIX */

#include <dirent.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

/* ---- sniff ------------------------------------------------------------- */

static int svg_ws(uint8_t c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

int rtx_svg_sniff(const uint8_t *b, size_t n) {
    size_t i = 0;
    if (!b) return 0;
    if (n >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF) i = 3;
    /* Prolog: <?xml ...?>, comments, <!DOCTYPE ...>, blanks; then <svg. */
    while (i < n) {
        while (i < n && svg_ws(b[i])) i++;
        if (i + 4 > n || b[i] != '<') return 0;
        if (b[i + 1] == '?' || b[i + 1] == '!') {
            const char *end = b[i + 1] == '?' ? "?>" : ">";
            size_t k = i + 2;
            if (i + 4 <= n && memcmp(b + i, "<!--", 4) == 0) end = "-->";
            else if (i + 9 <= n && memcmp(b + i, "<!DOCTYPE", 9) == 0) {
                /* an internal subset [ ... ] may hold '>' */
                int br = 0;
                for (; k < n; k++) {
                    if (b[k] == '[') br++;
                    else if (b[k] == ']') br--;
                    else if (b[k] == '>' && br <= 0) break;
                }
                if (k >= n) return 0;
                i = k + 1;
                continue;
            }
            {
                size_t el = strlen(end);
                for (; k + el <= n; k++)
                    if (memcmp(b + k, end, el) == 0) break;
                if (k + el > n) return 0;
                i = k + el;
            }
            continue;
        }
        /* <svg or <svg:svg (a prefixed root) */
        if (n - i >= 5 && memcmp(b + i, "<svg", 4) == 0 &&
            (svg_ws(b[i + 4]) || b[i + 4] == '>' || b[i + 4] == '/' || b[i + 4] == ':'))
            return 1;
        return 0;
    }
    return 0;
}

/* ---- state ------------------------------------------------------------- */

static uint64_t svg_fnv(const void *p, size_t n) {
    const uint8_t *s = (const uint8_t *)p;
    uint64_t h = 1469598103934665603ull;
    size_t i;
    for (i = 0; i < n; i++) {
        h ^= s[i];
        h *= 1099511628211ull;
    }
    return h;
}

static double svg_now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1000.0 + (double)t.tv_nsec / 1e6;
}

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pid_t g_pid;              /* live helper */
static int g_fd = -1;            /* our end of the socketpair */
static int g_disabled;           /* the helper said it refuses (platform stub) */
static int g_missing;            /* no helper binary */
static unsigned g_served;        /* requests the live helper answered */
static uint32_t g_gen;           /* generation stamp of the last request */
static double g_busy_until;      /* an abandoned request may run until then */
static int g_atexit;
static RtxSvgStats g_st;
static char g_bin[4096];
static int g_bin_done;

/* In-memory size cache: FNV of the bytes and their length -> CSS size. */
#define SVG_MEMN 1024
typedef struct {
    uint64_t h;
    uint64_t n;
    float w, hh;
    int used;
} SvgMem;
static SvgMem g_mem[SVG_MEMN];

void rtx_svg_forget_memory(void) {
    pthread_mutex_lock(&g_mu);
    memset(g_mem, 0, sizeof g_mem);
    pthread_mutex_unlock(&g_mu);
}

void rtx_svg_stats(RtxSvgStats *out) {
    pthread_mutex_lock(&g_mu);
    *out = g_st;
    out->pid = (int)g_pid;
    pthread_mutex_unlock(&g_mu);
}

/* ---- the helper binary ---------------------------------------------------- */

static int svg_exe_dir(char *out, size_t n) {
    char exe[3072];
    size_t len;
#if defined(__linux__)
    ssize_t r = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (r <= 0) return 0;
    exe[r] = 0;
#elif defined(__APPLE__)
    uint32_t sz = sizeof exe;
    char real[PATH_MAX];
    if (_NSGetExecutablePath(exe, &sz) != 0) return 0;
    if (realpath(exe, real)) snprintf(exe, sizeof exe, "%s", real);
#else
    return 0;
#endif
    len = strlen(exe);
    while (len && exe[len - 1] != '/') len--;
    if (!len) return 0;
    exe[len - 1] = 0;
    snprintf(out, n, "%s", exe);
    return 1;
}

static pthread_mutex_t g_bin_mu = PTHREAD_MUTEX_INITIALIZER;

const char *rtx_svg_helper_path(void) {
    pthread_mutex_lock(&g_bin_mu);
    if (!g_bin_done) {
        const char *env = getenv("RTX_RENDER_BIN");
        char dir[3200], p[3300];
        g_bin[0] = 0;
        if (env && env[0]) {
            snprintf(g_bin, sizeof g_bin, "%s", env);
        } else if (svg_exe_dir(dir, sizeof dir)) {
            snprintf(p, sizeof p, "%s/cctext-render", dir);
            if (access(p, X_OK) == 0) snprintf(g_bin, sizeof g_bin, "%s", p);
            else {
                /* bin-asan/, bin-tsan/, bin-prof/: the helper of bin/ */
                snprintf(p, sizeof p, "%s/../bin/cctext-render", dir);
                if (access(p, X_OK) == 0) snprintf(g_bin, sizeof g_bin, "%s", p);
            }
        }
        g_bin_done = 1;
    }
    pthread_mutex_unlock(&g_bin_mu);
    return g_bin;
}

/* ---- the helper process (g_mu held) ----------------------------------- */

static void svg_reap(int kill_it) {
    int st;
    if (g_fd >= 0) {
        close(g_fd);
        g_fd = -1;
    }
    if (g_pid > 0) {
        if (kill_it) kill(g_pid, SIGKILL);
        while (waitpid(g_pid, &st, 0) < 0 && errno == EINTR) {
        }
    }
    g_pid = 0;
    g_served = 0;
    g_busy_until = 0;
}

static void svg_atexit(void) { rtx_svg_shutdown(); }

/* Read exactly n bytes by the deadline. 0 ok, -1 EOF / error, -2 timeout,
 * -3 cancelled. */
static int svg_read(void *p, size_t n, double deadline, const _Atomic int *cancel) {
    uint8_t *b = (uint8_t *)p;
    while (n) {
        struct pollfd pf;
        double left = deadline - svg_now_ms();
        int to, r;
        ssize_t k;
        if (cancel && atomic_load_explicit(cancel, memory_order_relaxed)) return -3;
        if (left <= 0) return -2;
        to = left > 50 ? 50 : (int)left + 1;
        pf.fd = g_fd;
        pf.events = POLLIN;
        pf.revents = 0;
        r = poll(&pf, 1, to);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) return -1;
        if (r == 0) continue;
        k = read(g_fd, b, n);
        if (k < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (k <= 0) return -1;
        b += k;
        n -= (size_t)k;
    }
    return 0;
}

static int svg_write(const void *p, size_t n, double deadline) {
    const uint8_t *b = (const uint8_t *)p;
    while (n) {
        struct pollfd pf;
        double left = deadline - svg_now_ms();
        ssize_t k;
        int r;
        if (left <= 0) return -2;
        pf.fd = g_fd;
        pf.events = POLLOUT;
        pf.revents = 0;
        r = poll(&pf, 1, left > 50 ? 50 : (int)left + 1);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) return -1;
        if (r == 0) continue;
#if defined(MSG_NOSIGNAL)
        k = send(g_fd, b, n, MSG_NOSIGNAL);
#else
        k = send(g_fd, b, n, 0); /* SO_NOSIGPIPE is set on macOS */
#endif
        if (k < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (k <= 0) return -1;
        b += k;
        n -= (size_t)k;
    }
    return 0;
}

extern char **environ;

static int svg_spawn(uint32_t timeout_ms) {
    int sv[2];
    char pack[4200];
    const char *bin;
    char *argv[4];
    char *envp[4];
    char e1[256], e2[64];
    int ne = 0, rc;
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t at;
    sigset_t none, all;
    double t0 = svg_now_ms(), deadline;
    CrReply h;
    uint32_t hello[3];
    bin = rtx_svg_helper_path();
    if (!bin[0] || access(bin, X_OK) != 0) {
        g_missing = 1;
        return RTX_SVG_ENONE;
    }
    {
        size_t l = strlen(bin);
        while (l && bin[l - 1] != '/') l--;
        snprintf(pack, sizeof pack, "%.*scctext-render.pack", (int)l, bin);
    }
#if defined(SOCK_CLOEXEC)
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0) return RTX_SVG_ECRASH;
#else
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return RTX_SVG_ECRASH;
    fcntl(sv[0], F_SETFD, FD_CLOEXEC);
    fcntl(sv[1], F_SETFD, FD_CLOEXEC);
#endif
#if defined(SO_NOSIGPIPE)
    {
        int one = 1;
        setsockopt(sv[0], SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
    }
#endif
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, sv[1], 0);
    posix_spawn_file_actions_adddup2(&fa, sv[1], 1);
    if (!getenv("RTX_RENDER_DEBUG")) posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    posix_spawnattr_init(&at);
    sigemptyset(&none);
    sigfillset(&all);
    sigdelset(&all, SIGKILL);
    sigdelset(&all, SIGSTOP);
    posix_spawnattr_setsigmask(&at, &none);
    posix_spawnattr_setsigdefault(&at, &all);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    argv[0] = (char *)bin;
    argv[1] = "--pack";
    argv[2] = pack;
    argv[3] = NULL;
    /* A minimal environment: nothing of the editor's leaks into the
     * process that parses untrusted input. */
    if (getenv("CR_SELFTEST")) {
        snprintf(e1, sizeof e1, "CR_SELFTEST=%s", getenv("CR_SELFTEST"));
        envp[ne++] = e1;
    }
    if (getenv("RTX_RENDER_DEBUG")) {
        snprintf(e2, sizeof e2, "RTX_RENDER_DEBUG=1");
        envp[ne++] = e2;
    }
    envp[ne] = NULL;
    rc = posix_spawn(&g_pid, bin, &fa, &at, argv, envp);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    close(sv[1]);
    if (rc != 0) {
        close(sv[0]);
        g_pid = 0;
        g_missing = 1;
        return RTX_SVG_ENONE;
    }
    g_fd = sv[0];
    g_st.spawns++;
    g_served = 0;
    g_busy_until = 0;
    if (!g_atexit) {
        g_atexit = 1;
        atexit(svg_atexit);
    }
    deadline = t0 + (timeout_ms > 3000 ? timeout_ms : 3000);
    if (svg_read(&h, sizeof h, deadline, NULL) != 0 || h.magic != CR_MAGIC_REP ||
        h.type != CR_R_HELLO || h.len != sizeof hello ||
        svg_read(hello, sizeof hello, deadline, NULL) != 0) {
        g_st.crashes++;
        svg_reap(1);
        return RTX_SVG_ECRASH;
    }
    g_st.last_ready_ms = svg_now_ms() - t0;
    if ((hello[2] & CR_H_DISABLED) || !(hello[1] & (1u << CR_KIND_SVG))) {
        g_disabled = 1;
        svg_reap(0);
        return RTX_SVG_ENONE;
    }
    return RTX_SVG_OK;
}

/* Skip n payload bytes of a frame we do not want. */
static int svg_skip(size_t n, double deadline, const _Atomic int *cancel) {
    uint8_t buf[65536];
    while (n) {
        size_t k = n < sizeof buf ? n : sizeof buf;
        int r = svg_read(buf, k, deadline, cancel);
        if (r) return r;
        n -= k;
    }
    return 0;
}

static int svg_err_of(uint32_t code) {
    switch (code) {
    case CR_E_PIXELS: return RTX_SVG_EPX;
    case CR_E_NOMEM: return RTX_SVG_ENOMEM;
    case CR_E_KIND:
    case CR_E_DISABLED: return RTX_SVG_ENONE;
    default: return RTX_SVG_ECORRUPT;
    }
}

/* One request (g_mu held). size_only: fill *w, *h. Else also *out. */
static int svg_request_once(const uint8_t *b, size_t n, const RtxSvgCfg *cfg, int size_only,
                            uint32_t pw, uint32_t ph, const _Atomic int *cancel, float *w,
                            float *h, uint8_t **out, int *retry) {
    CrReq q;
    double start, deadline;
    int rc, got_size = 0;
    *retry = 0;
    if (g_disabled || g_missing) return RTX_SVG_ENONE;
    if (!g_pid) {
        rc = svg_spawn(cfg->timeout_ms);
        if (rc) return rc;
    }
    start = svg_now_ms();
    /* An abandoned request may still be running ahead of ours. */
    deadline = (g_busy_until > start ? g_busy_until : start) + (cfg->timeout_ms ? cfg->timeout_ms : 5000);
    memset(&q, 0, sizeof q);
    q.magic = CR_MAGIC_REQ;
    q.id = ++g_gen;
    if (!q.id) q.id = ++g_gen;
    q.kind = CR_KIND_SVG;
    q.flags = size_only ? CR_F_SIZE_ONLY : 0;
    q.scale = 1.0f;
    q.em_px = 16.0f;
    q.box_w = size_only ? 0 : pw;
    q.box_h = size_only ? 0 : ph;
    q.max_px = cfg->px_max > 0xffffffffull ? 0xffffffffu : (uint32_t)cfg->px_max;
    q.len = (uint32_t)n;
    g_st.requests++;
    if (svg_write(&q, sizeof q, deadline) != 0 || svg_write(b, n, deadline) != 0) goto dead;
    for (;;) {
        CrReply r;
        rc = svg_read(&r, sizeof r, deadline, cancel);
        if (rc) goto fail;
        if (r.magic != CR_MAGIC_REP || r.len > CR_PIXELS_HARD_MAX * 4u + 64u) goto dead;
        if (r.id != q.id) {
            /* A reply to a request we gave up on: drop it. */
            g_st.stale++;
            rc = svg_skip(r.len, deadline, cancel);
            if (rc) goto fail;
            continue;
        }
        if (r.type == CR_R_ERROR) {
            uint32_t code = 0;
            if (r.len < 4 || svg_read(&code, 4, deadline, cancel) != 0 ||
                svg_skip(r.len - 4, deadline, cancel) != 0)
                goto dead;
            g_served++;
            g_busy_until = 0;
            return svg_err_of(code);
        }
        if (r.type == CR_R_SIZE) {
            CrSize sz;
            if (r.len != sizeof sz || svg_read(&sz, sizeof sz, deadline, cancel) != 0) goto dead;
            if (!(sz.w_css > 0) || !(sz.h_css > 0) || !isfinite(sz.w_css) || !isfinite(sz.h_css))
                goto dead;
            *w = sz.w_css;
            *h = sz.h_css;
            got_size = 1;
            if (size_only) {
                g_served++;
                g_busy_until = 0;
                return RTX_SVG_OK;
            }
            continue;
        }
        if (r.type == CR_R_PIXELS && got_size && !size_only) {
            uint32_t hdr[3];
            uint8_t *px;
            size_t nb = (size_t)pw * ph * 4;
            if (r.len < 12 || svg_read(hdr, sizeof hdr, deadline, cancel) != 0) goto dead;
            if (hdr[0] != pw || hdr[1] != ph || hdr[2] != pw * 4 || (size_t)r.len - 12 != nb)
                goto dead;
            px = (uint8_t *)malloc(nb ? nb : 1);
            if (!px) {
                rc = svg_skip(nb, deadline, cancel);
                if (rc) goto fail;
                return RTX_SVG_ENOMEM;
            }
            rc = svg_read(px, nb, deadline, cancel);
            if (rc) {
                free(px);
                goto fail;
            }
            *out = px;
            g_served++;
            g_busy_until = 0;
            return RTX_SVG_OK;
        }
        goto dead;
    }
fail:
    if (rc == -3) {
        /* Cancelled: leave the helper working; its reply will be stale. */
        g_busy_until = deadline;
        return RTX_SVG_ECANCEL;
    }
    if (rc == -2) {
        g_st.kills++;
        svg_reap(1);
        return RTX_SVG_ETIMEOUT;
    }
dead:
    /* EOF or a broken frame: the helper crashed (or was killed by its
     * sandbox). An old helper may have died of old age (RLIMIT_CPU): one
     * retry on a fresh one. */
    *retry = g_served > 0;
    g_st.crashes++;
    svg_reap(1);
    return RTX_SVG_ECRASH;
}

static int svg_request(const uint8_t *b, size_t n, const RtxSvgCfg *cfg, int size_only,
                       uint32_t pw, uint32_t ph, const _Atomic int *cancel, float *w, float *h,
                       uint8_t **out) {
    int retry = 0, rc;
    rc = svg_request_once(b, n, cfg, size_only, pw, ph, cancel, w, h, out, &retry);
    if (rc == RTX_SVG_ECRASH && retry)
        rc = svg_request_once(b, n, cfg, size_only, pw, ph, cancel, w, h, out, &retry);
    return rc;
}

void rtx_svg_shutdown(void) {
    pthread_mutex_lock(&g_mu);
    if (g_pid > 0 && g_fd >= 0) {
        CrReq q;
        double dl = svg_now_ms() + 200;
        memset(&q, 0, sizeof q);
        q.magic = CR_MAGIC_REQ;
        q.kind = CR_KIND_QUIT;
        (void)svg_write(&q, sizeof q, dl);
        close(g_fd);
        g_fd = -1;
        /* Give it a moment to exit on its own, then make sure. */
        while (svg_now_ms() < dl) {
            int st;
            pid_t w = waitpid(g_pid, &st, WNOHANG);
            if (w == g_pid || (w < 0 && errno != EINTR)) {
                g_pid = 0;
                break;
            }
            {
                struct timespec ts = {0, 2 * 1000000};
                nanosleep(&ts, NULL);
            }
        }
    }
    svg_reap(1);
    /* The next request resolves the helper again (tests switch binaries). */
    g_missing = 0;
    g_disabled = 0;
    pthread_mutex_lock(&g_bin_mu);
    g_bin_done = 0;
    pthread_mutex_unlock(&g_bin_mu);
    pthread_mutex_unlock(&g_mu);
}

/* ---- disk cache ----------------------------------------------------------- */

static void svg_key(uint64_t h, uint64_t n, char *out, size_t on) {
    snprintf(out, on, "%016llx-%llx", (unsigned long long)h, (unsigned long long)n);
}

static int svg_disk_size_get(const RtxSvgCfg *cfg, uint64_t h, uint64_t n, float *w, float *hh) {
    char p[4400], k[64], buf[256];
    FILE *f;
    size_t got;
    char *tr;
    unsigned long long want;
    if (!cfg->cache_dir || !cfg->cache_max) return 0;
    svg_key(h, n, k, sizeof k);
    snprintf(p, sizeof p, "%s/%s.size", cfg->cache_dir, k);
    f = fopen(p, "rb");
    if (!f) return 0;
    got = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[got] = 0;
    tr = strstr(buf, "fnv ");
    if (!tr || strncmp(buf, "RTXS 1 ", 7) != 0) return 0;
    want = strtoull(tr + 4, NULL, 16);
    if (svg_fnv(buf, (size_t)(tr - buf)) != (uint64_t)want) return 0;
    if (sscanf(buf + 7, "%g %g", w, hh) != 2 || !(*w > 0) || !(*hh > 0)) return 0;
    return 1;
}

static void svg_disk_trim(const RtxSvgCfg *cfg, const char *keep) {
    for (;;) {
        DIR *dp = opendir(cfg->cache_dir);
        struct dirent *de;
        uint64_t total = 0;
        time_t oldest = 0;
        char victim[4400];
        struct stat st;
        victim[0] = 0;
        if (!dp) return;
        while ((de = readdir(dp))) {
            char p[4400];
            if (de->d_name[0] == '.') continue;
            snprintf(p, sizeof p, "%s/%s", cfg->cache_dir, de->d_name);
            if (stat(p, &st) != 0 || !S_ISREG(st.st_mode)) continue;
            total += (uint64_t)st.st_size;
            if ((!keep || strcmp(p, keep) != 0) && (!victim[0] || st.st_mtime < oldest)) {
                oldest = st.st_mtime;
                snprintf(victim, sizeof victim, "%s", p);
            }
        }
        closedir(dp);
        if (total <= cfg->cache_max || !victim[0]) return;
        (void)unlink(victim);
    }
}

static int svg_write_file(const char *path, const void *a, size_t an) {
    char tmp[4500];
    FILE *f;
    snprintf(tmp, sizeof tmp, "%s.%d.tmp", path, (int)getpid());
    f = fopen(tmp, "wb");
    if (!f) return 0;
    if (fwrite(a, 1, an, f) != an || fclose(f) != 0) {
        (void)unlink(tmp);
        return 0;
    }
    if (rename(tmp, path) != 0) {
        (void)unlink(tmp);
        return 0;
    }
    return 1;
}

static void svg_disk_size_put(const RtxSvgCfg *cfg, uint64_t h, uint64_t n, float w, float hh) {
    char p[4400], k[64], buf[256];
    int len;
    if (!cfg->cache_dir || !cfg->cache_max) return;
    svg_key(h, n, k, sizeof k);
    snprintf(p, sizeof p, "%s/%s.size", cfg->cache_dir, k);
    len = snprintf(buf, sizeof buf, "RTXS 1 %.9g %.9g\n", (double)w, (double)hh);
    len += snprintf(buf + len, sizeof buf - (size_t)len, "fnv %016llx\n",
                    (unsigned long long)svg_fnv(buf, (size_t)len));
    (void)svg_write_file(p, buf, (size_t)len);
}

/* Pixel file: "RTXP0001" | u32 w | u32 h | runs | u64 fnv of all before.
 * A run is u32 c: c & 0x80000000 -> (c & 0x7fffffff) literal pixels
 * follow; else one pixel repeated c times. Pixels are RGBA8 premultiplied. */
static uint8_t *svg_rle(const uint32_t *px, size_t npx, size_t *out_n) {
    size_t cap = 16 + npx * 4 + (npx / 4 + 2) * 4 + 8, o = 0, i = 0;
    uint8_t *b = (uint8_t *)malloc(cap);
    if (!b) return NULL;
    o = 16;
    while (i < npx) {
        size_t j = i + 1;
        while (j < npx && px[j] == px[i] && j - i < 0x7fffffff) j++;
        if (j - i >= 3) {
            uint32_t c = (uint32_t)(j - i);
            memcpy(b + o, &c, 4);
            memcpy(b + o + 4, &px[i], 4);
            o += 8;
            i = j;
        } else {
            /* literal until a run of 3 starts */
            size_t s = i;
            uint32_t c;
            while (i < npx && i - s < 0x7fffffff) {
                if (i + 2 < npx && px[i] == px[i + 1] && px[i] == px[i + 2]) break;
                i++;
            }
            c = 0x80000000u | (uint32_t)(i - s);
            if (o + 4 + (i - s) * 4 + 8 > cap) {
                free(b);
                return NULL;
            }
            memcpy(b + o, &c, 4);
            memcpy(b + o + 4, &px[s], (i - s) * 4);
            o += 4 + (i - s) * 4;
        }
        if (o + 16 > cap) {
            free(b);
            return NULL;
        }
    }
    *out_n = o;
    return b;
}

static void svg_disk_px_put(const RtxSvgCfg *cfg, uint64_t h, uint64_t n, uint32_t pw, uint32_t ph,
                            const uint8_t *px) {
    char p[4400], k[64];
    size_t bn = 0;
    uint8_t *b;
    uint64_t fnv;
    if (!cfg->cache_dir || !cfg->cache_max) return;
    b = svg_rle((const uint32_t *)px, (size_t)pw * ph, &bn);
    if (!b) return;
    memcpy(b, "RTXP0001", 8);
    memcpy(b + 8, &pw, 4);
    memcpy(b + 12, &ph, 4);
    fnv = svg_fnv(b, bn);
    memcpy(b + bn, &fnv, 8);
    bn += 8;
    if (bn <= cfg->cache_max) {
        svg_key(h, n, k, sizeof k);
        snprintf(p, sizeof p, "%s/%s-%ux%u.px", cfg->cache_dir, k, pw, ph);
        if (svg_write_file(p, b, bn)) svg_disk_trim(cfg, p);
    }
    free(b);
}

static int svg_disk_px_get(const RtxSvgCfg *cfg, uint64_t h, uint64_t n, uint32_t pw, uint32_t ph,
                           uint8_t **out) {
    char p[4400], k[64];
    FILE *f;
    struct stat st;
    uint8_t *b, *px;
    size_t bn, o = 16, at = 0, npx = (size_t)pw * ph;
    uint64_t fnv;
    uint32_t fw, fh;
    if (!cfg->cache_dir || !cfg->cache_max) return 0;
    svg_key(h, n, k, sizeof k);
    snprintf(p, sizeof p, "%s/%s-%ux%u.px", cfg->cache_dir, k, pw, ph);
    f = fopen(p, "rb");
    if (!f) return 0;
    if (fstat(fileno(f), &st) != 0 || st.st_size < 24 || (uint64_t)st.st_size > npx * 8 + 64) {
        fclose(f);
        return 0;
    }
    bn = (size_t)st.st_size;
    b = (uint8_t *)malloc(bn);
    if (!b || fread(b, 1, bn, f) != bn) {
        fclose(f);
        free(b);
        return 0;
    }
    fclose(f);
    memcpy(&fnv, b + bn - 8, 8);
    memcpy(&fw, b + 8, 4);
    memcpy(&fh, b + 12, 4);
    if (memcmp(b, "RTXP0001", 8) != 0 || fw != pw || fh != ph || svg_fnv(b, bn - 8) != fnv) {
        free(b);
        return 0;
    }
    bn -= 8;
    px = (uint8_t *)malloc(npx * 4 ? npx * 4 : 4);
    if (!px) {
        free(b);
        return 0;
    }
    while (o + 4 <= bn && at < npx) {
        uint32_t c, cnt;
        memcpy(&c, b + o, 4);
        o += 4;
        cnt = c & 0x7fffffffu;
        if (!cnt || cnt > npx - at) break;
        if (c & 0x80000000u) {
            if ((size_t)cnt * 4 > bn - o) break;
            memcpy(px + at * 4, b + o, (size_t)cnt * 4);
            o += (size_t)cnt * 4;
        } else {
            uint32_t v, i;
            if (o + 4 > bn) break;
            memcpy(&v, b + o, 4);
            o += 4;
            for (i = 0; i < cnt; i++) memcpy(px + (at + i) * 4, &v, 4);
        }
        at += cnt;
    }
    free(b);
    if (at != npx || o != bn) {
        free(px);
        return 0;
    }
    (void)utimensat(AT_FDCWD, p, NULL, 0); /* recently used */
    *out = px;
    return 1;
}

/* ---- API ------------------------------------------------------------------ */

static SvgMem *svg_mem_slot(uint64_t h, uint64_t n) {
    return &g_mem[(h ^ n) % SVG_MEMN];
}

int rtx_svg_size(const uint8_t *b, size_t n, const RtxSvgCfg *cfg, const _Atomic int *cancel,
                 float *w, float *h) {
    uint64_t hs;
    SvgMem *m;
    int rc;
    float fw = 0, fh = 0;
    if (!b || !n || !cfg) return RTX_SVG_ECORRUPT;
    if ((uint64_t)n > cfg->input_max || n > CR_INPUT_HARD_MAX) return RTX_SVG_EBIG;
    hs = svg_fnv(b, n);
    pthread_mutex_lock(&g_mu);
    m = svg_mem_slot(hs, n);
    if (m->used && m->h == hs && m->n == n) {
        *w = m->w;
        *h = m->hh;
        g_st.mem_hits++;
        pthread_mutex_unlock(&g_mu);
        return RTX_SVG_OK;
    }
    if (svg_disk_size_get(cfg, hs, n, &fw, &fh)) {
        g_st.disk_size_hits++;
        rc = RTX_SVG_OK;
    } else {
        rc = svg_request(b, n, cfg, 1, 0, 0, cancel, &fw, &fh, NULL);
        if (rc == RTX_SVG_OK) svg_disk_size_put(cfg, hs, n, fw, fh);
    }
    if (rc == RTX_SVG_OK) {
        m->used = 1;
        m->h = hs;
        m->n = n;
        m->w = fw;
        m->hh = fh;
        *w = fw;
        *h = fh;
    }
    pthread_mutex_unlock(&g_mu);
    return rc;
}

int rtx_svg_render(const uint8_t *b, size_t n, const RtxSvgCfg *cfg, uint32_t pw, uint32_t ph,
                   const _Atomic int *cancel, uint8_t **out) {
    uint64_t hs;
    int rc;
    float fw = 0, fh = 0;
    *out = NULL;
    if (!b || !n || !cfg || !pw || !ph) return RTX_SVG_ECORRUPT;
    if ((uint64_t)n > cfg->input_max || n > CR_INPUT_HARD_MAX) return RTX_SVG_EBIG;
    if ((uint64_t)pw * ph > cfg->px_max || pw > CR_SIDE_HARD_MAX || ph > CR_SIDE_HARD_MAX)
        return RTX_SVG_EPX;
    hs = svg_fnv(b, n);
    pthread_mutex_lock(&g_mu);
    if (svg_disk_px_get(cfg, hs, n, pw, ph, out)) {
        g_st.disk_px_hits++;
        pthread_mutex_unlock(&g_mu);
        return RTX_SVG_OK;
    }
    rc = svg_request(b, n, cfg, 0, pw, ph, cancel, &fw, &fh, out);
    if (rc == RTX_SVG_OK) {
        SvgMem *m = svg_mem_slot(hs, n);
        m->used = 1;
        m->h = hs;
        m->n = n;
        m->w = fw;
        m->hh = fh;
        svg_disk_px_put(cfg, hs, n, pw, ph, *out);
    }
    pthread_mutex_unlock(&g_mu);
    return rc;
}

#endif /* POSIX */
