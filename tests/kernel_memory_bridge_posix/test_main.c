/*
 * Kernel thread objects, through the path a title takes.
 *
 * A thread handle must lead to a guest thread object whose SignalState and
 * ExitStatus follow the host thread, through ObReferenceObjectByHandle and
 * through a duplicate of the handle, so a title that waits on the object or
 * polls its exit code sees the thread end.
 *
 * Each bridge is driven by ordinal through recomp_lookup_kernel, as in
 * tests/kernel_bridge, but against a real guest window from
 * xbox_MemoryLayoutInit.
 */
#include "kernel.h"
#include "xbox_memory_layout.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

typedef void (*recomp_func_t)(void);
static void worker_returns(void);
static void worker_terminates(void);
/* Two guest "start routines" for the thread-object checks. */
recomp_func_t recomp_lookup(uint32_t va)
{
    return va == 0x00010000u ? worker_returns
         : va == 0x00020000u ? worker_terminates : NULL;
}
recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va; return NULL; }

extern recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);
extern RECOMP_TLS uint32_t g_eax, g_esp, g_ecx;
extern ptrdiff_t g_xbox_mem_offset;

static int failures;

static void check(int ok, const char *what, const char *detail)
{
    printf("  %-62s %s", what, ok ? "PASS" : "FAIL");
    if (!ok && detail) printf(" -- %s", detail);
    putchar('\n');
    if (!ok) failures++;
}

#define G32(va) (*(volatile uint32_t *)((uintptr_t)(va) + g_xbox_mem_offset))
#define G16(va) (*(volatile uint16_t *)((uintptr_t)(va) + g_xbox_mem_offset))
#define G8(va)  (*(volatile uint8_t  *)((uintptr_t)(va) + g_xbox_mem_offset))
#define GP(va)  ((uint8_t *)((uintptr_t)(va) + g_xbox_mem_offset))

enum { ORD_MMALLOC = 165, ORD_MMALLOCEX = 166, ORD_MMFREE = 171,
       ORD_MMQUERYSIZE = 180, ORD_NTALLOC = 184, ORD_NTCLOSE = 187,
       ORD_NTCREATE = 190, ORD_NTFREE = 199, ORD_NTSETINFO = 226,
       ORD_KEWAIT = 159, ORD_NTDUP = 197, ORD_NTWAIT = 233, ORD_OBREF = 246,
       ORD_OBDEREF = 250, ORD_PSCREATE = 255, ORD_PSTERM = 258,
       ORD_NTWRITE = 236, ORD_NTQUERYINFO = 211 };
static const uint32_t ORDS[] = { ORD_MMALLOC, ORD_MMALLOCEX, ORD_MMFREE,
    ORD_MMQUERYSIZE, ORD_NTALLOC, ORD_NTCLOSE, ORD_NTCREATE, ORD_NTFREE,
    ORD_NTSETINFO, ORD_KEWAIT, ORD_NTDUP, ORD_NTWAIT, ORD_OBREF, ORD_OBDEREF,
    ORD_PSCREATE, ORD_PSTERM, ORD_NTWRITE, ORD_NTQUERYINFO };
#define NORDS (sizeof ORDS / sizeof ORDS[0])

static uint32_t g_thunks, g_stack, g_scratch;
static volatile int g_worker_go;

/* stdcall: return address, then the arguments. */
static uint32_t call(uint32_t ordinal, int argc, const uint32_t *argv)
{
    unsigned i;
    recomp_func_t fn = NULL;

    for (i = 0; i < NORDS; i++)
        if (ORDS[i] == ordinal)
            fn = recomp_lookup_kernel(G32(g_thunks + 4 * i));
    if (!fn) {
        printf("  ordinal %u did not resolve\n", ordinal);
        failures++;
        return 0xFFFFFFFFu;
    }
    G32(g_stack) = 0xBEEF0001u;
    for (i = 0; i < (unsigned)argc; i++)
        G32(g_stack + 4 + 4 * i) = argv[i];
    g_esp = g_stack;
    g_eax = 0xDEADBEEFu;
    fn();
    return g_eax;
}
#define CALL(ord, ...) call((ord), \
    (int)(sizeof((uint32_t[]){__VA_ARGS__}) / 4), (uint32_t[]){__VA_ARGS__})

