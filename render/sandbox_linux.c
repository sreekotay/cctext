/*
 * cctext-render Linux lockdown: rlimits + no_new_privs + seccomp-bpf (raw
 * BPF, no libseccomp). Called once, after the pack and fonts are in
 * memory and before the first request byte is read.
 *
 * Policy (x86_64 and aarch64):
 *   allow   read (fd 0 only), write (fd 1 and 2 only), exit, exit_group,
 *           brk, mmap / mprotect without PROT_EXEC, munmap, mremap,
 *           madvise, futex, rt_sigreturn, rt_sigprocmask, sigaltstack,
 *           clock_gettime, gettimeofday, getrandom, close, fstat /
 *           newfstatat, lseek (stdio), sched_yield, getpid
 *   EACCES  open / openat / openat2 / stat family / access / readlink:
 *           a lookup fails soft instead of killing the helper
 *   kill    everything else: socket, connect, execve, clone / fork,
 *           ptrace, mmap(PROT_EXEC), read of any other fd, ...
 * x32 syscalls (nr | 0x40000000 on x86_64) are killed.
 */
#if defined(__linux__)
#define _GNU_SOURCE
#include <errno.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#if defined(__x86_64__)
#define CR_ARCH_NR AUDIT_ARCH_X86_64
#elif defined(__aarch64__)
#define CR_ARCH_NR AUDIT_ARCH_AARCH64
#else
#error "sandbox_linux.c: add the audit arch for this target"
#endif

#define LD_NR BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr))
#define LD_ARG(i) BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[i]))
#define RET_ALLOW BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)
#define RET_ERRNO(e) BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | ((e) & SECCOMP_RET_DATA))
#define RET_KILL BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS)
/* if nr == n: run the next instruction, else skip it */
#define IF_NR(n) BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (n), 0, 1)
#define ALLOW_NR(n) IF_NR(n), RET_ALLOW
#define EACCES_NR(n) IF_NR(n), RET_ERRNO(EACCES)

int cr_sandbox_lockdown(size_t mem_limit, unsigned cpu_seconds)
{
    struct rlimit r;
    if (mem_limit) {
        r.rlim_cur = r.rlim_max = mem_limit;
        if (setrlimit(RLIMIT_AS, &r) != 0) return -1;
    }
    if (cpu_seconds) {
        r.rlim_cur = cpu_seconds;
        r.rlim_max = cpu_seconds + 1;
        if (setrlimit(RLIMIT_CPU, &r) != 0) return -1;
    }
    r.rlim_cur = r.rlim_max = 0;
    (void)setrlimit(RLIMIT_NOFILE, &r); /* no new fds even if a syscall slipped through */
    (void)setrlimit(RLIMIT_NPROC, &r);
    (void)setrlimit(RLIMIT_CORE, &r);
    /* No RLIMIT_FSIZE: stderr may be a log file the editor owns. */

    struct sock_filter f[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, CR_ARCH_NR, 1, 0),
        RET_KILL,
        LD_NR,
#if defined(__x86_64__)
        /* x32 ABI numbers alias the x86_64 ones: refuse them all. */
        BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, 0x40000000u, 0, 1),
        RET_KILL,
#endif
        /* read(0, ...) only */
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_read, 0, 4),
        LD_ARG(0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 0, 1),
        RET_ALLOW,
        RET_KILL,
        /* write(1 | 2, ...) only */
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_write, 0, 5),
        LD_ARG(0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 1, 1, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 2, 0, 1),
        RET_ALLOW,
        RET_KILL,
        /* mmap / mprotect: never PROT_EXEC (arg 2) */
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_mmap, 1, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_mprotect, 0, 4),
        LD_ARG(2),
        BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K, PROT_EXEC, 0, 1),
        RET_KILL,
        RET_ALLOW,
        ALLOW_NR(__NR_munmap),
        ALLOW_NR(__NR_mremap),
        ALLOW_NR(__NR_madvise),
        ALLOW_NR(__NR_brk),
        ALLOW_NR(__NR_exit),
        ALLOW_NR(__NR_exit_group),
        ALLOW_NR(__NR_futex),
        ALLOW_NR(__NR_rt_sigreturn),
        ALLOW_NR(__NR_rt_sigprocmask),
        ALLOW_NR(__NR_sigaltstack),
        ALLOW_NR(__NR_clock_gettime),
        ALLOW_NR(__NR_gettimeofday),
        ALLOW_NR(__NR_getrandom),
        ALLOW_NR(__NR_close),
        ALLOW_NR(__NR_fstat),
        ALLOW_NR(__NR_newfstatat),
        ALLOW_NR(__NR_lseek),
        ALLOW_NR(__NR_sched_yield),
        ALLOW_NR(__NR_getpid),
        /* fail soft: file lookups */
        EACCES_NR(__NR_openat),
#ifdef __NR_open
        EACCES_NR(__NR_open),
#endif
#ifdef __NR_stat
        EACCES_NR(__NR_stat),
#endif
#ifdef __NR_lstat
        EACCES_NR(__NR_lstat),
#endif
#ifdef __NR_access
        EACCES_NR(__NR_access),
#endif
#ifdef __NR_readlink
        EACCES_NR(__NR_readlink),
#endif
#ifdef __NR_openat2
        EACCES_NR(__NR_openat2),
#endif
        EACCES_NR(__NR_statx),
        EACCES_NR(__NR_faccessat),
#ifdef __NR_faccessat2
        EACCES_NR(__NR_faccessat2),
#endif
        EACCES_NR(__NR_readlinkat),
        RET_KILL,
    };
    struct sock_fprog prog = {.len = (unsigned short)(sizeof f / sizeof f[0]), .filter = f};
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        perror("cctext-render: no_new_privs");
        return -1;
    }
    if (syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_TSYNC, &prog) != 0) {
        perror("cctext-render: seccomp");
        return -1;
    }
    return 0;
}
#endif /* __linux__ */

/* keeps the translation unit non-empty on the other platforms */
typedef int cr_sandbox_linux_tu;
