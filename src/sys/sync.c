/*!
 *  @file libxutils/src/sys/sync.c
 *
 *  This source is part of the "libxutils" project
 *  2015-2020  Sandro Kalatozishvili (s.kalatoz@gmail.com)
 *
 * @brief Implementation of the various cross-platform
 * sync functionality with the POSIX mutex and SynchAPI
 * critical section, atomic builtins, rwlocks, and etc.
 */

#include "sync.h"

#ifndef _WIN32
#include <sched.h>
#include <time.h>
#endif

#if !defined(_WIN32) && !defined(PTHREAD_MUTEX_RECURSIVE)
#define PTHREAD_MUTEX_RECURSIVE PTHREAD_MUTEX_RECURSIVE_NP
#endif

void xusleep(uint32_t nUsecs)
{
#ifdef _WIN32
    DWORD nMilliSec = 0;
    if (nUsecs < 1000) nMilliSec = 1;
    else nMilliSec = nUsecs / 1000;
    Sleep(nMilliSec);
#else
    usleep(nUsecs);
#endif
}

/* Gives the processor up to another thread without sleeping: what a wait without
   an interval does between its checks, rather than holding a core in a busy loop */
void xyield(void)
{
#ifdef _WIN32
    SwitchToThread();
#else
    sched_yield();
#endif
}

static uint64_t XSync_MonoUsec(void)
{
#ifdef _WIN32
    LARGE_INTEGER nFreq, nCount;
    QueryPerformanceFrequency(&nFreq);
    QueryPerformanceCounter(&nCount);
    return (uint64_t)(nCount.QuadPart / nFreq.QuadPart) * 1000000 +
           (uint64_t)(nCount.QuadPart % nFreq.QuadPart) * 1000000 / (uint64_t)nFreq.QuadPart;
#else
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000 + (uint64_t)now.tv_nsec / 1000;
#endif
}

#ifndef _WIN32
/* The pthread functions return their error instead of setting errno */
static int XSync_InitMutex(pthread_mutex_t *pMutex, xbool_t bRecursive)
{
    if (!bRecursive) return pthread_mutex_init(pMutex, NULL);

    pthread_mutexattr_t mutexAttr;
    int nStatus = pthread_mutexattr_init(&mutexAttr);
    if (nStatus) return nStatus;

    nStatus = pthread_mutexattr_settype(&mutexAttr, PTHREAD_MUTEX_RECURSIVE);
    if (!nStatus) nStatus = pthread_mutex_init(pMutex, &mutexAttr);

    pthread_mutexattr_destroy(&mutexAttr);
    return nStatus;
}
#endif

void XSync_InitRecursive(xsync_mutex_t *pSync)
{
    XCHECK_VOID_NL((pSync != NULL));
#ifndef _WIN32
    int nStatus = XSync_InitMutex(&pSync->mutex, XTRUE);
    if (nStatus)
    {
        fprintf(stderr, "<%s:%d> %s: Can not initialize recursive mutex: %d\n",
            __FILE__, __LINE__, __FUNCTION__, nStatus);

        exit(EXIT_FAILURE);
    }
#else
    InitializeCriticalSection(&pSync->mutex);
#endif

    pSync->bEnabled = XTRUE;
}

void XSync_Init(xsync_mutex_t *pSync)
{
    XCHECK_VOID_NL((pSync != NULL));
#ifndef _WIN32
    int nStatus = XSync_InitMutex(&pSync->mutex, XFALSE);
    if (nStatus)
    {
        fprintf(stderr, "<%s:%d> %s: Can not initialize mutex: %d\n",
            __FILE__, __LINE__, __FUNCTION__, nStatus);

        exit(EXIT_FAILURE);
    }
#else
    InitializeCriticalSection(&pSync->mutex);
#endif

    pSync->bEnabled = XTRUE;
}

