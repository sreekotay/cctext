/*
 * cctext-render macOS lockdown: rlimits + sandbox_init() with a
 * deny-default profile (the Seatbelt SBPL the system's own services use).
 *
 * NOT BUILT OR RUN in the reference environment (no macOS there): written
 * against the documented API and Chromium's / WebKit's renderer profiles;
 * the first macOS build must run `CR_SELFTEST=...` (the test build) and
 * the svg_smoke protocol tests before this ships.
 *
 * What the profile allows after lockdown: nothing but what a process can
 * do without asking the kernel for a new resource — reading its already
 * open stdin and writing its already open stdout / stderr (Seatbelt checks
 * file access at open time), allocating memory, reading the clock, and the
 * sysctls libmalloc reads. No file opens (read or write), no network, no
 * process creation, no Mach service lookups, no IOKit.
 *
 * sandbox_init() is marked deprecated since 10.8 but is still exported
 * and used (Chromium used it for years; the replacement is private SPI).
 */
#if defined(__APPLE__)
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
int sandbox_init(const char *profile, uint64_t flags, char **errorbuf);
void sandbox_free_error(char *errorbuf);

static const char cr_profile[] =
    "(version 1)\n"
    "(deny default)\n"
    /* libmalloc / libSystem read a few sysctls lazily */
    "(allow sysctl-read)\n"
    /* its own signals (abort) */
    "(allow signal (target self))\n";

int cr_sandbox_lockdown(size_t mem_limit, unsigned cpu_seconds)
{
    struct rlimit r;
    char *err = NULL;
    /* RLIMIT_AS is not enforced by XNU; RLIMIT_DATA covers malloc's
     * brk-less heap only partly. The editor's time budget and the pixel
     * caps are the real bounds here. */
    if (mem_limit) {
        r.rlim_cur = r.rlim_max = mem_limit;
        (void)setrlimit(RLIMIT_DATA, &r);
    }
    if (cpu_seconds) {
        r.rlim_cur = cpu_seconds;
        r.rlim_max = cpu_seconds + 1;
        (void)setrlimit(RLIMIT_CPU, &r);
    }
    r.rlim_cur = r.rlim_max = 0;
    (void)setrlimit(RLIMIT_NPROC, &r);
    (void)setrlimit(RLIMIT_CORE, &r);
    r.rlim_cur = r.rlim_max = 3;
    (void)setrlimit(RLIMIT_NOFILE, &r);
    if (sandbox_init(cr_profile, 0, &err) != 0) {
        fprintf(stderr, "cctext-render: sandbox_init: %s\n", err ? err : "failed");
        if (err) sandbox_free_error(err);
        return -1;
    }
    return 0;
}
#pragma clang diagnostic pop
#endif /* __APPLE__ */

/* keeps the translation unit non-empty on the other platforms */
typedef int cr_sandbox_darwin_tu;
