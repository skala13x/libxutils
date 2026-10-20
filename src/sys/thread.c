/*!
 *  @file libxutils/src/sys/thread.c
 *
 *  This source is part of the "libxutils" project
 *  2015-2020  Sandro Kalatozishvili (s.kalatoz@gmail.com)
 *
 * @brief Implementation of the POSIX thread functionality
 */

#include "xstd.h"
#include "sync.h"
#include "thread.h"

#ifdef _WIN32
/* The trampoline owns a heap copy of the callback and its argument:
   reading them from the caller's xthread_t inside the new thread would
   race with the caller freeing or reusing that struct (detached use). */
typedef struct XThreadStartup {
    xthread_cb_t functionCb;
    void *pArgument;
} xthread_startup_t;

static DWORD WINAPI XThread_WinThread(void* pArg)
{
    xthread_startup_t *pStartup = (xthread_startup_t*)pArg;
    xthread_cb_t functionCb = pStartup->functionCb;
    void *pArgument = pStartup->pArgument;

    free(pStartup);
    functionCb(pArgument);
    return 0;
}
#endif

void XThread_Init(xthread_t *pThread)
{
    XCHECK_VOID_NL((pThread != NULL));
    pThread->nStackSize = XTHREAD_STACK_SIZE;
    pThread->functionCb = NULL;
    pThread->pArgument = NULL;
    pThread->nDetached = 0;
    pThread->nStatus = 0;
}

int XThread_Run(xthread_t *pThread)
{
    XCHECK_NL((pThread != NULL), XSTDERR);
    if (pThread->functionCb == NULL) return XSTDNON;
    pThread->nStatus = XTHREAD_FAIL;

#ifdef _WIN32
    xthread_startup_t *pStartup = (xthread_startup_t*)malloc(sizeof(xthread_startup_t));
    if (pStartup == NULL) return XSTDERR;

    pStartup->functionCb = pThread->functionCb;
    pStartup->pArgument = pThread->pArgument;

    pThread->threadId = CreateThread(NULL, pThread->nStackSize, XThread_WinThread, pStartup, 0, NULL);
    if (pThread->threadId == NULL)
    {
        free(pStartup);
        return XSTDERR;
    }

    if (pThread->nDetached) CloseHandle(pThread->threadId);
#else
    pthread_attr_t attr;
    int nAttrStatus = pthread_attr_init(&attr);
    if (nAttrStatus != 0)
    {
        errno = nAttrStatus;
        return XSTDERR; /* attr is uninitialized and must not be destroyed. */
    }

    nAttrStatus = pthread_attr_setstacksize(&attr, pThread->nStackSize);
    if (nAttrStatus != 0)
    {
        fprintf(stderr, "<%s:%d> %s: Can not initialize pthread attribute: %d\n",
            __FILE__, __LINE__, __FUNCTION__, nAttrStatus);

        pthread_attr_destroy(&attr);
        errno = nAttrStatus;
        return XSTDERR;
    }

    if (pThread->nDetached)
    {
        nAttrStatus = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (nAttrStatus != 0)
        {
            fprintf(stderr, "<%s:%d> %s Can not set detache state to the pthread attribute: %d\n",
                __FILE__, __LINE__, __FUNCTION__, nAttrStatus);

            pthread_attr_destroy(&attr);
            errno = nAttrStatus;
            return XSTDERR;
        }
    }

    int nStatus = pthread_create(&pThread->threadId, &attr, pThread->functionCb, pThread->pArgument);
    if (nStatus != 0)
    {
        fprintf(stderr, "<%s:%d> %s Can not create pthread: %d\n",
            __FILE__, __LINE__, __FUNCTION__, nStatus);

        pthread_attr_destroy(&attr);
        errno = nStatus;
        return XSTDERR;
    }

    pthread_attr_destroy(&attr);
#endif

    pThread->nStatus = XTHREAD_SUCCESS;
    return XSTDOK;
}

int XThread_Create(xthread_t *pThread, xthread_cb_t func, void *pArg, uint8_t nDtch)
{
    XCHECK_NL((pThread != NULL), XSTDERR);
    XThread_Init(pThread);
    pThread->functionCb = func;
    pThread->pArgument = pArg;
    pThread->nDetached = nDtch;
    return XThread_Run(pThread);
}

void* XThread_Join(xthread_t *pThread)
{
    XCHECK_NL((pThread != NULL), NULL);

    /* A thread that never started has nothing to join, its id is whatever was there */
    if (pThread->nDetached || pThread->nStatus != XTHREAD_SUCCESS) return NULL;

    void* pReturnValue = NULL;
    int nStatus = 0;

#ifdef _WIN32
    WaitForSingleObject(pThread->threadId, INFINITE);
    CloseHandle(pThread->threadId);
    pThread->nDetached = 1;
    nStatus = 1;
#else
    pthread_t nTid = pThread->threadId;
    nStatus = pthread_join(nTid, &pReturnValue);
#endif

    return nStatus ? NULL : pReturnValue;
}