XSTATUS XSync_InitAdv(xsync_mutex_t *pSync, xbool_t bRecursive)
{
    XCHECK_NL((pSync != NULL), XSTDERR);
#ifndef _WIN32
    int nStatus = XSync_InitMutex(&pSync->mutex, bRecursive);
    if (nStatus)
    {
        fprintf(stderr, "<%s:%d> %s: Can not initialize %smutex: %d\n",
            __FILE__, __LINE__, __FUNCTION__, bRecursive ? "recursive " : "", nStatus);

        return XSTDERR;
    }
#else
    InitializeCriticalSection(&pSync->mutex);
#endif

    pSync->bEnabled = XTRUE;
    return XSTDOK;
}

void XSync_Destroy(xsync_mutex_t *pSync)
{
    XCHECK_VOID_NL((pSync != NULL));
    if (pSync->bEnabled)
    {
#ifndef _WIN32
        int nStatus = pthread_mutex_destroy(&pSync->mutex);
        if (nStatus)
        {
            fprintf(stderr, "<%s:%d> %s: Can not deinitialize mutex: %d\n",
                __FILE__, __LINE__, __FUNCTION__, nStatus);

            exit(EXIT_FAILURE);
        }
#else
        DeleteCriticalSection(&pSync->mutex);
#endif
    }

    pSync->bEnabled = XFALSE;
}

void XSync_Lock(xsync_mutex_t *pSync)
{
    XCHECK_VOID_NL((pSync != NULL));
    if (pSync->bEnabled)
    {
#ifndef _WIN32
        int nStatus = pthread_mutex_lock(&pSync->mutex);
        if (nStatus)
        {
            fprintf(stderr, "<%s:%d> %s: Can not lock mutex: %d\n",
                __FILE__, __LINE__, __FUNCTION__, nStatus);

            exit(EXIT_FAILURE);
        }
#else
        EnterCriticalSection(&pSync->mutex);
#endif
    }
}

void XSync_Unlock(xsync_mutex_t *pSync)
{
    XCHECK_VOID_NL((pSync != NULL));
    if (pSync->bEnabled)
    {
#ifndef _WIN32
        int nStatus = pthread_mutex_unlock(&pSync->mutex);
        if (nStatus)
        {
            fprintf(stderr, "<%s:%d> %s: Can not unlock mutex: %d\n",
                __FILE__, __LINE__, __FUNCTION__, nStatus);

            exit(EXIT_FAILURE);
        }
#else
        LeaveCriticalSection(&pSync->mutex);
#endif
    }
}

void XRWSync_Init(xsync_rw_t *pSync)
{
    XCHECK_VOID_NL((pSync != NULL));
#ifndef _WIN32
    int nStatus = pthread_rwlock_init(&pSync->rwLock, NULL);
    if (nStatus)
    {
        fprintf(stderr, "<%s:%d> %s: Can not init rwlock: %d\n",
            __FILE__, __LINE__, __FUNCTION__, nStatus);

        exit(EXIT_FAILURE);
    }
#else
    InitializeSRWLock(&pSync->rwLock);
    pSync->bExclusive = XFALSE;
#endif

    pSync->bEnabled = XTRUE;
}

void XRWSync_ReadLock(xsync_rw_t *pSync)
{
    XCHECK_VOID_NL((pSync != NULL));
    if (pSync->bEnabled)
    {
#ifndef _WIN32
        int nStatus = pthread_rwlock_rdlock(&pSync->rwLock);
        if (nStatus)
        {
            fprintf(stderr, "<%s:%d> %s: Can not read lock rwlock: %d\n",
                __FILE__, __LINE__, __FUNCTION__, nStatus);

            exit(EXIT_FAILURE);
        }
#else
        AcquireSRWLockShared(&pSync->rwLock);
#endif
    }
}

void XRWSync_WriteLock(xsync_rw_t *pSync)
{
    XCHECK_VOID_NL((pSync != NULL));
    if (pSync->bEnabled)
    {
#ifndef _WIN32
        int nStatus = pthread_rwlock_wrlock(&pSync->rwLock);
        if (nStatus)
        {
            fprintf(stderr, "<%s:%d> %s: Can not write lock rwlock: %d\n",
                __FILE__, __LINE__, __FUNCTION__, nStatus);

            exit(EXIT_FAILURE);
        }
#else
        AcquireSRWLockExclusive(&pSync->rwLock);
        pSync->bExclusive = XTRUE;
#endif
    }
}

