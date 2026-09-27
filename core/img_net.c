/*
 * Remote image fetch: spawn curl with a fixed argv (core/img_net.h).
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "core/img_net.h"

/* Same values as core/img.cch (a plain C unit cannot include a .cch). */
#define NET_EFETCH (-13)
#define NET_ETIMEOUT (-15)
#define NET_ESIZE (-16)
#define NET_EHTTP (-17)

#if defined(__APPLE__)
#include <crt_externs.h>
#define RTX_NET_ENVIRON (*_NSGetEnviron())
#else
extern char **environ;
#define RTX_NET_ENVIRON environ
#endif

struct RtxImgNet {
    pid_t pid;
    int done;
    int rc;
    int not_modified;
    uint64_t max_bytes;
    long deadline_ms;
    char out[4096];
};

static long net_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

const char *rtx_img_net_prog(void) {
    const char *p = getenv("RTX_IMG_CURL");
    return p && p[0] ? p : "curl";
}

RtxImgNet *rtx_img_net_start(const char *url, const char *out_path,
                             const char *etag_path, int allow_http,
                             uint64_t max_bytes, unsigned timeout_s,
                             char *err, size_t errn) {
    RtxImgNet *n;
    char maxfs[32], maxt[32];
    const char *argv[40];
    int k = 0;
    posix_spawn_file_actions_t fa;
    int rc;
    struct stat st;
    if (err && errn) err[0] = 0;
    if (!url || !out_path) return NULL;
    /* Only http(s) URLs reach here; refuse anything else outright. */
    if (strncmp(url, "https://", 8) != 0 &&
        !(allow_http && strncmp(url, "http://", 7) == 0)) {
        if (err && errn) snprintf(err, errn, "not an allowed URL");
        return NULL;
    }
    n = (RtxImgNet *)calloc(1, sizeof *n);
    if (!n) return NULL;
    n->max_bytes = max_bytes;
    if (timeout_s < 1) timeout_s = 1;
    n->deadline_ms = net_now_ms() + (long)timeout_s * 1000 + 2000;
    snprintf(n->out, sizeof n->out, "%s", out_path);
    snprintf(maxfs, sizeof maxfs, "%llu", (unsigned long long)max_bytes);
    snprintf(maxt, sizeof maxt, "%u", timeout_s);
    argv[k++] = rtx_img_net_prog();
    argv[k++] = "--silent";
    argv[k++] = "--fail";
    argv[k++] = "--location";
    argv[k++] = "--max-redirs";
    argv[k++] = "3";
    argv[k++] = "--proto";
    argv[k++] = allow_http ? "=http,https" : "=https";
    argv[k++] = "--proto-redir";
    argv[k++] = allow_http ? "=http,https" : "=https";
    argv[k++] = "--max-filesize";
    argv[k++] = maxfs;
    argv[k++] = "--max-time";
    argv[k++] = maxt;
    argv[k++] = "--connect-timeout";
    argv[k++] = maxt;
    /* Loopback never goes through a proxy (tests, local servers). */
    argv[k++] = "--noproxy";
    argv[k++] = "localhost,127.0.0.1,::1";
    argv[k++] = "--output";
    argv[k++] = out_path;
    if (etag_path && etag_path[0]) {
        if (stat(etag_path, &st) == 0 && st.st_size > 0) {
            argv[k++] = "--etag-compare";
            argv[k++] = etag_path;
        }
        argv[k++] = "--etag-save";
        argv[k++] = etag_path;
    }
    argv[k++] = "--";
    argv[k++] = url;
    argv[k] = NULL;
    (void)unlink(out_path);
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    rc = posix_spawnp(&n->pid, argv[0], &fa, NULL, (char *const *)argv, RTX_NET_ENVIRON);
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) {
        if (err && errn) snprintf(err, errn, "cannot run %s: %s", argv[0], strerror(rc));
        free(n);
        return NULL;
    }
    return n;
}

int rtx_img_net_poll(RtxImgNet *n, int *not_modified) {
    int status = 0;
    pid_t w;
    struct stat st;
    if (not_modified) *not_modified = 0;
    if (!n) return NET_EFETCH;
    if (n->done) {
        if (not_modified) *not_modified = n->not_modified;
        return n->rc;
    }
    w = waitpid(n->pid, &status, WNOHANG);
    if (w == 0) {
        /* Still running: enforce the cap and the deadline ourselves. */
        if (stat(n->out, &st) == 0 && (uint64_t)st.st_size > n->max_bytes) {
            rtx_img_net_cancel(n);
            n->rc = NET_ESIZE;
            return n->rc;
        }
        if (net_now_ms() > n->deadline_ms) {
            rtx_img_net_cancel(n);
            n->rc = NET_ETIMEOUT;
            return n->rc;
        }
        return 0;
    }
    n->done = 1;
    n->pid = 0;
    if (w < 0 || !WIFEXITED(status)) {
        n->rc = NET_EFETCH;
        return n->rc;
    }
    switch (WEXITSTATUS(status)) {
    case 0:
        if (stat(n->out, &st) != 0) {
            /* --etag-compare matched: curl wrote nothing. */
            n->not_modified = 1;
            n->rc = 1;
        } else if ((uint64_t)st.st_size > n->max_bytes) {
            n->rc = NET_ESIZE;
        } else {
            n->rc = 1;
        }
        break;
    case 28: n->rc = NET_ETIMEOUT; break;           /* operation timeout */
    case 63: n->rc = NET_ESIZE; break;              /* max filesize */
    case 1: n->rc = NET_EHTTP; break;               /* protocol refused */
    default: n->rc = NET_EFETCH; break;
    }
    if (not_modified) *not_modified = n->not_modified;
    return n->rc;
}

void rtx_img_net_cancel(RtxImgNet *n) {
    int status;
    if (!n || n->done) return;
    if (n->pid > 0) {
        kill(n->pid, SIGKILL);
        while (waitpid(n->pid, &status, 0) < 0 && errno == EINTR) {
        }
    }
    n->pid = 0;
    n->done = 1;
    if (n->rc == 0) n->rc = NET_EFETCH;
    (void)unlink(n->out);
}

void rtx_img_net_free(RtxImgNet *n) {
    if (!n) return;
    rtx_img_net_cancel(n);
    free(n);
}
