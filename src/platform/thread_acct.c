/*
 * thread_acct.c - where each thread's time goes, once a second
 * (RECOMP_THREAD_LOG=1).
 *
 * A title that misses frames on a slow machine is limited by one of three
 * things: a guest thread's own CPU work, its waits (for the GPU, a vblank,
 * another thread), or the host threads that feed it (the push-buffer
 * executor, the display). The frame rate alone does not say which. This
 * keeps, per thread, the time it spent blocked on an object (an event,
 * semaphore, mutex, condition: every guest kernel wait ends in one) and
 * asleep (Sleep, xbox_HostSleepNs, a yield), reads its CPU time from the
 * system, and prints the threads that did anything:
 *
 *   [THREADS] guest main cpu 702 obj 210 sleep 0 | nv2a exec cpu 310 ...
 *
 * all in ms per second of wall time. CPU spent spinning on guest memory --
 * D3D's kick-off waiting for the GPU's write-back flush, a vblank-counter
 * poll -- is CPU here; the kick-off part is measured from the other side
 * (xbox_ThreadAcctKick: how long each acknowledgement could have been
 * waited for) and printed with it.
 *
 * Off (the default) it costs one load and a branch at each wait. Threads
 * register themselves at their first wait or when named
 * (xbox_ThreadAcctName); up to 64.
 */
#include "thread_acct.h"
#include "host_time.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#endif
#endif

#if defined(_MSC_VER)
#define ACCT_TLS __declspec(thread)
#define ACCT_FETCH_ADD32(p, v) InterlockedExchangeAdd((volatile LONG *)(p), (LONG)(v))
#define ACCT_ADD64(p, v) InterlockedExchangeAdd64((volatile LONG64 *)(p), (LONG64)(v))
#define ACCT_FENCE() MemoryBarrier()
#else
#define ACCT_TLS __thread
#define ACCT_FETCH_ADD32(p, v) __sync_fetch_and_add((p), (v))
#define ACCT_ADD64(p, v) __sync_fetch_and_add((p), (v))
#define ACCT_FENCE() __sync_synchronize()
#endif

#define ACCT_SLOTS 64

typedef struct {
    char name[32];
    volatile uint64_t wait_ns[XBOX_ACCT_KINDS];
    uint64_t last_wait[XBOX_ACCT_KINDS], last_cpu;
    int used;
#if defined(_WIN32)
    HANDLE th;
#else
    pthread_t th;
#endif
} AcctSlot;

int g_thread_acct_on = -1;
static AcctSlot s_slot[ACCT_SLOTS];
static volatile int s_nslots;
static ACCT_TLS AcctSlot *t_slot;
static volatile uint64_t s_kick_ns, s_kick_n;

static AcctSlot *acct_self(void)
{
    if (!t_slot) {
        int i = (int)ACCT_FETCH_ADD32(&s_nslots, 1);
        if (i >= ACCT_SLOTS)
            return NULL;
        t_slot = &s_slot[i];
        snprintf(t_slot->name, sizeof t_slot->name, "thread %d", i);
#if defined(_WIN32)
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                        &t_slot->th, 0, FALSE, DUPLICATE_SAME_ACCESS);
#else
        t_slot->th = pthread_self();
#endif
        ACCT_FENCE();
        t_slot->used = 1;
    }
    return t_slot;
}