void *XTask_WorkerThread(void *pContext)
{
    XCHECK_NL((pContext != NULL), NULL);
    xtask_t *pTask = (xtask_t*)pContext;
    XSYNC_ATOMIC_SET(&pTask->nStatus, XTASK_STAT_ACTIVE);

    xbool_t bIsPaused = XFALSE;
    xatomic_t nAction = 0;

    while((nAction = XSYNC_ATOMIC_GET(&pTask->nAction)) != XTASK_CTRL_STOP)
    {
        xatomic_t nInterval = XSYNC_ATOMIC_GET(&pTask->nIntervalU);

        if (nAction == XTASK_CTRL_PAUSE)
        {
            if (!bIsPaused)
            {
                XSYNC_ATOMIC_SET(&pTask->nStatus, XTASK_STAT_PAUSED);
                nInterval = nInterval ? nInterval : XTASK_SLEEP_USEC;
                bIsPaused = XTRUE;
            }

            if (nInterval) xusleep((uint32_t)nInterval);
            continue;
        }

        if (bIsPaused)
        {
            XSYNC_ATOMIC_SET(&pTask->nStatus, XTASK_STAT_ACTIVE);
            bIsPaused = XFALSE;
        }

        if (pTask->callback(pTask->pContext) < 0) break;
        if (nInterval) xusleep((uint32_t)nInterval);
    }

    XSYNC_ATOMIC_SET(&pTask->nStatus, XTASK_STAT_STOPPED);
    return NULL;
}

static int XTask_Launch(xtask_t *pTask, xtask_cb_t callback, void *pContext, uint32_t nIntervalU, uint8_t nDetached)
{
    XCHECK_NL((pTask != NULL), XSTDERR);
    if (callback == NULL) return 0;
    pTask->callback = callback;
    pTask->pContext = pContext;

    XSYNC_ATOMIC_SET(&pTask->nAction, XTASK_CTRL_RELEASE);
    XSYNC_ATOMIC_SET(&pTask->nIntervalU, nIntervalU);
    XSYNC_ATOMIC_SET(&pTask->nStatus, XTASK_STAT_CREATED);

    int nStatus = XThread_Create(&pTask->taskTd, XTask_WorkerThread, pTask, nDetached);
    if (nStatus != XSTDOK) XSYNC_ATOMIC_SET(&pTask->nStatus, XTASK_STAT_FAIL);

    return nStatus;
}

/* The worker runs detached. Once the task is XTASK_STAT_STOPPED it touches neither the task nor its context again,
   so either can be freed, but its thread may still be on its way out: code it returns through must not be unloaded
   yet. A task started with XTask_StartJoinable() is joined by the XTask_Stop() that waits for it instead, which
   returns only once the thread is gone; until then the task holds its thread, and has to be stopped that way. */
int XTask_Start(xtask_t *pTask, xtask_cb_t callback, void *pContext, uint32_t nIntervalU)
{
    return XTask_Launch(pTask, callback, pContext, nIntervalU, XTRUE);
}

int XTask_StartJoinable(xtask_t *pTask, xtask_cb_t callback, void *pContext, uint32_t nIntervalU)
{
    return XTask_Launch(pTask, callback, pContext, nIntervalU, XFALSE);
}

/* Whether a task with this status can still get to nEvent. A stopped task is never active or paused again, and
   one that never started, or failed to, never gets anywhere: waiting for either used to wait forever. */
static xbool_t XTask_CanReach(int nStatus, int nEvent)
{
    if (nStatus == XTASK_STAT_STOPPED) return nEvent == XTASK_STAT_STOPPED ? XTRUE : XFALSE;
    return nStatus == XTASK_STAT_CREATED || nStatus == XTASK_STAT_ACTIVE || nStatus == XTASK_STAT_PAUSED;
}

/* Waits for the task to get to nEvent, sleeping nIntervalU between checks or yielding without one, or not at all
   for a negative one. Returns the time slept, which stops at UINT32_MAX rather than wrapping round. */
uint32_t XTask_Wait(xtask_t *pTask, int nEvent, int nIntervalU)
{
    uint32_t nSlept = 0;
    int nStatus;
    XCHECK_NL((pTask != NULL), nSlept);

    while ((nStatus = (int)XSYNC_ATOMIC_GET(&pTask->nStatus)) != nEvent)
    {
        if (nIntervalU < 0 || !XTask_CanReach(nStatus, nEvent)) break;
        else if (!nIntervalU)
        {
            xyield();
            continue;
        }

        xusleep((uint32_t)nIntervalU);
        nSlept = (uint32_t)nIntervalU > UINT32_MAX - nSlept ? UINT32_MAX : nSlept + (uint32_t)nIntervalU;
    }

    return nSlept;
}

uint32_t XTask_Hold(xtask_t *pTask, int nIntervalU)
{
    XCHECK_NL((pTask != NULL), 0);
    XSYNC_ATOMIC_SET(&pTask->nAction, XTASK_CTRL_PAUSE);
    return XTask_Wait(pTask, XTASK_STAT_PAUSED, nIntervalU);
}

uint32_t XTask_Release(xtask_t *pTask, int nIntervalU)
{
    XCHECK_NL((pTask != NULL), 0);
    XSYNC_ATOMIC_SET(&pTask->nAction, XTASK_CTRL_RELEASE);
    return XTask_Wait(pTask, XTASK_STAT_ACTIVE, nIntervalU);
}

uint32_t XTask_Stop(xtask_t *pTask, int nIntervalU)
{
    XCHECK_NL((pTask != NULL), 0);
    XSYNC_ATOMIC_SET(&pTask->nAction, XTASK_CTRL_STOP);
    uint32_t nSlept = XTask_Wait(pTask, XTASK_STAT_STOPPED, nIntervalU);

    /* A joinable worker that has stopped is joined once, which marks its thread as no longer to be joined */
    if (!pTask->taskTd.nDetached && XSYNC_ATOMIC_GET(&pTask->nStatus) == XTASK_STAT_STOPPED)
    {
        XThread_Join(&pTask->taskTd);
        pTask->taskTd.nDetached = XTRUE;
    }

    return nSlept;
}
