/* cctext embed fixture: asm, tricky strings, comments, macros, #if 0. */
#include <stdio.h>

#define SWAP(a, b) do { \
    int _t = (a);       \
    (a) = (b);          \
    (b) = _t;           \
} while (0)

static inline unsigned long rdtsc_lo(void) {
    unsigned long lo = 0;
#if defined(__x86_64__)
    asm volatile("rdtsc\n\t"
                 "mov %%eax, %0"
                 : "=r"(lo) : : "edx");
#endif
    return lo;
}

static const char *not_a_comment = "/* this is a string, not a comment */";
/* a comment containing a double quote " and a single quote ' */
static const char quote_char = '"';
static const char *fmt = "%s=%d %5.2f\t%%\n";

#if 0
dead code with an unbalanced " quote; a real compiler only warns here
static int nope(void) { return "; }
#endif

int main(void) {
    int a = 1, b = 2;
    SWAP(a, b);
    printf(fmt, "swapped", a, (double)b);
    printf("%s %c %lu\n", not_a_comment, quote_char, rdtsc_lo());
    return 0;
}
