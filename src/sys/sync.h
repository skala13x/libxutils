/*!
 *  @file libxutils/src/sys/sync.h
 *
 *  This source is part of the "libxutils" project
 *  2015-2020  Sandro Kalatozishvili (s.kalatoz@gmail.com)
 *
 * @brief Implementation of the various cross-platform
 * sync functionality with the POSIX mutex and SynchAPI
 * critical section, rwlocks, atomic built-ins, and etc.
 */

#ifndef __XUTILS_XSYNC_H__
#define __XUTILS_XSYNC_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "xstd.h"

/* The lifecycle of a lock is its owner's to order: XSync_Init/Destroy and XRWSync_Init/Destroy must not run while
   any other thread can lock or unlock it. bEnabled tells a lock that was never set up, or was torn down, from one
   in use, for a caller that does so in order; it cannot make a lock destroyed under another thread safe to use. */
typedef struct XSyncMutex {
#ifdef _WIN32
    CRITICAL_SECTION mutex;
#else
    pthread_mutex_t mutex;
#endif
    xbool_t bEnabled;
} xsync_mutex_t;

typedef struct XSyncRW {
#ifdef _WIN32
    xbool_t bExclusive;
    SRWLOCK rwLock;
#else
    pthread_rwlock_t rwLock;
#endif
    xbool_t bEnabled;
} xsync_rw_t;

/* A handshake between a controller and one thread. The controller raises the barrier (XSyncBar_Bar), waits for
   the acknowledgement (XSyncBar_WaitAck or XSyncBar_WaitAckFor), and lowers it (XSyncBar_Reset), as often as it
   likes and as quickly. The thread waits for the barrier (XSyncBar_CheckBar), does what it is for, acknowledges it
   (XSyncBar_Ack), and waits while XSyncBar_CheckAck() holds before it looks for the next one: that stops holding as
   soon as the barrier is lowered or another one raised, however soon after. Every barrier is a value of its own,
   so an acknowledgement is for the barrier it saw and for no other. */
typedef struct XSyncBar {
    xvolatile_t nBar;
    xvolatile_t nAck;
} xsync_bar_t;

#ifdef _WIN32
#define XSYNC_ATOMIC_ADD(dst,val) (InterlockedExchangeAdd(dst, val) + (val))
#define XSYNC_ATOMIC_SUB(dst,val) (InterlockedExchangeAdd(dst, -(val)) - (val))
#define XSYNC_ATOMIC_SET(dst,val) InterlockedExchange(dst, val)
#define XSYNC_ATOMIC_GET(dst) InterlockedExchangeAdd(dst, 0)
#define XSYNC_ATOMIC_CAS(dst,oldval,newval) (InterlockedCompareExchange(dst, newval, oldval) == (oldval))
#else
#define XSYNC_ATOMIC_ADD(dst,val) __sync_add_and_fetch(dst, val)
#define XSYNC_ATOMIC_SUB(dst,val) __sync_sub_and_fetch(dst, val)
#define XSYNC_ATOMIC_SET(dst,val) __atomic_exchange_n(dst, val, __ATOMIC_SEQ_CST)
#define XSYNC_ATOMIC_GET(dst) __sync_add_and_fetch(dst, 0)
#define XSYNC_ATOMIC_CAS(dst,oldval,newval) __sync_bool_compare_and_swap(dst, oldval, newval)
#endif

void xusleep(uint32_t nUsecs);
void xyield(void);

XSTATUS XSync_InitAdv(xsync_mutex_t *pSync, xbool_t bRecursive);
void XSync_InitRecursive(xsync_mutex_t *pSync);
void XSync_Init(xsync_mutex_t *pSync);
void XSync_Destroy(xsync_mutex_t *pSync);
void XSync_Lock(xsync_mutex_t *pSync);
void XSync_Unlock(xsync_mutex_t *pSync);

void XRWSync_Init(xsync_rw_t *pSync);
void XRWSync_ReadLock(xsync_rw_t *pSync);
void XRWSync_WriteLock(xsync_rw_t *pSync);
void XRWSync_Unlock(xsync_rw_t *pSync);
void XRWSync_ReadUnlock(xsync_rw_t *pSync);
void XRWSync_WriteUnlock(xsync_rw_t *pSync);
void XRWSync_Destroy(xsync_rw_t *pSync);

void XSyncBar_Bar(xsync_bar_t *pBar);
void XSyncBar_Ack(xsync_bar_t *pBar);
void XSyncBar_Reset(xsync_bar_t *pBar);
uint8_t XSyncBar_CheckBar(xsync_bar_t *pBar);
uint8_t XSyncBar_CheckAck(xsync_bar_t *pBar);
uint32_t XSyncBar_WaitAck(xsync_bar_t *pBar, uint32_t nSleepUsec);
XSTATUS XSyncBar_WaitAckFor(xsync_bar_t *pBar, uint32_t nSleepUsec, uint32_t nTimeoutUsec);

#ifdef __cplusplus
}
#endif

#endif /* __XUTILS_XSYNC_H__ */
