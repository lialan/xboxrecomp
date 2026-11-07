/*
 * host_stacks.c - every host thread's native call stack, on demand (Linux and
 * Android; a no-op elsewhere).
 *
 * RECOMP_STALL_DUMP shows each guest thread's last kernel call and its guest
 * stack, which says nothing about a thread blocked or spinning in host code
 * between kernel calls -- a host lock, a fault handler, a replaced function.
 * This signals every thread of the process (/proc/self/task) with a spare
 * real-time signal; each unwinds itself into a slot, and the caller prints the
 * slots as module+offset (llvm-addr2line -f -e <unstripped .so> 0x...) with
 * the thread's name. Debuggerd needs a debuggable app; this does not.
 */
#include "host_stacks.h"

#if defined(__linux__)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dirent.h>
#include <dlfcn.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/ucontext.h>
#include <time.h>
#include <unistd.h>
#include <unwind.h>

#define HS_MAX_THREADS 128
#define HS_DEPTH 24

typedef struct {
    volatile int done;
    int tid, n;
    uintptr_t pc[HS_DEPTH];
} HsSlot;

static HsSlot s_slots[HS_MAX_THREADS];
static volatile int s_next;

typedef struct { uintptr_t *pc; int n; } HsWalk;

static _Unwind_Reason_Code hs_step(struct _Unwind_Context *ctx, void *arg)
{
    HsWalk *w = (HsWalk *)arg;
    uintptr_t ip = (uintptr_t)_Unwind_GetIP(ctx);
    if (!ip)
        return _URC_END_OF_STACK;
    w->pc[w->n++] = ip;
    return w->n >= HS_DEPTH ? _URC_END_OF_STACK : _URC_NO_REASON;
}

static void hs_handler(int sig, siginfo_t *si, void *uc)
{
    (void)sig;
    int i = (int)(intptr_t)si->si_value.sival_int;
    if (i < 0 || i >= HS_MAX_THREADS)
        return;
    /* the interrupted pc and return address first: the unwinder cannot
     * always step through the signal frame */
    int n = 0;
#if defined(__aarch64__)
    const ucontext_t *u = (const ucontext_t *)uc;
    s_slots[i].pc[n++] = (uintptr_t)u->uc_mcontext.pc;
    s_slots[i].pc[n++] = (uintptr_t)u->uc_mcontext.regs[30];
#elif defined(__x86_64__)
    const ucontext_t *u = (const ucontext_t *)uc;
    s_slots[i].pc[n++] = (uintptr_t)u->uc_mcontext.gregs[REG_RIP];
    s_slots[i].pc[n++] = 0;
#else
    (void)uc;
#endif
    HsWalk w = { s_slots[i].pc + n, 0 };
    _Unwind_Backtrace(hs_step, &w);
    s_slots[i].n = n + w.n;
    __atomic_store_n(&s_slots[i].done, 1, __ATOMIC_RELEASE);
}

static void hs_name(int tid, char *out, size_t cap)
{
    char path[64];
    snprintf(path, sizeof path, "/proc/self/task/%d/comm", tid);
    FILE *f = fopen(path, "r");
    out[0] = 0;
    if (f) {
        if (fgets(out, (int)cap, f))
            out[strcspn(out, "\n")] = 0;
        fclose(f);
    }
}

void xbox_host_stacks_dump(void)
{
    static int sig;
    if (!sig) {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = hs_handler;
        sa.sa_flags = SA_SIGINFO | SA_RESTART;
        sigemptyset(&sa.sa_mask);
        sig = SIGRTMIN + 7;
        if (sigaction(sig, &sa, NULL) != 0) {
            fprintf(stderr, "[STACKS] sigaction failed\n");
            sig = 0;
            return;
        }
    }
    memset(s_slots, 0, sizeof s_slots);
    s_next = 0;
    const int self = (int)syscall(SYS_gettid), pid = getpid();
    DIR *d = opendir("/proc/self/task");
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d)) && s_next < HS_MAX_THREADS) {
        int tid = atoi(e->d_name);
        if (tid <= 0 || tid == self)
            continue;
        int i = s_next++;
        s_slots[i].tid = tid;
        siginfo_t info;
        memset(&info, 0, sizeof info);
        info.si_signo = sig;
        info.si_code = SI_QUEUE;
        info.si_pid = pid;
        info.si_uid = getuid();
        info.si_value.sival_int = i;
        if (syscall(SYS_rt_tgsigqueueinfo, pid, tid, sig, &info) != 0)
            s_slots[i].done = -1;
    }
    closedir(d);
    /* each answers within its next scheduling slice; a thread blocked with
     * signals masked does not answer at all */
    struct timespec ts = { 0, 20000000 };
    for (int k = 0; k < 25; k++) {
        int all = 1;
        for (int i = 0; i < s_next; i++)
            all &= __atomic_load_n(&s_slots[i].done, __ATOMIC_ACQUIRE) != 0;
        if (all)
            break;
        nanosleep(&ts, NULL);
    }
    fprintf(stderr, "[STACKS] %d threads\n", s_next);
    for (int i = 0; i < s_next; i++) {
        char name[32];
        hs_name(s_slots[i].tid, name, sizeof name);
        fprintf(stderr, "[STACKS] tid %d %-16s", s_slots[i].tid, name);
        if (s_slots[i].done != 1) {
            fprintf(stderr, " (no answer)\n");
            continue;
        }
        for (int k = 0; k < s_slots[i].n; k++) {
            Dl_info di;
            void *p = (void *)s_slots[i].pc[k];
            if (k == 2)
                fputs(" |", stderr);   /* the interrupted pc, lr | the unwound frames */
            if (dladdr(p, &di) && di.dli_fbase) {
                const char *m = strrchr(di.dli_fname, '/');
                fprintf(stderr, " %s+%#lx", m ? m + 1 : di.dli_fname,
                        (unsigned long)(s_slots[i].pc[k] - (uintptr_t)di.dli_fbase));
            } else
                fprintf(stderr, " %p", p);
        }
        fputc('\n', stderr);
    }
    fflush(stderr);
}
#else
void xbox_host_stacks_dump(void) {}
#endif
