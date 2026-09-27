// corpus/tsdguard: proves runtime/xl_tsd_guard.h's page-protection scheme itself, not the
// lifted-code path that uses it. mmap/mprotect/SIGSEGV are POSIX, so this runs on the build host
// (no device, no lift): it includes the exact header runtime.c does, so there is one
// implementation under test, not a reimplementation that could quietly drift from it.
//
// coordination/crutches.md, "TPIDRRO_EL0 backed by a mostly-zero block": round 10's build-time
// scan (xlate/src/tsd_scan.cpp) is a static heuristic with known false-negative gaps (see
// xl_tsd_guard.h's own comment, and coordination/reviews/2026-09-27-katabasis-7ecd76e.md). This
// is what actually proves a guest read below the one audited offset (0x358, Swift's
// __PTK_FRAMEWORK_SWIFT_KEY7) cannot silently return zero: it now faults for real, regardless of
// whether any static scan would have caught the instruction that did it.
#include "../../runtime/xl_tsd_guard.h"

#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

static sigjmp_buf jb;

static void handler(int sig, siginfo_t *info, void *ctx)
{
    (void)sig;
    (void)info;
    (void)ctx;
    siglongjmp(jb, 1);
}

// Returns 1 if reading *(uint64_t *)addr raised SIGSEGV/SIGBUS (and does not touch *out), 0 if it
// read cleanly (and stores the value read).
static int try_read(uintptr_t addr, uint64_t *out)
{
    if (sigsetjmp(jb, 1) == 0) {
        *out = *(volatile uint64_t *)addr;
        return 0;
    }
    return 1;
}

int main(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL); // a protection fault on some kernels raises this instead

    void *pages = xl_tsd_alloc();
    uintptr_t base = xl_tsd_base(pages);
    int fail = 0;
    uint64_t v;

    // Slots this recompiler's guest threads have never populated and never audited: real Darwin
    // has pthread_self at slot 0, errno at slot 1, and so on up to slot 106, all below 0x358.
    uintptr_t bad_offsets[] = {0, 8, XL_TSD_AUDITED_OFFSET - 8};
    for (size_t i = 0; i < sizeof(bad_offsets) / sizeof(bad_offsets[0]); i++) {
        if (!try_read(base + bad_offsets[i], &v)) {
            fprintf(stderr, "FAIL: offset 0x%lx did not fault (read %llu)\n",
                    (unsigned long)bad_offsets[i], (unsigned long long)v);
            fail = 1;
        }
    }

    // The audited offset, and a bit further into the same RW page (unprotected by design, see
    // xl_tsd_guard.h): both must read cleanly, and read zero (nothing has ever populated them).
    uintptr_t good_offsets[] = {XL_TSD_AUDITED_OFFSET, XL_TSD_AUDITED_OFFSET + 8};
    for (size_t i = 0; i < sizeof(good_offsets) / sizeof(good_offsets[0]); i++) {
        if (try_read(base + good_offsets[i], &v)) {
            fprintf(stderr, "FAIL: offset 0x%lx faulted, expected a clean read of zero\n",
                    (unsigned long)good_offsets[i]);
            fail = 1;
        } else if (v != 0) {
            fprintf(stderr, "FAIL: offset 0x%lx read %llu, expected zero\n",
                    (unsigned long)good_offsets[i], (unsigned long long)v);
            fail = 1;
        }
    }

    xl_tsd_free(pages);
    if (fail)
        return 1;
    printf("ok\n");
    return 0;
}
