/*
 * nv2a_pb_defer.h - the push-buffer decode and the back end's submission on
 * two threads (RECOMP_PB_DECODE_THREAD). See nv2a_pb_defer.c.
 */
#ifndef NV2A_PB_DEFER_H
#define NV2A_PB_DEFER_H

#include "nv2a_backend.h"

/* The back end the executor should call: `real` itself when deferral is
 * off, otherwise a proxy that records each call for the submission thread. */
const Nv2aBackend *nv2a_defer_wrap(const Nv2aBackend *real);

/* 1 while calls are being deferred. */
int nv2a_defer_active(void);

/* Wait until every call recorded so far has been submitted. The executor
 * calls it before anything the title can see -- a semaphore release, the
 * kick-off latch, a visibility report, GET -- so those still follow the
 * draws before them, as on the GPU. A no-op when deferral is off. */
void nv2a_defer_drain(void);

/* nv2a_defer_drain(), naming why (for the [DEFER] counts). */
enum {
    NV2A_SYNC_OTHER, NV2A_SYNC_SEMAPHORE, NV2A_SYNC_KICKOFF, NV2A_SYNC_REPORT_CLEAR,
    NV2A_SYNC_REPORT, NV2A_SYNC_GET, NV2A_SYNC_FLIP, NV2A_SYNC_KINDS
};
void nv2a_defer_sync(int why);

/* While deferring, queue a guest word's write behind the calls recorded so
 * far (a semaphore release) and return 1; otherwise 0, and the caller
 * writes it. */
int nv2a_defer_write(volatile uint32_t *at, uint32_t value);

#endif
