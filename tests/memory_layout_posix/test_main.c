/*
 * The Xbox memory model on a POSIX host -- written to fail.
 *
 * These are the properties a port has to satisfy, asserted before the port
 * exists so that "done" is defined by something other than opinion. On
 * arm64 macOS today every one of them fails, and each failure names a
 * distinct problem rather than one problem seen four times:
 *
 *   1. host page size    the code hardcodes 4096 in places; Apple Silicon
 *                        uses 16384, so a VirtualProtect over "one page"
 *                        silently covers four times what it means to
 *   2. base view         __PAGEZERO spans 0..4GB, so every candidate base in
 *                        try_bases[] is unmappable, and the "let the OS
 *                        choose" sentinel is never reached because the loop
 *                        condition terminates on it
 *   3. mirrors           28 views at 64MB intervals must alias the same
 *                        physical pages: the 26-bit address bus means
 *                        0x04070000 reads what 0x00070000 holds
 *   4. teardown          shutdown must release every view, or a second init
 *                        in one process fails on addresses it already owns
 *
 * Deliberately not asserted: the tiled aperture at 0xF0000000. Whether that
 * specific architectural alias survives relocation to a high host base is a
 * question about what guest code assumes, and guessing at it here would bake
 * in an answer nobody has established.
 */
#include "xbox_memory_layout.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <setjmp.h>
#include <signal.h>

static int failures = 0;

/* Probing a mirror that the layout could not place faults, and a test that
 * dies on a signal reports nothing about the other 27. Catch the fault and
 * treat it as "not accessible" so the run finishes and names every mirror. */
static sigjmp_buf fault_jmp;
static volatile sig_atomic_t faulted;

static void on_fault(int sig) { (void)sig; faulted = 1; siglongjmp(fault_jmp, 1); }

static void fault_guard_install(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_fault;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
}

/* Read one byte, or report that the address is not accessible. */
static int try_read(const unsigned char *p, unsigned char *out)
{
    faulted = 0;
    if (sigsetjmp(fault_jmp, 1) == 0) { *out = *p; return 1; }
    return 0;
}

static int try_write(unsigned char *p, unsigned char v)
{
    faulted = 0;
    if (sigsetjmp(fault_jmp, 1) == 0) { *p = v; return 1; }
    return 0;
}

static void check(int ok, const char *what, const char *detail)
{
    printf("  %-44s %s%s%s\n", what, ok ? "PASS" : "FAIL",
           detail && !ok ? " -- " : "", detail && !ok ? detail : "");
    if (!ok) failures++;
}

/* The model parses section layout from an XBE header; the synthetic one from
 * tools/conformance/mkxbe.py is enough to get through init. */
static size_t load_xbe(const char *path, unsigned char *buf, size_t cap)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t n = fread(buf, 1, cap, f);
    fclose(f);
    return n;
}

int main(int argc, char **argv)
{
    /* Unbuffered: these checks poke at mappings that may fault, and buffered
     * output is discarded when they do -- leaving a crash with no indication
     * of which check reached it. */
    setvbuf(stdout, NULL, _IONBF, 0);
    fault_guard_install();

    static unsigned char xbe[1 << 20];
    const char *path = argc > 1 ? argv[1] : "tools/conformance/test.xbe";
    size_t n = load_xbe(path, xbe, sizeof xbe);
    if (!n) {
        fprintf(stderr, "cannot read %s -- run: python3 tools/conformance/mkxbe.py\n",
                path);
        return 2;
    }

    long host_page = sysconf(_SC_PAGESIZE);
    printf("host page size %ld, %d mirrors expected\n\n",
           host_page, XBOX_NUM_MIRRORS);

    /* 1. The model must come up at all. */
    BOOL ok = xbox_MemoryLayoutInit(xbe, n);
    check(ok, "xbox_MemoryLayoutInit succeeds",
          "base view unmappable: every try_bases[] entry is inside __PAGEZERO");
    if (!ok) {
        printf("\n%d failure(s); later checks need a live mapping.\n", failures);
        return 1;
    }

    void  *base = xbox_GetMemoryBase();
    size_t size = xbox_GetMappedSize();
    check(base != NULL, "base pointer is non-NULL", NULL);
    check(size > 0, "mapped size is non-zero", NULL);

    /* 2. Host page granularity, not the guest's 4 KB. */
    check(((uintptr_t)base % (uintptr_t)host_page) == 0,
          "base is aligned to the HOST page size",
          "aligned to 4096 but not to the host's larger page");
    check((size % (size_t)host_page) == 0,
          "mapped size is a whole number of host pages", NULL);

    /* 3. The wrap. Write through the base, read through each mirror: the
     *    26-bit bus means every mirror is the same physical memory. */
    fprintf(stderr, "\n[reached mirror section]\n");
    unsigned char *p = (unsigned char *)base;
    const size_t probe = 0x70000;          /* the address the comment cites */
    if (probe < size) {
        p[probe] = 0x5A;
        int aliased = 0, checked = 0, unmapped = 0;
        for (int m = 1; m <= XBOX_NUM_MIRRORS; m++) {
            unsigned char *mp = p + (size_t)m * size + probe;
            checked++;
            /* Ask before touching. A mirror the layout could not place is not
             * mapped at all, and dereferencing it would abort this test with a
             * signal instead of reporting which mirrors are missing. */
            unsigned char got = 0;
            if (!try_read(mp, &got)) { unmapped++; continue; }
            if (got == 0x5A) aliased++;
        }
        char detail[128];
        snprintf(detail, sizeof detail,
                 "%d of %d aliased, %d never mapped", aliased, checked, unmapped);
        check(aliased == checked, "every mirror aliases the base page", detail);

        /* And the other direction: a write through a mirror is visible at the
         * base, which is what a title's out-of-range write actually does. */
        unsigned char *m1 = p + size + probe;
        if (try_write(m1, 0xA5)) {
            check(p[probe] == 0xA5, "a write through mirror 1 reaches the base",
                  "mirror is a separate copy, not an alias");
        } else {
            check(0, "a write through mirror 1 reaches the base",
                  "mirror 1 is not accessible");
        }
    } else {
        check(0, "mapped region covers the 0x70000 probe", "region too small");
    }

    fprintf(stderr, "[mirror section done]\n");

    /* 4. Teardown has to give the addresses back, or nothing can re-init. */
    xbox_MemoryLayoutShutdown();
    BOOL again = xbox_MemoryLayoutInit(xbe, n);
    check(again, "a second init after shutdown succeeds",
          "shutdown leaked views, so the addresses are still taken");
    if (again) xbox_MemoryLayoutShutdown();

    printf("\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