static uint64_t thread_cpu_ns(const AcctSlot *s)
{
#if defined(_WIN32)
    FILETIME c, e, k, u;
    if (!GetThreadTimes(s->th, &c, &e, &k, &u))
        return 0;
    return ((((uint64_t)k.dwHighDateTime << 32) | k.dwLowDateTime)
            + (((uint64_t)u.dwHighDateTime << 32) | u.dwLowDateTime)) * 100ull;
#elif defined(__APPLE__)
    thread_basic_info_data_t info;
    mach_msg_type_number_t n = THREAD_BASIC_INFO_COUNT;
    if (thread_info(pthread_mach_thread_np(s->th), THREAD_BASIC_INFO,
                    (thread_info_t)&info, &n) != KERN_SUCCESS)
        return 0;
    return (uint64_t)info.user_time.seconds * 1000000000ull
         + (uint64_t)info.user_time.microseconds * 1000ull
         + (uint64_t)info.system_time.seconds * 1000000000ull
         + (uint64_t)info.system_time.microseconds * 1000ull;
#else
    clockid_t cid;
    struct timespec ts;
    if (pthread_getcpuclockid(s->th, &cid) || clock_gettime(cid, &ts))
        return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

#if defined(_WIN32)
static DWORD WINAPI acct_reporter(LPVOID arg)
#else
static void *acct_reporter(void *arg)
#endif
{
    uint64_t last = xbox_HostNowNs();
    (void)arg;
    for (;;) {
        char line[1536];
        size_t len = 0;
        uint64_t now, wall, kn, kc;
        static uint64_t last_kick_ns, last_kick_n;
        int i, n;

        /* not xbox_HostSleepNs: that is accounted, and this would report itself */
#if defined(_WIN32)
        Sleep(1000);
#else
        {
            struct timespec ts = { 1, 0 };
            nanosleep(&ts, NULL);
        }
#endif
        now = xbox_HostNowNs();
        wall = now - last;
        last = now;
        n = s_nslots < ACCT_SLOTS ? s_nslots : ACCT_SLOTS;
        len += (size_t)snprintf(line + len, sizeof line - len, "[THREADS] ms/s:");
        for (i = 0; i < n; i++) {
            AcctSlot *s = &s_slot[i];
            uint64_t cpu, w0, w1;
            double k = 1000.0 / ((double)wall / 1e6);   /* ms -> ms per second */
            if (!s->used)
                continue;
            cpu = thread_cpu_ns(s);
            w0 = s->wait_ns[XBOX_ACCT_OBJECT];
            w1 = s->wait_ns[XBOX_ACCT_SLEEP];
            {
                double c = (cpu - s->last_cpu) / 1e6 * k;
                double o = (w0 - s->last_wait[XBOX_ACCT_OBJECT]) / 1e6 * k;
                double z = (w1 - s->last_wait[XBOX_ACCT_SLEEP]) / 1e6 * k;
                s->last_cpu = cpu;
                s->last_wait[XBOX_ACCT_OBJECT] = w0;
                s->last_wait[XBOX_ACCT_SLEEP] = w1;
                if ((c >= 10.0 || o >= 10.0 || z >= 10.0) && len < sizeof line - 96)
                    len += (size_t)snprintf(line + len, sizeof line - len,
                                            " | %s cpu %.0f obj %.0f sleep %.0f",
                                            s->name, c, o, z);
            }
        }
        kn = s_kick_n - last_kick_n;
        kc = s_kick_ns - last_kick_ns;
        last_kick_n = s_kick_n;
        last_kick_ns = s_kick_ns;
        if (len < sizeof line - 64)
            snprintf(line + len, sizeof line - len, " | kick-off acks %llu, waited <= %.1f ms",
                     (unsigned long long)kn, kc / 1e6);
        fprintf(stderr, "%s\n", line);
        fflush(stderr);
    }
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

static void acct_initialize(void)
{
    const char *e=getenv("RECOMP_THREAD_LOG");
    int enabled=e && *e=='1';
    if(enabled) {
#if defined(_WIN32)
        HANDLE reporter=CreateThread(NULL,0,acct_reporter,NULL,0,NULL);
        if(reporter) CloseHandle(reporter);else enabled=0;
#else
        pthread_t t;
        if(!pthread_create(&t,NULL,acct_reporter,NULL)) pthread_detach(t);else enabled=0;
#endif
        if(enabled) fprintf(stderr,"[THREADS] per-thread CPU and wait time, once a second (RECOMP_THREAD_LOG=1)\n");
    }
#if defined(_MSC_VER)
    _InterlockedExchange((volatile long *)&g_thread_acct_on,enabled);
#else
    __atomic_store_n(&g_thread_acct_on,enabled,__ATOMIC_RELEASE);
#endif
}
#if defined(_WIN32)
static INIT_ONCE acct_once=INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK acct_initialize_once(PINIT_ONCE once,PVOID parameter,PVOID *context) {
    (void)once;(void)parameter;(void)context;acct_initialize();return TRUE;
}
#else
static pthread_once_t acct_once=PTHREAD_ONCE_INIT;
#endif
int xbox_ThreadAcctInit(void)
{
#if defined(_WIN32)
    InitOnceExecuteOnce(&acct_once,acct_initialize_once,NULL,NULL);
#else
    pthread_once(&acct_once,acct_initialize);
#endif
    return xbox_ThreadAcctEnabled();
}

void (*g_xbox_thread_named)(const char *name);

void xbox_ThreadAcctName(const char *name)
{
    AcctSlot *s;
    if (g_xbox_thread_named)
        g_xbox_thread_named(name);
    if (xbox_ThreadAcctInit() <= 0 || !(s = acct_self()))
        return;
    snprintf(s->name, sizeof s->name, "%s", name);
}

void xbox_ThreadAcctAdd(int kind, uint64_t ns)
{
    AcctSlot *s = acct_self();
    if (s && kind >= 0 && kind < XBOX_ACCT_KINDS)
        s->wait_ns[kind] += ns;
}

const char *xbox_ThreadAcctSelfName(void)
{
    return t_slot ? t_slot->name : "unnamed";
}

void xbox_ThreadAcctKick(uint64_t ns)
{
    ACCT_ADD64(&s_kick_ns, ns);
    ACCT_ADD64(&s_kick_n, 1);
}
