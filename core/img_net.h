/*
 * Fetch shim for remote images (docs/images.md "Remote"). One fetch is a
 * background transfer into a file, polled from the UI thread's pump,
 * cancellable, with a byte cap and a timeout. Never a shell, never a
 * blocking call on the frame.
 *
 * This build spawns `curl` (posix_spawnp, argv only, no shell): libcurl
 * is not a build dependency (no headers in the reference environment)
 * and a macOS NSURLSession backend is not wired yet, so both platforms
 * use the curl binary macOS and every Linux distribution ship. The
 * arguments pin the policy: --proto =https (=http,https only when the
 * user allowed http for that host), --proto-redir the same, at most 3
 * redirects, --max-filesize and --max-time, --fail on HTTP errors, the
 * body to a temp file. The shim also stats the file while it grows and
 * kills the transfer past the cap (a chunked reply has no length for
 * --max-filesize to check) and past the deadline.
 */
#ifndef RTX_IMG_NET_H
#define RTX_IMG_NET_H

#include <stddef.h>
#include <stdint.h>

typedef struct RtxImgNet RtxImgNet;

/* Start fetching url into out_path (created / truncated). etag_path: a
 * file curl reads a validator from (--etag-compare, when it exists) and
 * saves the reply's ETag to. NULL on failure to start (err filled). */
RtxImgNet *rtx_img_net_start(const char *url, const char *out_path,
                             const char *etag_path, int allow_http,
                             uint64_t max_bytes, unsigned timeout_s,
                             char *err, size_t errn);

/* 0: still running. 1: done, out_path holds the body (or, with an ETag
 * match, is empty: *not_modified = 1). < 0: an RTX_IMG_E* code (fetch,
 * timeout, size, http). */
int rtx_img_net_poll(RtxImgNet *n, int *not_modified);

/* Kill and reap (no zombie); safe on a finished fetch. */
void rtx_img_net_cancel(RtxImgNet *n);
void rtx_img_net_free(RtxImgNet *n);

/* The program the shim runs ("curl"; RTX_IMG_CURL overrides, tests). */
const char *rtx_img_net_prog(void);

#endif /* RTX_IMG_NET_H */
