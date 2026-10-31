/*
 * nv2a_pb_defer.c - decode and submission on two threads.
 *
 * RECOMP_PB_DECODE_THREAD=1: the executor thread walks the push buffer and
 * decodes it as before, but its calls into the back end (clear, draw,
 * draw_raw, flip) are recorded into a ring, each with a copy of what it
 * points at, and a second thread replays them into the real back end -- so
 * the walk of the next batches overlaps the GL submission of the last ones.
 * Within a frame of VC3's mission intro that is ~800 draws whose GL time and
 * decode time used to add up on one thread.
 *
 * What a call points at, and what is copied:
 *   - everything the executor owns and goes on changing: the surface, render
 *     state, textures, combiners, the fixed-function state, the CPU path's
 *     vertices, the index list, the program and its constants, an inline
 *     batch's payload, and the batch's extent (computed now, for the batches
 *     a back end may ask it of);
 *   - not guest memory (vertex streams, textures). The title reuses those
 *     only after the GPU has said it is done with them -- GET, the semaphore
 *     its fences read, the kick-off latch, a visibility report -- and none of
 *     those reaches the title before the draws ahead of it are submitted:
 *     a semaphore release and GET's advance are queued in the ring as guest
 *     writes (nv2a_defer_write) that the submission thread makes in order,
 *     and for the latch and the reports the executor drains the ring first
 *     (nv2a_defer_sync). What the title sees of the GPU's progress is the
 *     submission thread's, as it would be a GPU's.
 * A flip, and a draw_raw with a check buffer, are replayed synchronously:
 * the executor presents the flip's resolve right after, and reads the check.
 *
 * A draw_raw is answered "taken" at once; one the real back end then
 * declines is counted ([DEFER] declined) rather than redone on the CPU,
 * whose state has moved on by then. (Over VC3's regression the GL back end
 * declines none.)
 *
 * RECOMP_PB_DECODE_THREAD=2: the same records replayed synchronously, one at
 * a time -- the copies checked with no concurrency, for comparison.
 * Slower-machine stand-ins: RECOMP_EXEC_DRAW_DELAY_US is spent by the
 * executor per program batch, as decode (unchanged by deferral);
 * RECOMP_EXEC_GL_DELAY_US per draw handed to the back end, as submission --
 * on the submission thread while deferring, inline otherwise. The Quest's
 * intro is about 6 us of decode and 13 us of submission a draw where the
 * Mac's is 2 and 3.
 */
#include "nv2a_pb_defer.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <time.h>
#include "platform/host_time.h"   /* xbox_HostNowNs */
#include "platform/thread_acct.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif


#define RING_BYTES (8u << 20)
#define REC_ALIGN 16u

enum { REC_PAD, REC_CLEAR, REC_DRAW, REC_DRAW_RAW, REC_FLIP, REC_WRITE };

typedef struct {
    uint32_t type, size;          /* size of the whole record, aligned */
} RecHdr;

typedef struct {
    RecHdr h;
    Nv2aSurface surf;
    Nv2aRenderState rs;
    uint32_t flags, argb, zstencil, has_rs;
} ClearRec;

typedef struct {
    RecHdr h;
    Nv2aSurface surf;
    Nv2aBatch b;
    Nv2aTexture tex, stage[4];
    Nv2aRenderState rs;
    Nv2aCombiner rc;
    Nv2aRawGeometry g;
    Nv2aFixedState ff;
    Nv2aRawExtent ext;
    int ext_ok;
    int *result;                  /* draw_raw's answer, for a synchronous one */
} DrawRec;

typedef struct {
    RecHdr h;
    volatile uint32_t *at;
    uint32_t value;
} WriteRec;

static const Nv2aBackend *s_real;
static Nv2aBackend s_proxy;
static int s_mode = -1;           /* 0 off, 1 threaded, 2 synchronous */
static uint8_t *s_ring;
static volatile uint64_t s_head;  /* producer: bytes published */
static volatile uint64_t s_tail;  /* consumer: bytes replayed */
static volatile int s_consumer_sleeping, s_producer_waiting;
static long s_gl_us;             /* RECOMP_EXEC_GL_DELAY_US */

static struct {
    uint64_t recs, bytes, drains, drain_ns, busy_ns, declined, max_depth;
    uint64_t sync_n[NV2A_SYNC_KINDS], sync_ns[NV2A_SYNC_KINDS], prog, writes;
} s_st;