static recomp_func_t lookup_ord(uint32_t ordinal)
{
    unsigned i;

    for (i = 0; i < NORDS; i++)
        if (ORDS[i] == ordinal)
            return recomp_lookup_kernel(G32(g_thunks + 4 * i));
    return NULL;
}

/* Runs on the worker's own thread and stack (g_esp is thread-local). */
static void worker_returns(void)
{
    g_eax = 7;
}

static void worker_terminates(void)
{
    recomp_func_t term = lookup_ord(ORD_PSTERM);

    while (!g_worker_go)
        usleep(1000);
    g_esp -= 8;
    G32(g_esp) = 0;              /* return address */
    G32(g_esp + 4) = 0x1234u;    /* ExitStatus */
    term();                      /* does not return */
}

static unsigned char *load(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    unsigned char *b;
    long len;

    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    rewind(f);
    b = malloc((size_t)len);
    if (b && fread(b, 1, (size_t)len, f) != (size_t)len) { free(b); b = NULL; }
    fclose(f);
    *n = (size_t)len;
    return b;
}

/* ---- thread handles lead to guest thread objects -------------------- */
static uint32_t spawn(uint32_t routine, uint32_t handle_va)
{
    G32(handle_va) = 0;
    /* ThreadHandle, ExtraSize, KStack, Tls, ThreadId, ctx1, ctx2,
     * CreateSuspended, DebugStack, StartRoutine */
    CALL(ORD_PSCREATE, handle_va, 0, 0, 0, 0, 0, 0, 0, 0, routine);
    return G32(handle_va);
}

static uint32_t obref(uint32_t h)
{
    G32(g_scratch + 0x200) = 0xCCCCCCCCu;
    CALL(ORD_OBREF, h, 0, g_scratch + 0x200);
    return G32(g_scratch + 0x200);
}

static void obderef(uint32_t obj)
{
    recomp_func_t fn = lookup_ord(ORD_OBDEREF);

    g_ecx = obj;
    if (fn) fn();
}