#ifndef _WIN32
static void XRWSync_Release(xsync_rw_t *pSync, const char *pFunction)
{
    int nStatus = pthread_rwlock_unlock(&pSync->rwLock);
    if (nStatus)
    {
        fprintf(stderr, "<%s:%d> %s: Can not unlock rwlock: %d\n",
            __FILE__, __LINE__, pFunction, nStatus);

        exit(EXIT_FAILURE);
    }
}
#endif

/* Releases whichever lock the caller holds. An SRW lock has to be told which one that is, so on Windows the
   writer that holds it marks it exclusive. The mark is cleared while the lock is still held: cleared after the
   release, it could be cleared over the mark of the next writer, or still be there for the next reader, and
   either then released the lock as what it was not. Only the holder of the exclusive lock ever writes the mark,
   and nobody reads it then but the holder, so it is never written while it is read. */
void XRWSync_Unlock(xsync_rw_t *pSync)
{
    XCHECK_VOID_NL((pSync != NULL));
    if (pSync->bEnabled)
    {
#ifndef _WIN32
        XRWSync_Release(pSync, __FUNCTION__);
#else
        if (pSync->bExclusive)
        {
            pSync->bExclusive = XFALSE;
            ReleaseSRWLockExclusive(&pSync->rwLock);
        }
        else ReleaseSRWLockShared(&pSync->rwLock);
#endif
    }
}

/* Releases a lock taken with XRWSync_ReadLock(), saying so rather than leaving it to the mark */
void XRWSync_ReadUnlock(xsync_rw_t *pSync)
{
    XCHECK_VOID_NL((pSync != NULL));
    if (pSync->bEnabled)
    {
#ifndef _WIN32
        XRWSync_Release(pSync, __FUNCTION__);
#else
        ReleaseSRWLockShared(&pSync->rwLock);
#endif
    }
}

/* Releases a lock taken with XRWSync_WriteLock() */
void XRWSync_WriteUnlock(xsync_rw_t *pSync)
{
    XCHECK_VOID_NL((pSync != NULL));
    if (pSync->bEnabled)
    {
#ifndef _WIN32
        XRWSync_Release(pSync, __FUNCTION__);
#else
        pSync->bExclusive = XFALSE;
        ReleaseSRWLockExclusive(&pSync->rwLock);
#endif
    }
}

void XRWSync_Destroy(xsync_rw_t *pSync)
{
    XCHECK_VOID_NL((pSync != NULL));
    if (pSync->bEnabled)
    {
#ifndef _WIN32
        int nStatus = pthread_rwlock_destroy(&pSync->rwLock);
        if (nStatus)
        {
            fprintf(stderr, "<%s:%d> %s: Can not destroy rwlock: %d\n",
                __FILE__, __LINE__, __FUNCTION__, nStatus);

            exit(EXIT_FAILURE);
        }
#else
        (void)pSync;
#endif
    }

    pSync->bEnabled = XFALSE;
}

/* Every barrier raised in the process takes a value of its own from one count, odd, and never the one value whose
   acknowledgement would be zero; a lowered barrier is zero. An acknowledgement is the value it saw, plus one so that
   none is ever zero, and holds only while that is still the barrier's value. So it answers the barrier it saw and
   no other, however soon that one is lowered and the next raised, which a pair of flags let a thread take for the
   one it had answered, or not see go down at all. Lowering needs no history: XSyncBar_Reset() sets a barrier up too. */
static xatomic_t g_nBarRaised = 0;

static xatomic_t XSyncBar_Raised(void)
{
    unsigned long nCount = (unsigned long)XSYNC_ATOMIC_ADD(&g_nBarRaised, 1);
    return (xatomic_t)(((nCount % 0x7FFFFFFFUL) << 1) | 1UL);
}