#ifdef _WIN32
static CRITICAL_SECTION s_mu;
static CONDITION_VARIABLE s_work_cv, s_done_cv;
#define LOCK() EnterCriticalSection(&s_mu)
#define UNLOCK() LeaveCriticalSection(&s_mu)
#define WAIT_WORK() SleepConditionVariableCS(&s_work_cv, &s_mu, 2)
#define WAIT_DONE() SleepConditionVariableCS(&s_done_cv, &s_mu, 2)
#define SIGNAL_WORK() WakeConditionVariable(&s_work_cv)
#define SIGNAL_DONE() WakeConditionVariable(&s_done_cv)
#else
static pthread_mutex_t s_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_work_cv = PTHREAD_COND_INITIALIZER;
static pthread_cond_t s_done_cv = PTHREAD_COND_INITIALIZER;
#define LOCK() pthread_mutex_lock(&s_mu)
#define UNLOCK() pthread_mutex_unlock(&s_mu)
static void timed_wait(pthread_cond_t *cv)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += 2000000;            /* 2 ms: a missed wake costs no more */
    if (ts.tv_nsec >= 1000000000) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000;
    }
    pthread_cond_timedwait(cv, &s_mu, &ts);
}
#define WAIT_WORK() timed_wait(&s_work_cv)
#define WAIT_DONE() timed_wait(&s_done_cv)
#define SIGNAL_WORK() pthread_cond_signal(&s_work_cv)
#define SIGNAL_DONE() pthread_cond_signal(&s_done_cv)
#endif

