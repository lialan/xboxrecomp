/*
 * thread_acct.h - per-thread CPU and wait time, once a second
 * (RECOMP_THREAD_LOG=1; thread_acct.c).
 */
#ifndef XBOX_THREAD_ACCT_H
#define XBOX_THREAD_ACCT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { XBOX_ACCT_OBJECT, XBOX_ACCT_SLEEP, XBOX_ACCT_KINDS };

/* -1 not yet read, 0 off, 1 on. */
extern int g_thread_acct_on;

/* Reads RECOMP_THREAD_LOG once (starting the reporter); returns 1 when on. */
int xbox_ThreadAcctInit(void);
/* Names the calling thread in the report (and registers it). */
void xbox_ThreadAcctName(const char *name);
/* Time the calling thread just spent blocked (XBOX_ACCT_OBJECT) or asleep. */
void xbox_ThreadAcctAdd(int kind, uint64_t ns);
/* A GPU write-back flush acknowledged, at most ns after it was asked for. */
void xbox_ThreadAcctKick(uint64_t ns);
/* The calling thread's name in the report ("unnamed" if it has none). */
const char *xbox_ThreadAcctSelfName(void);

/* Around a wait: a load and a branch while the log is off. */
#define XBOX_ACCT_BEGIN() \
    uint64_t _acct_t0 = (g_thread_acct_on > 0 || (g_thread_acct_on < 0 && xbox_ThreadAcctInit() > 0)) \
                        ? xbox_HostNowNs() : 0
#define XBOX_ACCT_END(kind) \
    do { if (_acct_t0) xbox_ThreadAcctAdd((kind), xbox_HostNowNs() - _acct_t0); } while (0)

#ifdef __cplusplus
}
#endif
#endif
