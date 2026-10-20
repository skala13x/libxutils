/*!
 *  @file libxutils/examples/thread.c
 *
 *  This source is part of the "libxutils" project
 *  2015-2026  Sandro Kalatozishvili (s.kalatoz@gmail.com)
 *
 * @brief Example of working with threads and tasks. Worker threads sum
 * parts of a range in parallel: each one writes its own result, adds it
 * to a total guarded by a mutex and reports progress with an atomic
 * counter. A task calls a function periodically in a thread of its own,
 * it is paused, resumed and stopped from outside or stops by itself.
 */

#include "xstd.h"
#include "thread.h"
#include "sync.h"
#include "log.h"

#define XTHREAD_WORKERS     4
#define XTHREAD_RANGE_END   2000000ULL

typedef struct {
    xsync_mutex_t lock;
    uint64_t nTotal;
    int nFinished;
    xatomic_t nProgress;
} xshared_t;

typedef struct {
    xshared_t *pShared;
    uint64_t nFrom;
    uint64_t nTo;
    uint64_t nSum;
} xworker_t;

static void* sum_range(void *pArg)
{
    xworker_t *pWorker = (xworker_t*)pArg;
    xshared_t *pShared = pWorker->pShared;
    uint64_t i, nSum = 0;

    for (i = pWorker->nFrom; i <= pWorker->nTo; i++)
    {
        nSum += i;

        /* Atomic operations need no lock, others can read the progress at any time */
        if (!(i % 100000)) XSYNC_ATOMIC_ADD(&pShared->nProgress, 1);
    }

    /* The result goes to the caller through the argument, it works on every platform */
    pWorker->nSum = nSum;

    /* A mutex for more than one value changed together */
    XSync_Lock(&pShared->lock);
    pShared->nTotal += nSum;
    pShared->nFinished++;
    XSync_Unlock(&pShared->lock);

    return NULL;
}

/* Task callbacks run again and again, a negative return value ends the task */
static int heartbeat(void *pCtx)
{
    XSYNC_ATOMIC_ADD((xatomic_t*)pCtx, 1);
    return XSTDOK;
}

static int countdown(void *pCtx)
{
    xatomic_t nLeft = XSYNC_ATOMIC_SUB((xatomic_t*)pCtx, 1);
    return nLeft > 0 ? XSTDOK : XSTDERR;
}

int main()
{
    xlog_defaults();
    xlog_enable(XLOG_INFO);

//////////////////////////////////////////////////////////////////////////////////////////////
// Split a sum between worker threads and wait for all of them
//////////////////////////////////////////////////////////////////////////////////////////////

    xshared_t shared;
    XSync_Init(&shared.lock);
    shared.nTotal = 0;
    shared.nFinished = 0;
    shared.nProgress = 0;

    xthread_t threads[XTHREAD_WORKERS];
    xworker_t workers[XTHREAD_WORKERS];
    uint64_t nPart = XTHREAD_RANGE_END / XTHREAD_WORKERS;
    int i, nStarted = 0;

    for (i = 0; i < XTHREAD_WORKERS; i++)
    {
        workers[i].pShared = &shared;
        workers[i].nFrom = i * nPart + 1;
        workers[i].nTo = i + 1 < XTHREAD_WORKERS ? (i + 1) * nPart : XTHREAD_RANGE_END;
        workers[i].nSum = 0;

        /* Not detached, the thread is joined below */
        if (XThread_Create(&threads[i], sum_range, &workers[i], XFALSE) != XSTDOK)
        {
            xloge("Failed to start worker %d: %s", i, XSTRERR);
            break;
        }

        nStarted++;
    }

    for (i = 0; i < nStarted; i++)
    {
        XThread_Join(&threads[i]);
        xlog("Worker %d summed %llu to %llu: %llu", i, (unsigned long long)workers[i].nFrom,
            (unsigned long long)workers[i].nTo, (unsigned long long)workers[i].nSum);
    }

    XSync_Destroy(&shared.lock);
    uint64_t nExpected = XTHREAD_RANGE_END * (XTHREAD_RANGE_END + 1) / 2;

    xlog("%d workers finished, progress steps %ld", shared.nFinished, (long)XSYNC_ATOMIC_GET(&shared.nProgress));
    xlog("Total %llu, expected %llu\n", (unsigned long long)shared.nTotal, (unsigned long long)nExpected);

    if (nStarted != XTHREAD_WORKERS || shared.nTotal != nExpected)
    {
        xloge("The sum is not complete");
        return XSTDERR;
    }

//////////////////////////////////////////////////////////////////////////////////////////////
// Run a function every millisecond, pause, resume and stop it
//////////////////////////////////////////////////////////////////////////////////////////////

    xatomic_t nBeats = 0;
    xtask_t task;

    if (XTask_Start(&task, heartbeat, &nBeats, 1000) != XSTDOK)
    {
        xloge("Failed to start the heartbeat task: %s", XSTRERR);
        return XSTDERR;
    }

    xusleep(20000);

    /* Hold() returns when the task is paused, it does not beat while held */
    XTask_Hold(&task, 1000);
    xatomic_t nHeld = XSYNC_ATOMIC_GET(&nBeats);

    xusleep(20000);
    xlog("Beats when held: %ld, after 20 ms: %ld", (long)nHeld, (long)XSYNC_ATOMIC_GET(&nBeats));

    XTask_Release(&task, 1000);
    xusleep(20000);

    /* Stop() returns when the task has ended */
    XTask_Stop(&task, 1000);
    xlog("Beats after release and stop: %ld", (long)XSYNC_ATOMIC_GET(&nBeats));

//////////////////////////////////////////////////////////////////////////////////////////////
// A task that ends by itself
//////////////////////////////////////////////////////////////////////////////////////////////

    xatomic_t nLeft = 5;
    if (XTask_Start(&task, countdown, &nLeft, 1000) != XSTDOK)
    {
        xloge("Failed to start the countdown task: %s", XSTRERR);
        return XSTDERR;
    }

    /* Stop() would end it too, here the callback does: the status tells when */
    while (XSYNC_ATOMIC_GET(&task.nStatus) != XTASK_STAT_STOPPED) xusleep(1000);
    xlog("Countdown ended at %ld", (long)XSYNC_ATOMIC_GET(&nLeft));

    return 0;
}