/* Only the controller raises and lowers the barrier. The old acknowledgement goes before the new value comes, with
   a full barrier between them: with an acquire barrier alone, the clear could be seen after the barrier was up,
   and wipe an acknowledgement that came quickly. */
void XSyncBar_Bar(xsync_bar_t *pBar)
{
    XCHECK_VOID_NL((pBar != NULL));
    xatomic_t nRaised = XSyncBar_Raised();
    XSYNC_ATOMIC_SET(&pBar->nAck, 0);
    XSYNC_ATOMIC_SET(&pBar->nBar, nRaised);
}

/* Acknowledges the barrier as it is now. The thread that acknowledges then waits for as long as
   XSyncBar_CheckAck() holds: it stops holding the moment the barrier is lowered or another one raised. */
void XSyncBar_Ack(xsync_bar_t *pBar)
{
    XCHECK_VOID_NL((pBar != NULL));
    unsigned long nSeen = (unsigned long)XSYNC_ATOMIC_GET(&pBar->nBar);
    XSYNC_ATOMIC_SET(&pBar->nAck, (xatomic_t)(nSeen + 1));
}

void XSyncBar_Reset(xsync_bar_t *pBar)
{
    XCHECK_VOID_NL((pBar != NULL));
    XSYNC_ATOMIC_SET(&pBar->nBar, 0);
    XSYNC_ATOMIC_SET(&pBar->nAck, 0);
}

uint8_t XSyncBar_CheckBar(xsync_bar_t *pBar)
{
    XCHECK_NL((pBar != NULL), XSTDNON);
    return ((unsigned long)XSYNC_ATOMIC_GET(&pBar->nBar) & 1) ? XSTDOK : XSTDNON;
}

uint8_t XSyncBar_CheckAck(xsync_bar_t *pBar)
{
    XCHECK_NL((pBar != NULL), XSTDNON);
    unsigned long nAck = (unsigned long)XSYNC_ATOMIC_GET(&pBar->nAck);
    unsigned long nBar = (unsigned long)XSYNC_ATOMIC_GET(&pBar->nBar);
    return (nAck != 0 && (xatomic_t)nAck == (xatomic_t)(nBar + 1)) ? XSTDOK : XSTDNON;
}

/* Waits for the acknowledgement for as long as it takes, sleeping nSleepUsec between checks, or yielding without
   one. Returns the time slept, which stops at UINT32_MAX rather than wrapping round after about 71 minutes. */
uint32_t XSyncBar_WaitAck(xsync_bar_t *pBar, uint32_t nSleepUsec)
{
    uint32_t nUsecs = 0;

    /* Nothing acknowledges a barrier there is not, the wait went on forever */
    XCHECK_NL((pBar != NULL), nUsecs);

    while (!XSyncBar_CheckAck(pBar))
    {
        if (!nSleepUsec)
        {
            xyield();
            continue;
        }

        xusleep(nSleepUsec);
        nUsecs = nSleepUsec > UINT32_MAX - nUsecs ? UINT32_MAX : nUsecs + nSleepUsec;
    }

    return nUsecs;
}

/* As XSyncBar_WaitAck(), giving up once nTimeoutUsec have passed by the monotonic clock. Returns XSTDOK when the
   acknowledgement came, XSTDNON when it did not in time. */
XSTATUS XSyncBar_WaitAckFor(xsync_bar_t *pBar, uint32_t nSleepUsec, uint32_t nTimeoutUsec)
{
    XCHECK_NL((pBar != NULL), XSTDNON);
    uint64_t nStart = XSync_MonoUsec();

    while (!XSyncBar_CheckAck(pBar))
    {
        if (XSync_MonoUsec() - nStart >= nTimeoutUsec)
            return XSyncBar_CheckAck(pBar) ? XSTDOK : XSTDNON;
        else if (nSleepUsec) xusleep(nSleepUsec);
        else xyield();
    }

    return XSTDOK;
}
