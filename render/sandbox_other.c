/*
 * cctext-render on a platform without a lockdown here (Windows until the
 * AppContainer launcher exists): the helper starts, says so in its hello
 * (CR_H_DISABLED) and answers every request with CR_E_DISABLED, so the
 * editor shows placeholders. It never parses untrusted input unsandboxed.
 */
#if !defined(__linux__) && !defined(__APPLE__)
#include <stddef.h>

int cr_sandbox_lockdown(size_t mem_limit, unsigned cpu_seconds)
{
    (void)mem_limit;
    (void)cpu_seconds;
    return -1;
}
#endif

/* keeps the translation unit non-empty on the other platforms */
typedef int cr_sandbox_other_tu;