#ifdef _MSC_VER   /* x64: volatile accesses are acquire/release under /volatile:ms */
static uint64_t load_acq(volatile uint64_t *p) { return *p; }
static void store_rel(volatile uint64_t *p, uint64_t v) { *p = v; }
static void store_rel32(volatile uint32_t *p, uint32_t v) { *p = v; }
#define FENCE() MemoryBarrier()
#else
static uint64_t load_acq(volatile uint64_t *p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static void store_rel(volatile uint64_t *p, uint64_t v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }
static void store_rel32(volatile uint32_t *p, uint32_t v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }
#define FENCE() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#endif

int nv2a_defer_active(void)
{
    return s_mode > 0 && s_real;
}

/* ── the submission thread ──────────────────────────────────── */

static void delay_stand_in(long us)
{
    uint64_t end;
    if (us <= 0)
        return;
    end = xbox_HostNowNs() + (uint64_t)us * 1000ull;
    while (xbox_HostNowNs() < end)
        ;
}

static int defer_extent(const Nv2aRawGeometry *g, Nv2aRawExtent *out)
{
    const DrawRec *r = (const DrawRec *)((const uint8_t *)g - offsetof(DrawRec, g));
    if (!r->ext_ok)
        return 0;
    *out = r->ext;
    return 1;
}

static void replay(RecHdr *h)
{
    switch (h->type) {
    case REC_CLEAR: {
        ClearRec *c = (ClearRec *)h;
        if (s_real->clear)
            s_real->clear(&c->surf, c->has_rs ? &c->rs : NULL, c->flags, c->argb, c->zstencil);
        break;
    }
    case REC_DRAW:
    case REC_DRAW_RAW: {
        DrawRec *d = (DrawRec *)h;
        delay_stand_in(s_gl_us);
        if (h->type == REC_DRAW) {
            if (s_real->draw)
                s_real->draw(&d->surf, &d->b);
        } else {
            int ok = s_real->draw_raw ? s_real->draw_raw(&d->surf, &d->b) : 0;
            if (d->result)
                *d->result = ok;
            else if (!ok)
                s_st.declined++;
        }
        break;
    }
    case REC_WRITE: {             /* after everything before it */
        WriteRec *w = (WriteRec *)h;
        store_rel32(w->at, w->value);
        s_st.writes++;
        break;
    }
    case REC_FLIP:
        if (s_real->flip)
            s_real->flip();
        break;
    default:
        break;
    }
}

#ifdef _WIN32
static DWORD WINAPI submit_thread(LPVOID unused)
#else
static void *submit_thread(void *unused)
#endif
{
    (void)unused;
    xbox_ThreadAcctName("nv2a submit");
    for (;;) {
        uint64_t head = load_acq(&s_head), tail = s_tail;
        if (tail == head) {
            LOCK();
            s_consumer_sleeping = 1;
            FENCE();   /* vs publish() */
            if (load_acq(&s_head) == s_tail)
                WAIT_WORK();
            s_consumer_sleeping = 0;
            UNLOCK();
            continue;
        }
        while (tail < head) {
            RecHdr *h = (RecHdr *)(s_ring + (tail % RING_BYTES));
            uint64_t t0 = xbox_HostNowNs();
            replay(h);
            s_st.busy_ns += xbox_HostNowNs() - t0;
            tail += h->size;
            store_rel(&s_tail, tail);
            FENCE();
            if (s_producer_waiting) {
                LOCK();
                SIGNAL_DONE();
                UNLOCK();
            }
        }
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

/* ── the executor's side ────────────────────────────────────── */

/* Wait until the consumer has replayed up to `until`. */
static void wait_tail(uint64_t until)
{
    int spins = 0;
    while (load_acq(&s_tail) < until) {
        if (++spins < 64)
            continue;
        LOCK();
        s_producer_waiting = 1;
        FENCE();       /* vs the consumer's tail store */
        if (load_acq(&s_tail) < until)
            WAIT_DONE();
        s_producer_waiting = 0;
        UNLOCK();
    }
}

static const char *const s_sync_name[NV2A_SYNC_KINDS] = {
    "other", "semaphore", "kick-off", "report clear", "report", "GET", "flip",
};

void nv2a_defer_sync(int why)
{
    static int skip = -1;
    uint64_t head, t0, dt;
    if (!nv2a_defer_active())
        return;
    if (skip < 0) {
        const char *e = getenv("RECOMP_PB_DEFER_NOSYNC");   /* experiments only */
        skip = e ? (int)strtol(e, NULL, 0) : 0;
    }
    if (why <= 0 || why >= NV2A_SYNC_KINDS)
        why = NV2A_SYNC_OTHER;
    if (skip & (1 << why))
        return;
    head = s_head;
    if (load_acq(&s_tail) >= head)
        return;
    t0 = xbox_HostNowNs();
    wait_tail(head);
    dt = xbox_HostNowNs() - t0;
    s_st.drains++;
    s_st.drain_ns += dt;
    s_st.sync_n[why]++;
    s_st.sync_ns[why] += dt;
}

void nv2a_defer_drain(void)
{
    nv2a_defer_sync(NV2A_SYNC_OTHER);
}

/* Room for `bytes` (aligned) at the ring's write position, contiguous. */
static uint8_t *reserve(uint32_t bytes)
{
    uint64_t pos = s_head;
    uint32_t off = (uint32_t)(pos % RING_BYTES);
    if (off + bytes > RING_BYTES) {             /* pad to the end, start again */
        uint32_t pad = RING_BYTES - off;
        wait_tail(pos + pad + bytes - RING_BYTES);
        ((RecHdr *)(s_ring + off))->type = REC_PAD;
        ((RecHdr *)(s_ring + off))->size = pad;
        pos += pad;
        store_rel(&s_head, pos);
        off = 0;
    }
    wait_tail(pos + bytes > RING_BYTES ? pos + bytes - RING_BYTES : 0);
    return s_ring + off;
}

static void publish(RecHdr *h)
{
    uint64_t depth;
    s_st.recs++;
    s_st.bytes += h->size;
    store_rel(&s_head, s_head + h->size);
    FENCE();   /* then look for a sleeping consumer */
    depth = s_head - load_acq(&s_tail);
    if (depth > s_st.max_depth)
        s_st.max_depth = depth;
    if (s_consumer_sleeping) {
        LOCK();
        SIGNAL_WORK();
        UNLOCK();
    }
    if (s_mode == 2)
        nv2a_defer_drain();
}

static uint32_t align(uint32_t n) { return (n + REC_ALIGN - 1) & ~(REC_ALIGN - 1); }

/* Bytes to copy for an attribute stream that is not guest memory (an inline
 * batch's payload); 0 for a guest stream, used where it is. */
static uint32_t attr_copy_bytes(const Nv2aAttrib *a)
{
    return a->data && !a->guest_va ? a->bytes : 0;
}

static void p_clear(const Nv2aSurface *s, const Nv2aRenderState *rs, uint32_t flags,
                    uint32_t argb, uint32_t zstencil)
{
    uint32_t size = align(sizeof(ClearRec));
    ClearRec *c = (ClearRec *)reserve(size);
    c->h.type = REC_CLEAR;
    c->h.size = size;
    c->surf = *s;
    if (rs)
        c->rs = *rs;
    else
        memset(&c->rs, 0, sizeof c->rs);
    c->has_rs = rs != NULL;
    c->flags = flags;
    c->argb = argb;
    c->zstencil = zstencil;
    publish(&c->h);
}

static DrawRec *record_draw(const Nv2aSurface *s, const Nv2aBatch *b, int raw)
{
    const Nv2aRawGeometry *g = b->raw;   /* also on a draw(): a back end may look at it */
    uint32_t size = align(sizeof(DrawRec)), off, i;
    uint32_t vbytes = 0, xbytes = 0, ibytes = 0, cbytes = 0, pbytes = 0, abytes[16];
    DrawRec *d;
    uint8_t *p;

    if (b->vertices && b->count)
        vbytes = align(b->count * (uint32_t)sizeof(Nv2aVertex));
    if (b->ext && b->count)
        xbytes = align(b->count * (uint32_t)sizeof(Nv2aVertexEx));
    if (g) {
        ibytes = align(g->index_count * 2u);
        for (i = 0; i < 16; i++)
            abytes[i] = align(attr_copy_bytes(&g->attr[i]));
        if (g->kind == NV2A_RAW_PROGRAM) {
            s_st.prog++;
            if (g->constants)
                cbytes = align(192u * 16u);
            if (g->program)
                pbytes = align((g->program_start + g->program_len) * 16u);
        }
    } else {
        memset(abytes, 0, sizeof abytes);
    }
    size += vbytes + xbytes + ibytes + cbytes + pbytes;
    for (i = 0; i < 16; i++)
        size += abytes[i];
    d = (DrawRec *)reserve(size);
    d->h.type = raw ? REC_DRAW_RAW : REC_DRAW;
    d->h.size = size;
    d->surf = *s;
    d->b = *b;
    d->result = NULL;
    d->ext_ok = 0;
    if (b->texture) {
        d->tex = *b->texture;
        d->b.texture = &d->tex;
    }
    for (i = 0; i < 4; i++)
        if (b->stage[i]) {
            d->stage[i] = *b->stage[i];
            d->b.stage[i] = &d->stage[i];
        }
    if (b->state) {
        d->rs = *b->state;
        d->b.state = &d->rs;
    }
    if (b->rc) {
        d->rc = *b->rc;
        d->b.rc = &d->rc;
    }
    p = (uint8_t *)d;
    off = align(sizeof(DrawRec));
    if (vbytes) {
        memcpy(p + off, b->vertices, b->count * sizeof(Nv2aVertex));
        d->b.vertices = (const Nv2aVertex *)(p + off);
        off += vbytes;
    }
    if (xbytes) {
        memcpy(p + off, b->ext, b->count * sizeof(Nv2aVertexEx));
        d->b.ext = (const Nv2aVertexEx *)(p + off);
        off += xbytes;
    }
    if (g) {
        d->g = *g;
        d->b.raw = &d->g;
        if (g->ff) {
            d->ff = *g->ff;
            d->g.ff = &d->ff;
        }
        if (ibytes) {
            memcpy(p + off, g->indices, g->index_count * 2u);
            d->g.indices = (const uint16_t *)(p + off);
            off += ibytes;
        }
        for (i = 0; i < 16; i++)
            if (abytes[i]) {
                memcpy(p + off, g->attr[i].data, g->attr[i].bytes);
                d->g.attr[i].data = p + off;
                off += abytes[i];
            }
        if (cbytes) {
            memcpy(p + off, g->constants, 192u * 16u);
            d->g.constants = (const float (*)[4])(p + off);
            off += cbytes;
        }
        if (pbytes) {
            memcpy(p + off, g->program, (g->program_start + g->program_len) * 16u);
            d->g.program = (const uint32_t (*)[4])(p + off);
            off += pbytes;
        }
        /* The extent reads the executor's state as it is now: computed here
         * for the batches a back end may ask it of (screen-space programs,
         * and those whose first vertex is behind the eye). */
        if (g->extent) {
            if (g->kind == NV2A_RAW_PROGRAM && !(g->w0 > 0.0f && g->w0 != 1.0f))
                d->ext_ok = g->extent(g, &d->ext);
            d->g.extent = defer_extent;
        }
    }
    return d;
}

static void p_draw(const Nv2aSurface *s, const Nv2aBatch *b)
{
    DrawRec *d = record_draw(s, b, 0);
    publish(&d->h);
}

static int p_draw_raw(const Nv2aSurface *s, const Nv2aBatch *b)
{
    DrawRec *d = record_draw(s, b, 1);
    int result = 1;
    if (b->raw->check_out) {             /* the executor reads it back now */
        d->result = &result;
        publish(&d->h);
        nv2a_defer_drain();
        return result;
    }
    publish(&d->h);
    return 1;
}

int nv2a_defer_write(volatile uint32_t *at, uint32_t value)
{
    uint32_t size = align(sizeof(WriteRec));
    WriteRec *w;
    if (!nv2a_defer_active())
        return 0;
    w = (WriteRec *)reserve(size);
    w->h.type = REC_WRITE;
    w->h.size = size;
    w->at = at;
    w->value = value;
    publish(&w->h);
    return 1;
}

static void p_flip(void)
{
    uint32_t size = align(sizeof(RecHdr));
    RecHdr *h = (RecHdr *)reserve(size);
    h->type = REC_FLIP;
    h->size = size;
    publish(h);
    nv2a_defer_sync(NV2A_SYNC_FLIP);     /* the present reads its resolve next */

    if (getenv("RECOMP_PROF")) {         /* once a second, with the executor's */
        static uint64_t last;
        uint64_t now = xbox_HostNowNs();
        if (!last)
            last = now;
        if (now - last >= 1000000000ull) {
            fprintf(stderr, "[DEFER] %.2f s: %llu records, %.1f MB copied, submit busy %.0f ms,"
                    " %llu drains waiting %.0f ms, deepest %.1f MB, %llu declined\n",
                    (now - last) / 1e9, (unsigned long long)s_st.recs, s_st.bytes / 1e6,
                    s_st.busy_ns / 1e6, (unsigned long long)s_st.drains, s_st.drain_ns / 1e6,
                    s_st.max_depth / 1e6, (unsigned long long)s_st.declined);
            {
                char line[512];
                int n = 0, k;
                for (k = 0; k < NV2A_SYNC_KINDS; k++)
                    if (s_st.sync_n[k])
                        n += snprintf(line + n, sizeof line - (size_t)n, " %s %llu (%.0f ms)",
                                      s_sync_name[k], (unsigned long long)s_st.sync_n[k],
                                      s_st.sync_ns[k] / 1e6);
                line[n] = 0;
                fprintf(stderr, "[DEFER]   drains:%s; %llu program draws, %llu semaphore"
                        " writes queued\n", line, (unsigned long long)s_st.prog,
                        (unsigned long long)s_st.writes);
            }
            memset(&s_st, 0, sizeof s_st);
            last = now;
        }
    }
}

/* RECOMP_EXEC_GL_DELAY_US without deferral: the same wait, inline. */
static void d_draw(const Nv2aSurface *s, const Nv2aBatch *b)
{
    delay_stand_in(s_gl_us);
    s_real->draw(s, b);
}

static int d_draw_raw(const Nv2aSurface *s, const Nv2aBatch *b)
{
    delay_stand_in(s_gl_us);
    return s_real->draw_raw(s, b);
}

const Nv2aBackend *nv2a_defer_wrap(const Nv2aBackend *real)
{
    if (s_mode < 0) {
        const char *e = getenv("RECOMP_PB_DECODE_THREAD");
        const char *g = getenv("RECOMP_EXEC_GL_DELAY_US");
        s_mode = e ? atoi(e) : 0;
        if (s_mode < 0 || s_mode > 2)
            s_mode = 0;
        s_gl_us = g ? atol(g) : 0;
    }
    if (real && !s_mode && s_gl_us > 0) {
        s_real = real;
        s_proxy = *real;
        s_proxy.draw = real->draw ? d_draw : NULL;
        s_proxy.draw_raw = real->draw_raw ? d_draw_raw : NULL;
        return &s_proxy;
    }
    if (!real || !s_mode) {
        s_real = real;
        return real;
    }
    if (!s_ring) {
        s_ring = (uint8_t *)malloc(RING_BYTES);
        if (!s_ring) {
            s_mode = 0;
            return real;
        }
#ifdef _WIN32
        InitializeCriticalSection(&s_mu);
        InitializeConditionVariable(&s_work_cv);
        InitializeConditionVariable(&s_done_cv);
        CloseHandle(CreateThread(NULL, 0, submit_thread, NULL, 0, NULL));
#else
        {
            pthread_t th;
            pthread_create(&th, NULL, submit_thread, NULL);
            pthread_detach(th);
        }
#endif
        fprintf(stderr, "[DEFER] push-buffer decode and GL submission on two threads"
                " (RECOMP_PB_DECODE_THREAD=%d%s)\n", s_mode,
                s_mode == 2 ? ": each call replayed before the next" : "");
    }
    s_real = real;
    s_proxy.clear = real->clear ? p_clear : NULL;
    s_proxy.draw = real->draw ? p_draw : NULL;
    s_proxy.flip = real->flip ? p_flip : NULL;
    s_proxy.draw_raw = real->draw_raw ? p_draw_raw : NULL;
    return &s_proxy;
}