static void test_threads(void)
{
    char d[160];
    uint32_t h, h2, dup, obj, obj2, st, zero = g_scratch + 0x300;

    printf("Thread objects\n");
    xbox_SetThreadMode(XBOX_THREAD_MODE_SPAWN);
    G32(zero) = 0; G32(zero + 4) = 0;   /* a zero relative timeout */

    h = spawn(0x00020000u, g_scratch + 0x100);
    check(h != 0, "PsCreateSystemThreadEx writes a handle", NULL);
    obj = obref(h);
    snprintf(d, sizeof d, "object 0x%08X", obj);
    check(obj != 0 && obj != 0xCCCCCCCCu && G8(obj) == 6,
          "ObReferenceObjectByHandle returns a guest thread object", d);
    check(obj && G32(obj + 4) == 0 && G32(obj + 0x120) == 0x103,
          "a running thread is not signalled (STILL_ACTIVE)", NULL);
    st = CALL(ORD_NTWAIT, h, 0, zero);
    snprintf(d, sizeof d, "status 0x%08X", st);
    check(st == 0x102, "NtWaitForSingleObject times out while it runs", d);

    g_worker_go = 1;
    st = CALL(ORD_NTWAIT, h, 0, 0);
    snprintf(d, sizeof d, "status 0x%08X", st);
    check(st == 0, "NtWaitForSingleObject returns once it terminates", d);
    st = CALL(ORD_KEWAIT, obj, 0, 0, 0, 0);
    snprintf(d, sizeof d, "status 0x%08X", st);
    check(st == 0, "KeWaitForSingleObject on the object is satisfied", d);
    snprintf(d, sizeof d, "signal %u exit 0x%08X", G32(obj + 4), G32(obj + 0x120));
    check(G32(obj + 4) == 1 && G32(obj + 0x120) == 0x1234u,
          "PsTerminateSystemThread sets SignalState and ExitStatus", d);
    obderef(obj);
    CALL(ORD_NTCLOSE, h);

    h2 = spawn(0x00010000u, g_scratch + 0x104);
    G32(g_scratch + 0x108) = 0;
    CALL(ORD_NTDUP, h2, g_scratch + 0x108, 2);
    dup = G32(g_scratch + 0x108);
    st = CALL(ORD_NTWAIT, h2, 0, 0);
    check(st == 0, "a worker that returns can be waited on", NULL);
    obj2 = obref(dup);
    snprintf(d, sizeof d, "object 0x%08X signal %u exit 0x%08X", obj2,
             obj2 ? G32(obj2 + 4) : 0, obj2 ? G32(obj2 + 0x120) : 0);
    check(obj2 && obj2 != 0xCCCCCCCCu && G32(obj2 + 4) == 1 &&
          G32(obj2 + 0x120) == 0,
          "returning signals it with STATUS_SUCCESS (via a duplicate)", d);
    obderef(obj2);
    snprintf(d, sizeof d, "source 0x%08X duplicate 0x%08X", h2, dup);
    check(dup != 0 && dup != h2, "NtDuplicateObject gives a new token", d);
    CALL(ORD_NTCLOSE, h2);
    st = CALL(ORD_NTWAIT, dup, 0, 0);
    snprintf(d, sizeof d, "status 0x%08X", st);
    check(st == 0, "the duplicate outlives the first handle", d);
    obj2 = obref(dup);
    check(obj2 && obj2 != 0xCCCCCCCCu && G8(obj2) == 6,
          "the duplicate still leads to the thread object", NULL);
    obderef(obj2);
    CALL(ORD_NTCLOSE, dup);

    obj = obref(0xFFFFFFFEu);
    check(obj && obj != 0xCCCCCCCCu && G8(obj) == 6 && G32(obj + 4) == 0,
          "NtCurrentThread has an unsignalled object", NULL);
    obderef(obj);
}

int main(int argc, char **argv)
{
    char tmpl[] = "/tmp/kmembridgeXXXXXX", cache[256], cmd[300];
    size_t n = 0;
    unsigned char *xbe;
    unsigned i;

    setvbuf(stdout, NULL, _IONBF, 0);
    if (!mkdtemp(tmpl)) return 2;
    setenv("XDG_DATA_HOME", tmpl, 1);
    snprintf(cache, sizeof cache, "%s/xboxrecomp/Cache", tmpl);
    snprintf(cmd, sizeof cmd, "mkdir -p '%s'", cache);
    if (system(cmd) != 0) return 2;

    xbe = load(argc > 1 ? argv[1] : "tools/conformance/test.xbe", &n);
    if (!xbe || !xbox_MemoryLayoutInit(xbe, n)) {
        printf("cannot map guest memory (run tools/conformance/mkxbe.py)\n");
        return 2;
    }

    g_thunks  = xbox_HeapAlloc(0x1000, 4096);
    g_stack   = xbox_HeapAlloc(0x1000, 4096);
    g_scratch = xbox_HeapAlloc(0x1000, 4096);
    for (i = 0; i < NORDS; i++)
        G32(g_thunks + 4 * i) = 0x80000000u | ORDS[i];
    G32(g_thunks + 4 * NORDS) = 0;
    xbox_kernel_set_thunk_address(g_thunks, NORDS + 1);
    xbox_kernel_bridge_init();

    test_threads();

    snprintf(cmd, sizeof cmd, "rm -rf '%s'", tmpl);
    (void)system(cmd);
    printf("\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
