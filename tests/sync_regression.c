/* libxutils: mutexes, read/write locks, barriers and background tasks.
 *
 * The locks are checked by the invariant they exist to protect: a shared
 * counter incremented by contending threads has to end at exactly the
 * number of increments, and a reader must never observe a half written
 * value. Disabled locks are checked too, since the library lets a caller
 * turn synchronisation off and every entry point has to tolerate that.
 */

#include "test.h"
#include "sync.h"
#include "thread.h"
#include "xtime.h"
#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define SYNC_TSAN 1
#endif
#endif
#if defined(__SANITIZE_THREAD__) && !defined(SYNC_TSAN)
#define SYNC_TSAN 1
#endif

#define SYNC_THREADS    4
#define SYNC_ROUNDS     4000

typedef struct {
    xsync_mutex_t lock;
    long nCounter;
} sync_mutex_test_t;

static void *sync_mutex_worker(void *pContext)
{
    sync_mutex_test_t *pTest = (sync_mutex_test_t*)pContext;
    for (int i = 0; i < SYNC_ROUNDS; i++)
    {
        XSync_Lock(&pTest->lock);
        pTest->nCounter++;
        XSync_Unlock(&pTest->lock);
    }
    return NULL;
}

static int XTest_mutex(void)
{
    /* A contended counter ends at exactly the number of increments. */
    sync_mutex_test_t test;
    test.nCounter = 0;
    XSync_Init(&test.lock);
    CHECK(test.lock.bEnabled == XTRUE, "An initialized mutex is enabled");

    xthread_t threads[SYNC_THREADS];
    for (int i = 0; i < SYNC_THREADS; i++)
        CHECK(XThread_Create(&threads[i], sync_mutex_worker, &test, XFALSE) == XSTDOK, "Start a worker");
    for (int i = 0; i < SYNC_THREADS; i++) XThread_Join(&threads[i]);

    CHECK(test.nCounter == (long)SYNC_THREADS * SYNC_ROUNDS, "Every increment is accounted for");
    XSync_Destroy(&test.lock);
    CHECK(test.lock.bEnabled == XFALSE, "A destroyed mutex is disabled");

    /* Locking a destroyed mutex is a no-op rather than undefined behaviour. */
    XSync_Lock(&test.lock);
    XSync_Unlock(&test.lock);
    XSync_Destroy(&test.lock);

    /* Destroying twice is safe because the disabled flag gates the work. */
    XSync_Destroy(&test.lock);
    CHECK(test.lock.bEnabled == XFALSE, "A repeatedly destroyed mutex stays disabled");
    return 0;
}

static int XTest_recursive(void)
{
    /* A recursive mutex can be taken by the same thread more than once;
     * a plain one deadlocks instead, which is why the two are separate. */
    xsync_mutex_t lock;
    XSync_InitRecursive(&lock);
    CHECK(lock.bEnabled == XTRUE, "A recursive mutex is enabled");

    XSync_Lock(&lock);
    XSync_Lock(&lock);
    XSync_Lock(&lock);
    XSync_Unlock(&lock);
    XSync_Unlock(&lock);
    XSync_Unlock(&lock);
    CHECK(1, "A recursive mutex can be re-entered by its own thread");
    XSync_Destroy(&lock);

    /* The advanced initializer selects between the two behaviours. */
    CHECK(XSync_InitAdv(&lock, XTRUE) == XSTDOK, "The advanced initializer creates a recursive mutex");
    XSync_Lock(&lock);
    XSync_Lock(&lock);
    XSync_Unlock(&lock);
    XSync_Unlock(&lock);
    XSync_Destroy(&lock);

    CHECK(XSync_InitAdv(&lock, XFALSE) == XSTDOK, "The advanced initializer creates a plain mutex");
    XSync_Lock(&lock);
    XSync_Unlock(&lock);
    XSync_Destroy(&lock);

    return 0;
}

typedef struct {
    xsync_rw_t lock;
    xatomic_t nStop;
    long nValue;
    long nMirror;
    xatomic_t nTornReads;
    xatomic_t nReads;
    xatomic_t nReadersUp;
} sync_rw_test_t;

/* Writers keep two fields in step; a torn read would see them differ. */
static void *sync_rw_writer(void *pContext)
{
    sync_rw_test_t *pTest = (sync_rw_test_t*)pContext;
    for (int i = 0; i < SYNC_ROUNDS; i++)
    {
        XRWSync_WriteLock(&pTest->lock);
        pTest->nValue++;
        pTest->nMirror = pTest->nValue;
        XRWSync_Unlock(&pTest->lock);
    }
    return NULL;
}

static void *sync_rw_reader(void *pContext)
{
    sync_rw_test_t *pTest = (sync_rw_test_t*)pContext;
    /* Take the lock once and announce it, so the writers only start when
     * the readers are demonstrably running: whether a given thread gets
     * scheduled in some window is the scheduler's business, not the lock's,
     * and a test that depends on it fails on a busy machine for no reason. */
    XRWSync_ReadLock(&pTest->lock);
    if (pTest->nValue != pTest->nMirror) XSYNC_ATOMIC_ADD(&pTest->nTornReads, 1);
    XRWSync_Unlock(&pTest->lock);
    XSYNC_ATOMIC_ADD(&pTest->nReads, 1);
    XSYNC_ATOMIC_ADD(&pTest->nReadersUp, 1);

    /* Then keep reading until the writers are done. The short sleep keeps
     * them from starving the writers when this runs under a memory checker. */
    while (!XSYNC_ATOMIC_GET(&pTest->nStop))
    {
        XRWSync_ReadLock(&pTest->lock);
        if (pTest->nValue != pTest->nMirror) XSYNC_ATOMIC_ADD(&pTest->nTornReads, 1);
        XRWSync_Unlock(&pTest->lock);
        XSYNC_ATOMIC_ADD(&pTest->nReads, 1);
        xusleep(100);
    }
    return NULL;
}

static int XTest_rwlock(void)
{
    sync_rw_test_t test;
    memset(&test, 0, sizeof(test));
    XRWSync_Init(&test.lock);
    CHECK(test.lock.bEnabled == XTRUE, "An initialized read/write lock is enabled");

    xthread_t writers[2], readers[2];
    for (int i = 0; i < 2; i++)
        CHECK(XThread_Create(&readers[i], sync_rw_reader, &test, XFALSE) == XSTDOK, "Start a reader");

    /* Wait for both readers to have taken the lock before the writers
     * start, so the contention the torn-read check depends on is real
     * rather than a matter of timing. */
    for (int i = 0; i < 20000 && XSYNC_ATOMIC_GET(&test.nReadersUp) < 2; i++) xusleep(500);
    CHECK(XSYNC_ATOMIC_GET(&test.nReadersUp) == 2, "Both readers took the lock before the writes began");

    for (int i = 0; i < 2; i++)
        CHECK(XThread_Create(&writers[i], sync_rw_writer, &test, XFALSE) == XSTDOK, "Start a writer");

    for (int i = 0; i < 2; i++) XThread_Join(&writers[i]);
    XSYNC_ATOMIC_SET(&test.nStop, 1);
    for (int i = 0; i < 2; i++) XThread_Join(&readers[i]);

    CHECK(test.nValue == 2 * SYNC_ROUNDS, "Every write is accounted for");
    CHECK(test.nMirror == test.nValue, "The two fields end in step");
    CHECK(XSYNC_ATOMIC_GET(&test.nReads) >= 2, "Every reader took the lock at least once");
    CHECK(XSYNC_ATOMIC_GET(&test.nTornReads) == 0, "No reader observed a half written update");

    /* Several readers can hold the lock at once. */
    XRWSync_ReadLock(&test.lock);
    XRWSync_Unlock(&test.lock);
    XRWSync_WriteLock(&test.lock);
    XRWSync_Unlock(&test.lock);

    XRWSync_Destroy(&test.lock);
    CHECK(test.lock.bEnabled == XFALSE, "A destroyed read/write lock is disabled");

    /* A destroyed lock is tolerated: the disabled flag gates every call. */
    XRWSync_ReadLock(&test.lock);
    XRWSync_WriteLock(&test.lock);
    XRWSync_Unlock(&test.lock);
    XRWSync_Destroy(&test.lock);
    CHECK(test.lock.bEnabled == XFALSE, "A repeatedly destroyed read/write lock stays disabled");
    return 0;
}

typedef struct {
    xsync_bar_t bar;
    xatomic_t nSeen;
} sync_bar_test_t;

static void *sync_bar_worker(void *pContext)
{
    sync_bar_test_t *pTest = (sync_bar_test_t*)pContext;

    /* Wait for the barrier to be raised, then acknowledge it. */
    while (!XSyncBar_CheckBar(&pTest->bar)) xusleep(1000);
    XSYNC_ATOMIC_SET(&pTest->nSeen, 1);
    XSyncBar_Ack(&pTest->bar);
    return NULL;
}

static int XTest_barrier(void)
{
    sync_bar_test_t test;
    memset(&test, 0, sizeof(test));
    XSyncBar_Reset(&test.bar);

    CHECK(XSyncBar_CheckBar(&test.bar) == XSTDNON, "A reset barrier is lowered");
    CHECK(XSyncBar_CheckAck(&test.bar) == XSTDNON, "A reset barrier is unacknowledged");

    /* Raising the barrier clears any previous acknowledgement. */
    XSyncBar_Ack(&test.bar);
    CHECK(XSyncBar_CheckAck(&test.bar) == XSTDOK, "An acknowledgement is visible");
    XSyncBar_Bar(&test.bar);
    CHECK(XSyncBar_CheckBar(&test.bar) == XSTDOK, "The raised barrier is visible");
    CHECK(XSyncBar_CheckAck(&test.bar) == XSTDNON, "Raising the barrier clears the acknowledgement");

    /* A waiter blocks until the other side acknowledges. */
    XSyncBar_Reset(&test.bar);
    xthread_t worker;
    CHECK(XThread_Create(&worker, sync_bar_worker, &test, XFALSE) == XSTDOK, "Start the barrier worker");

    XSyncBar_Bar(&test.bar);
    XSyncBar_WaitAck(&test.bar, 1000);
    CHECK(XSYNC_ATOMIC_GET(&test.nSeen) == 1, "The worker saw the barrier before acknowledging");
    CHECK(XSyncBar_CheckAck(&test.bar) == XSTDOK, "The wait returned only once acknowledged");
    XThread_Join(&worker);

    /* Waiting on an already acknowledged barrier returns at once. */
    CHECK(XSyncBar_WaitAck(&test.bar, 1000) == 0, "An acknowledged barrier does not wait");

    XSyncBar_Reset(&test.bar);
    CHECK(XSyncBar_CheckBar(&test.bar) == XSTDNON && XSyncBar_CheckAck(&test.bar) == XSTDNON,
        "A reset clears both halves of the barrier");
    return 0;
}

typedef struct {
    xatomic_t nRuns;
    xatomic_t nStopAfter;
} sync_task_test_t;

static int sync_task_callback(void *pContext)
{
    sync_task_test_t *pTest = (sync_task_test_t*)pContext;
    xatomic_t nRuns = XSYNC_ATOMIC_ADD(&pTest->nRuns, 1);

    xatomic_t nStopAfter = XSYNC_ATOMIC_GET(&pTest->nStopAfter);
    if (nStopAfter > 0 && nRuns >= nStopAfter) return XSTDERR;
    return XSTDOK;
}

static int XTest_task(void)
{
    /* A task runs its callback repeatedly until it is stopped. */
    sync_task_test_t test;
    memset(&test, 0, sizeof(test));

    xtask_t task;
    memset(&task, 0, sizeof(task));
    CHECK(XTask_Start(&task, sync_task_callback, &test, 1000) == XSTDOK, "The task starts");

    for (int i = 0; i < 500 && XSYNC_ATOMIC_GET(&test.nRuns) < 5; i++) xusleep(2000);
    CHECK(XSYNC_ATOMIC_GET(&test.nRuns) >= 5, "The task callback runs repeatedly");

    /* Holding the task pauses it; releasing resumes it. */
    XTask_Hold(&task, 1000);
    CHECK(XSYNC_ATOMIC_GET(&task.nStatus) == XTASK_STAT_PAUSED, "A held task reports itself paused");

    xatomic_t nAtPause = XSYNC_ATOMIC_GET(&test.nRuns);
    xusleep(30000);
    CHECK(XSYNC_ATOMIC_GET(&test.nRuns) == nAtPause, "A paused task stops running its callback");

    XTask_Release(&task, 1000);
    CHECK(XSYNC_ATOMIC_GET(&task.nStatus) == XTASK_STAT_ACTIVE, "A released task reports itself active");

    for (int i = 0; i < 500 && XSYNC_ATOMIC_GET(&test.nRuns) <= nAtPause; i++) xusleep(2000);
    CHECK(XSYNC_ATOMIC_GET(&test.nRuns) > nAtPause, "A released task runs its callback again");

    XTask_Stop(&task, 1000);
    CHECK(XSYNC_ATOMIC_GET(&task.nStatus) == XTASK_STAT_STOPPED, "A stopped task reports itself stopped");

    xatomic_t nAtStop = XSYNC_ATOMIC_GET(&test.nRuns);
    xusleep(30000);
    CHECK(XSYNC_ATOMIC_GET(&test.nRuns) == nAtStop, "A stopped task never runs again");

    /* A callback returning an error ends the task on its own. */
    memset(&test, 0, sizeof(test));
    XSYNC_ATOMIC_SET(&test.nStopAfter, 3);
    memset(&task, 0, sizeof(task));
    CHECK(XTask_Start(&task, sync_task_callback, &test, 1000) == XSTDOK, "The self-stopping task starts");

    for (int i = 0; i < 500 && XSYNC_ATOMIC_GET(&task.nStatus) != XTASK_STAT_STOPPED; i++) xusleep(2000);
    CHECK(XSYNC_ATOMIC_GET(&task.nStatus) == XTASK_STAT_STOPPED, "A failing callback ends the task");
    CHECK(XSYNC_ATOMIC_GET(&test.nRuns) == 3, "The task stopped on the failing run");

    /* A task with no callback cannot start. */
    memset(&task, 0, sizeof(task));
    CHECK(XTask_Start(&task, NULL, &test, 1000) == 0, "A task without a callback is refused");
    CHECK(XSYNC_ATOMIC_GET(&task.nStatus) != XTASK_STAT_ACTIVE, "A refused task never becomes active");
    return 0;
}

static int XTest_sleep(void)
{
    /* The sleep helper has to actually wait, and a zero request has to
     * return promptly rather than blocking. */
    uint64_t nStart = XTime_GetMs();
    xusleep(50000);
    uint64_t nElapsed = XTime_GetMs() - nStart;
    CHECK(nElapsed >= 40, "A fifty millisecond sleep waits");
    CHECK(nElapsed < 5000, "A fifty millisecond sleep does not overshoot wildly");

    nStart = XTime_GetMs();
    xusleep(0);
    CHECK(XTime_GetMs() - nStart < 1000, "A zero sleep returns promptly");
    return 0;
}

static int XTest_atomics(void)
{
    /* The atomic macros have to read back exactly what was written,
     * including across the signed boundary. */
    xatomic_t nValue = 0;
    CHECK(XSYNC_ATOMIC_GET(&nValue) == 0, "A zeroed atomic reads back zero");

    XSYNC_ATOMIC_SET(&nValue, 42);
    CHECK(XSYNC_ATOMIC_GET(&nValue) == 42, "A written atomic reads back its value");

    XSYNC_ATOMIC_ADD(&nValue, 8);
    CHECK(XSYNC_ATOMIC_GET(&nValue) == 50, "An added atomic reads back the sum");

    XSYNC_ATOMIC_SUB(&nValue, 20);
    CHECK(XSYNC_ATOMIC_GET(&nValue) == 30, "A subtracted atomic reads back the difference");

    XSYNC_ATOMIC_SET(&nValue, 0);
    for (int i = 0; i < 1000; i++) XSYNC_ATOMIC_ADD(&nValue, 1);
    CHECK(XSYNC_ATOMIC_GET(&nValue) == 1000, "Repeated increments accumulate exactly");

    /* Add and subtract return the value they leave behind, on every platform:
     * that is what a reference count's "last one out frees" test relies on. */
    CHECK(XSYNC_ATOMIC_ADD(&nValue, 5) == 1005, "An add returns the new value");
    CHECK(XSYNC_ATOMIC_SUB(&nValue, 1005) == 0, "A subtract returns the new value");
    CHECK(XSYNC_ATOMIC_GET(&nValue) == 0, "The returned value is the stored one");
    return 0;
}

static int sync_idle_callback(void *pContext)
{
    (void)pContext;
    return 0;
}

static int XTest_task_start_state(void)
{
    /* The worker marks itself ACTIVE as its first step. XTask_Start() used to
       write CREATED only after starting it, which could land on top of that
       and was never replaced, so waiting for the task to become active hung. */
    for (int nRun = 0; nRun < 500; nRun++)
    {
        xtask_t task;
        memset(&task, 0, sizeof(task));
        CHECK(XTask_Start(&task, sync_idle_callback, NULL, 200) == XSTDOK, "The task starts");

        uint64_t nDeadline = XTime_GetMs() + 2000;
        while (XSYNC_ATOMIC_GET(&task.nStatus) != XTASK_STAT_ACTIVE && XTime_GetMs() < nDeadline) xusleep(50);
        CHECK(XSYNC_ATOMIC_GET(&task.nStatus) == XTASK_STAT_ACTIVE, "A started task always becomes active");

        XTask_Stop(&task, 100);
        CHECK(XSYNC_ATOMIC_GET(&task.nStatus) == XTASK_STAT_STOPPED, "The task stops");
    }

    return 0;
}

#if !defined(_WIN32) && !defined(SYNC_TSAN)
static void sync_destroy_locked(void)
{
    xsync_mutex_t lock;
    XSync_Init(&lock);
    XSync_Lock(&lock);
    XSync_Destroy(&lock);
}

static void sync_unlock_unowned(void)
{
    xsync_mutex_t lock;
    XSync_InitRecursive(&lock);
    XSync_Unlock(&lock);
}

static void sync_write_twice(void)
{
    xsync_rw_t lock;
    XRWSync_Init(&lock);
    XRWSync_WriteLock(&lock);
    XRWSync_WriteLock(&lock);
}

static void sync_read_while_writing(void)
{
    xsync_rw_t lock;
    XRWSync_Init(&lock);
    XRWSync_WriteLock(&lock);
    XRWSync_ReadLock(&lock);
}

/* Runs pMisuse in a child and returns its exit status, with what it wrote to stderr in pOutput */
static int sync_run_child(void(*pMisuse)(void), char *pOutput, size_t nSize)
{
    int pipeFds[2];
    if (pipe(pipeFds) < 0) return -1;

    pid_t nPid = fork();
    if (nPid < 0) return -1;

    if (nPid == 0)
    {
        dup2(pipeFds[1], STDERR_FILENO);
        close(pipeFds[0]);
        pMisuse();
        XTEST_EXIT(0);
    }

    close(pipeFds[1]);
    size_t nUsed = 0;
    ssize_t nRead;
    while (nUsed + 1 < nSize && (nRead = read(pipeFds[0], pOutput + nUsed, nSize - 1 - nUsed)) > 0) nUsed += (size_t)nRead;
    pOutput[nUsed] = '\0';
    close(pipeFds[0]);

    int nStatus = 0;
    if (waitpid(nPid, &nStatus, 0) != nPid || !WIFEXITED(nStatus)) return -1;
    return WEXITSTATUS(nStatus);
}
#endif

static int XTest_misuse(void)
{
#if !defined(_WIN32) && !defined(SYNC_TSAN)
    /* A lock that is used wrongly ends the process, and says why with the error the lock returned */
    struct { void(*pMisuse)(void); const char *pWhat; int nError; } cases[] = {
        { sync_destroy_locked, "Can not deinitialize mutex", EBUSY },
        { sync_unlock_unowned, "Can not unlock mutex", EPERM },
        { sync_write_twice, "Can not write lock rwlock", EDEADLK },
        { sync_read_while_writing, "Can not read lock rwlock", EDEADLK }
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++)
    {
        char sOutput[512], sExpected[128];
        snprintf(sExpected, sizeof(sExpected), "%s: %d\n", cases[i].pWhat, cases[i].nError);
        CHECK(sync_run_child(cases[i].pMisuse, sOutput, sizeof(sOutput)) == EXIT_FAILURE, "The misuse ends the process");
        CHECK(strstr(sOutput, sExpected) != NULL, "The report names the misuse and the error the lock returned");
    }
#endif
    return 0;
}

/* Writers that release with XRWSync_WriteUnlock() and readers with XRWSync_ReadUnlock(), against writers and readers
   that leave it to XRWSync_Unlock(), all at once on one lock: whichever way a lock is released, it is released as
   the kind it was taken as. On Windows that is what the mark set by a writer has to get right, and a mark cleared
   after the release used to leave the next writer's lock to be released as shared, or the next reader's as
   exclusive. */
typedef struct {
    sync_rw_test_t shared;
    xatomic_t nInside;
    xatomic_t nOverlaps;
} sync_rw_mixed_t;

static void sync_rw_mixed_write(sync_rw_mixed_t *pTest, xbool_t bExplicit)
{
    XRWSync_WriteLock(&pTest->shared.lock);
    if (XSYNC_ATOMIC_ADD(&pTest->nInside, 1) != 1) XSYNC_ATOMIC_ADD(&pTest->nOverlaps, 1);
    pTest->shared.nValue++;
    pTest->shared.nMirror = pTest->shared.nValue;
    XSYNC_ATOMIC_SUB(&pTest->nInside, 1);

    if (bExplicit) XRWSync_WriteUnlock(&pTest->shared.lock);
    else XRWSync_Unlock(&pTest->shared.lock);
}

static void *sync_rw_mixed_writer(void *pContext)
{
    sync_rw_mixed_t *pTest = (sync_rw_mixed_t*)pContext;
    for (int i = 0; i < SYNC_ROUNDS; i++) sync_rw_mixed_write(pTest, i % 2 ? XTRUE : XFALSE);
    return NULL;
}

static void *sync_rw_mixed_reader(void *pContext)
{
    sync_rw_mixed_t *pTest = (sync_rw_mixed_t*)pContext;
    for (int i = 0; !XSYNC_ATOMIC_GET(&pTest->shared.nStop); i++)
    {
        XRWSync_ReadLock(&pTest->shared.lock);
        if (XSYNC_ATOMIC_GET(&pTest->nInside)) XSYNC_ATOMIC_ADD(&pTest->nOverlaps, 1);
        if (pTest->shared.nValue != pTest->shared.nMirror) XSYNC_ATOMIC_ADD(&pTest->shared.nTornReads, 1);
        XSYNC_ATOMIC_ADD(&pTest->shared.nReads, 1);

        if (i % 2) XRWSync_ReadUnlock(&pTest->shared.lock);
        else XRWSync_Unlock(&pTest->shared.lock);
        if (i % 64 == 0) xusleep(50);
    }
    return NULL;
}

static int XTest_rwlock_unlocks(void)
{
    sync_rw_mixed_t test;
    memset(&test, 0, sizeof(test));
    XRWSync_Init(&test.shared.lock);

    /* Each kind released the way it was taken, on its own */
    XRWSync_ReadLock(&test.shared.lock);
    XRWSync_ReadLock(&test.shared.lock);
    XRWSync_ReadUnlock(&test.shared.lock);
    XRWSync_ReadUnlock(&test.shared.lock);
    XRWSync_WriteLock(&test.shared.lock);
    XRWSync_WriteUnlock(&test.shared.lock);
    XRWSync_WriteLock(&test.shared.lock);
    XRWSync_Unlock(&test.shared.lock);
    XRWSync_ReadLock(&test.shared.lock);
    XRWSync_Unlock(&test.shared.lock);

    xthread_t writers[3], readers[3];
    for (int i = 0; i < 3; i++)
        CHECK(XThread_Create(&readers[i], sync_rw_mixed_reader, &test, XFALSE) == XSTDOK, "Start a reader");
    for (int i = 0; i < 3; i++)
        CHECK(XThread_Create(&writers[i], sync_rw_mixed_writer, &test, XFALSE) == XSTDOK, "Start a writer");

    for (int i = 0; i < 3; i++) XThread_Join(&writers[i]);
    XSYNC_ATOMIC_SET(&test.shared.nStop, 1);
    for (int i = 0; i < 3; i++) XThread_Join(&readers[i]);

    CHECK(test.shared.nValue == 3 * SYNC_ROUNDS && test.shared.nMirror == test.shared.nValue, "Every write is there");
    CHECK(!XSYNC_ATOMIC_GET(&test.nOverlaps), "A writer was always alone, however the lock was released");
    CHECK(!XSYNC_ATOMIC_GET(&test.shared.nTornReads), "No reader saw a write half done");
    CHECK(XSYNC_ATOMIC_GET(&test.shared.nReads) > 0, "The readers read");

    /* A lock destroyed or never set up is left alone by both */
    XRWSync_Destroy(&test.shared.lock);
    XRWSync_ReadUnlock(&test.shared.lock);
    XRWSync_WriteUnlock(&test.shared.lock);
    CHECK(!test.shared.lock.bEnabled, "A destroyed lock stays destroyed");
    return 0;
}

#define SYNC_STRINGIFY(x) #x
#define SYNC_EXPANDED(x) SYNC_STRINGIFY(x)

static int XTest_atomic_contract(void)
{
    /* SET stores whatever it is given and hands back what was there, values other than 1 included, which
       __sync_lock_test_and_set() did not promise everywhere */
    xatomic_t nValue = 5;
    CHECK(XSYNC_ATOMIC_SET(&nValue, 7) == 5 && XSYNC_ATOMIC_GET(&nValue) == 7, "SET exchanges any value");
    CHECK(XSYNC_ATOMIC_SET(&nValue, 0) == 7 && XSYNC_ATOMIC_GET(&nValue) == 0, "Down to zero");
    CHECK(XSYNC_ATOMIC_SET(&nValue, 123456789) == 0 && XSYNC_ATOMIC_GET(&nValue) == 123456789, "And up again");

    /* CAS stores only over the value it expects, and says whether it did */
    CHECK(XSYNC_ATOMIC_CAS(&nValue, 123456789, 42) && XSYNC_ATOMIC_GET(&nValue) == 42, "A CAS over the value stores");
    CHECK(!XSYNC_ATOMIC_CAS(&nValue, 123456789, 7) && XSYNC_ATOMIC_GET(&nValue) == 42, "One over another does not");

#ifndef _WIN32
    /* It is a full barrier: an acquire barrier let a store before it be seen after it, which no test on a strongly
       ordered processor can tell apart, so the contract itself is checked */
    const char *pExpansion = SYNC_EXPANDED(XSYNC_ATOMIC_SET(&nValue, 1));
    char sSeqCst[16];
    snprintf(sSeqCst, sizeof(sSeqCst), ", %d)", __ATOMIC_SEQ_CST);
    CHECK(strstr(pExpansion, "__atomic_exchange_n") != NULL && strstr(pExpansion, sSeqCst) != NULL,
        "SET is a sequentially consistent exchange");
#endif
    return 0;
}

/* A controller raises the barrier, and a worker hands it something before acknowledging it, round after round. An
   acknowledgement the controller never saw would leave it waiting: the wait has a deadline, so that is a failure
   rather than a hang. */
typedef struct {
    xsync_bar_t bar;
    xatomic_t nStop;
    long nHanded;
    long nRounds;
} sync_bar_protocol_t;

/* The protocol of the barrier: wait for it, hand over what it is for, acknowledge it, and wait while the
   acknowledgement holds. The controller lowers one barrier and raises the next straight away, which a thread
   waiting for the barrier to be seen down could miss, and which a thread that looked for a barrier that was up
   and not acknowledged, in two looks, could take for the one it had already answered. */
static void *sync_bar_responder(void *pContext)
{
    sync_bar_protocol_t *pTest = (sync_bar_protocol_t*)pContext;
    long nRound = 0;

    while (!XSYNC_ATOMIC_GET(&pTest->nStop))
    {
        if (!XSyncBar_CheckBar(&pTest->bar))
        {
            xyield();
            continue;
        }

        pTest->nHanded = ++nRound;
        XSyncBar_Ack(&pTest->bar);

        while (XSyncBar_CheckAck(&pTest->bar) && !XSYNC_ATOMIC_GET(&pTest->nStop)) xyield();
    }

    return NULL;
}

static int XTest_barrier_protocol(void)
{
    sync_bar_protocol_t test;
    memset(&test, 0, sizeof(test));
    XSyncBar_Reset(&test.bar);

    xthread_t responder;
    CHECK(XThread_Create(&responder, sync_bar_responder, &test, XFALSE) == XSTDOK, "Start the responder");

    int nLost = 0;
    for (long nRound = 1; nRound <= 2000 && !nLost; nRound++)
    {
        XSyncBar_Bar(&test.bar);
        if (XSyncBar_WaitAckFor(&test.bar, nRound % 2 ? 0 : 50, 5000000) != XSTDOK) nLost = 1;
        else if (test.nHanded != nRound) nLost = 2;
        XSyncBar_Reset(&test.bar);
        test.nRounds = nRound;
    }

    XSYNC_ATOMIC_SET(&test.nStop, 1);
    XThread_Join(&responder);
    CHECK(nLost != 1, "No acknowledgement was lost");
    CHECK(nLost != 2, "And each came with what was handed over for it");
    CHECK(test.nRounds == 2000, "Every round completed");
    return 0;
}

static int XTest_barrier_values(void)
{
    /* A reset sets a barrier up, whatever its memory held, and every barrier raised is a value of its own: an
       acknowledgement of one never answers another, raised on the same barrier or on any other */
    xsync_bar_t bars[3];
    memset(bars, 0xA5, sizeof(bars));
    for (int i = 0; i < 3; i++)
    {
        XSyncBar_Reset(&bars[i]);
        CHECK(!XSyncBar_CheckBar(&bars[i]) && !XSyncBar_CheckAck(&bars[i]) && !bars[i].nBar && !bars[i].nAck,
            "A reset barrier is lowered and unacknowledged, from any memory");
    }

    xatomic_t nLast[3] = {0, 0, 0};
    for (int nRound = 0; nRound < 2000; nRound++)
    {
        xsync_bar_t *pBar = &bars[nRound % 3];
        xatomic_t nAcked = pBar->nAck;

        XSyncBar_Bar(pBar);
        CHECK(XSyncBar_CheckBar(pBar) && !XSyncBar_CheckAck(pBar) && !pBar->nAck, "Raised, and not acknowledged");
        CHECK((pBar->nBar & 1) && (xatomic_t)(pBar->nBar + 1) != 0, "As a value with an acknowledgement of its own");
        for (int i = 0; i < 3; i++) CHECK(bars[i].nBar != nLast[i] || !nLast[i], "Never a value seen before");

        /* What answered the barrier before does not answer this one */
        pBar->nAck = nAcked;
        CHECK(!XSyncBar_CheckAck(pBar), "An acknowledgement of another barrier does not answer it");

        XSyncBar_Ack(pBar);
        CHECK(XSyncBar_CheckAck(pBar) && XSyncBar_WaitAckFor(pBar, 0, 0) == XSTDOK, "Its own does");
        nLast[nRound % 3] = pBar->nBar;

        /* Raised again without being lowered, it is another barrier */
        nAcked = pBar->nAck;
        XSyncBar_Bar(pBar);
        pBar->nAck = nAcked;
        CHECK(!XSyncBar_CheckAck(pBar), "A barrier raised again is not answered by the one before");
        XSyncBar_Ack(pBar);

        if (nRound % 2) XSyncBar_Reset(pBar);
        CHECK(!(nRound % 2) || (!XSyncBar_CheckBar(pBar) && !XSyncBar_CheckAck(pBar)), "Lowered");
    }

    return 0;
}

static int XTest_barrier_wait_for(void)
{
    /* A wait with a deadline gives up at it, sleeping or not, and takes an acknowledgement that is there */
    sync_bar_test_t test;
    memset(&test, 0, sizeof(test));
    XSyncBar_Reset(&test.bar);
    XSyncBar_Bar(&test.bar);

    for (int nSleep = 0; nSleep < 2; nSleep++)
    {
        uint64_t nStart = XTime_GetMonoMs();
        CHECK(XSyncBar_WaitAckFor(&test.bar, nSleep ? 1000 : 0, 20000) == XSTDNON, "Nobody acknowledges in time");
        uint64_t nElapsed = XTime_GetMonoMs() - nStart;
        CHECK(nElapsed >= 20 && nElapsed < 5000, "The wait lasts until its deadline, and not much past it");
    }

    CHECK(XSyncBar_WaitAckFor(&test.bar, 1000, 0) == XSTDNON, "A deadline already passed is a single look");
    XSyncBar_Ack(&test.bar);
    CHECK(XSyncBar_WaitAckFor(&test.bar, 1000, 0) == XSTDOK, "Which finds an acknowledgement that is there");

    /* One that comes while waiting, without sleeps between the looks */
    XSyncBar_Reset(&test.bar);
    xthread_t worker;
    CHECK(XThread_Create(&worker, sync_bar_worker, &test, XFALSE) == XSTDOK, "Start the barrier worker");
    XSyncBar_Bar(&test.bar);
    CHECK(XSyncBar_WaitAckFor(&test.bar, 0, 5000000) == XSTDOK, "An acknowledgement in time ends the wait");
    XThread_Join(&worker);

    XSyncBar_Bar(&test.bar);
    CHECK(XThread_Create(&worker, sync_bar_worker, &test, XFALSE) == XSTDOK, "Start the barrier worker again");
    CHECK(XSyncBar_WaitAck(&test.bar, 0) == 0, "A wait that yields instead of sleeping slept for nothing");
    XThread_Join(&worker);
    return 0;
}

static int XTest_task_unreachable(void)
{
    /* A wait for what a task can no longer do returns rather than waiting forever */
    xtask_t task;
    memset(&task, 0, sizeof(task));
    uint64_t nStart = XTime_GetMonoMs();
    CHECK(XTask_Stop(&task, 1000) == 0, "A task that never started is not waited for to stop");
    CHECK(XTask_Hold(&task, 1000) == 0 && XTask_Release(&task, 1000) == 0, "Nor to pause or to go on");

    sync_task_test_t test;
    memset(&test, 0, sizeof(test));
    XSYNC_ATOMIC_SET(&test.nStopAfter, 1);
    CHECK(XTask_Start(&task, sync_task_callback, &test, 1000) == XSTDOK, "A task that stops itself starts");
    CHECK(XTask_Stop(&task, 0) == 0 && XSYNC_ATOMIC_GET(&task.nStatus) == XTASK_STAT_STOPPED, "And stops");
    CHECK(XTask_Hold(&task, 1000) == 0, "A stopped task is not waited for to pause");
    CHECK(XTask_Release(&task, 1000) == 0, "Nor to go on");
    CHECK(XTask_Stop(&task, 1000) == 0, "And stopping it again does not wait");
    CHECK(XTime_GetMonoMs() - nStart < 5000, "None of it waited");

    /* A wait without an interval yields between its looks, and still sees the task get there */
    memset(&test, 0, sizeof(test));
    memset(&task, 0, sizeof(task));
    CHECK(XTask_Start(&task, sync_task_callback, &test, 1000) == XSTDOK, "A task starts");
    CHECK(XTask_Hold(&task, 0) == 0 && XSYNC_ATOMIC_GET(&task.nStatus) == XTASK_STAT_PAUSED, "It pauses");
    CHECK(XTask_Release(&task, 0) == 0 && XSYNC_ATOMIC_GET(&task.nStatus) == XTASK_STAT_ACTIVE, "And goes on");
    CHECK(XTask_Stop(&task, -1) == 0, "A stop that does not wait does not");
    CHECK(XTask_Stop(&task, 0) == 0 && XSYNC_ATOMIC_GET(&task.nStatus) == XTASK_STAT_STOPPED, "One that does sees it");
    return 0;
}

/* The worker thread of a joinable task is gone by the time XTask_Stop() returns: what a thread has set to be done
   at its exit has been done, and that is the last thing it does */
static pthread_key_t g_taskExitKey;
static xatomic_t g_nTaskExits;
/* It takes its time, so that only a stop that waited for the thread to be gone can find it done */
static void sync_task_exited(void *pValue)
{
    (void)pValue;
    xusleep(100000);
    XSYNC_ATOMIC_ADD(&g_nTaskExits, 1);
}

static int sync_task_marked(void *pContext)
{
    if (pthread_getspecific(g_taskExitKey) == NULL) pthread_setspecific(g_taskExitKey, pContext);
    return sync_task_callback(pContext);
}

static int XTest_task_joinable(void)
{
    CHECK(pthread_key_create(&g_taskExitKey, sync_task_exited) == 0, "Create the exit marker");
    sync_task_test_t test;
    xtask_t task;

    for (int nSelf = 0; nSelf < 2; nSelf++)
    {
        memset(&test, 0, sizeof(test));
        memset(&task, 0, sizeof(task));
        XSYNC_ATOMIC_SET(&g_nTaskExits, 0);
        if (nSelf) XSYNC_ATOMIC_SET(&test.nStopAfter, 2);

        CHECK(XTask_StartJoinable(&task, sync_task_marked, &test, 1000) == XSTDOK, "A joinable task starts");
        CHECK(!task.taskTd.nDetached, "Its thread is joinable");
        for (int i = 0; i < 500 && XSYNC_ATOMIC_GET(&test.nRuns) < 2; i++) xusleep(1000);

        XTask_Stop(&task, 1000);
        CHECK(XSYNC_ATOMIC_GET(&task.nStatus) == XTASK_STAT_STOPPED, "It stops, by itself or when asked");
        CHECK(XSYNC_ATOMIC_GET(&g_nTaskExits) == 1, "And its thread has finished when the stop returns");
        CHECK(task.taskTd.nDetached, "Joined, it is not joined again");
        CHECK(XTask_Stop(&task, 1000) == 0, "A second stop returns at once");
    }

    /* A stop that does not wait leaves the thread to the next one that does */
    memset(&test, 0, sizeof(test));
    memset(&task, 0, sizeof(task));
    XSYNC_ATOMIC_SET(&g_nTaskExits, 0);
    CHECK(XTask_StartJoinable(&task, sync_task_marked, &test, 1000) == XSTDOK, "Another joinable task starts");
    for (int i = 0; i < 500 && XSYNC_ATOMIC_GET(&test.nRuns) < 1; i++) xusleep(1000);
    XTask_Stop(&task, -1);
    XTask_Stop(&task, 1000);
    CHECK(XSYNC_ATOMIC_GET(&g_nTaskExits) == 1 && task.taskTd.nDetached, "Which joins it");
    CHECK(XTask_StartJoinable(&task, NULL, &test, 1000) == 0, "A joinable task needs a callback too");

    pthread_key_delete(g_taskExitKey);
    return 0;
}

/* Exported, though not declared in the header */
void *XTask_WorkerThread(void *pContext);
uint32_t XTask_Wait(xtask_t *pTask, int nEvent, int nIntervalU);

static void* XTest_NoWork(void *pArg) { return pArg; }
static int XTest_NoTask(void *pCtx) { (void)pCtx; return -1; }

static int XTest_null_args(void)
{
    /* No lock, barrier, thread or task: each call returns rather than faulting, and a
       wait for something that is not there returns at once instead of waiting forever */
    XSync_Init(NULL);
    XSync_InitRecursive(NULL);
    CHECK(XSync_InitAdv(NULL, XTRUE) == XSTDERR, "No mutex is initialized");
    XSync_Lock(NULL);
    XSync_Unlock(NULL);
    XSync_Destroy(NULL);

    XRWSync_Init(NULL);
    XRWSync_ReadLock(NULL);
    XRWSync_ReadUnlock(NULL);
    XRWSync_WriteLock(NULL);
    XRWSync_WriteUnlock(NULL);
    XRWSync_Unlock(NULL);
    XRWSync_Destroy(NULL);

    XSyncBar_Bar(NULL);
    XSyncBar_Ack(NULL);
    XSyncBar_Reset(NULL);
    CHECK(XSyncBar_CheckBar(NULL) == XSTDNON && XSyncBar_CheckAck(NULL) == XSTDNON, "No barrier is up or answered");
    CHECK(XSyncBar_WaitAck(NULL, 0) == 0 && XSyncBar_WaitAckFor(NULL, 0, 1000000) == XSTDNON, "Nor waited for");

    XThread_Init(NULL);
    CHECK(XThread_Run(NULL) == XSTDERR && XThread_Create(NULL, XTest_NoWork, NULL, 0) == XSTDERR, "No thread runs");
    CHECK(XThread_Join(NULL) == NULL && XTask_WorkerThread(NULL) == NULL, "Or is joined");

    /* A thread that never ran has no id to join, pthread_join() was handed whatever was there */
    xthread_t idle;
    XThread_Init(&idle);
    CHECK(XThread_Join(&idle) == NULL, "A thread that never started is not joined");
    CHECK(XThread_Run(&idle) == XSTDNON && XThread_Join(&idle) == NULL, "Nor one that had nothing to run");

    CHECK(XTask_Start(NULL, XTest_NoTask, NULL, 0) == XSTDERR, "No task starts");
    CHECK(XTask_StartJoinable(NULL, XTest_NoTask, NULL, 0) == XSTDERR, "Joinable or not");
    CHECK(XTask_Wait(NULL, XTASK_STAT_STOPPED, 0) == 0 && XTask_Hold(NULL, 0) == 0, "Nor is waited for");
    CHECK(XTask_Release(NULL, 0) == 0 && XTask_Stop(NULL, 0) == 0, "Released or stopped");
    return 0;
}

XTEST_MAIN(
    XTEST_CASE(null_args),
    XTEST_CASE(mutex),
    XTEST_CASE(recursive),
    XTEST_CASE(rwlock),
    XTEST_CASE(rwlock_unlocks),
    XTEST_CASE(barrier),
    XTEST_CASE(atomic_contract),
    XTEST_CASE(barrier_protocol),
    XTEST_CASE(barrier_values),
    XTEST_CASE(barrier_wait_for),
    XTEST_CASE(task_unreachable),
    XTEST_CASE(task_joinable),
    XTEST_CASE(task),
    XTEST_CASE(task_start_state),
    XTEST_CASE(sleep),
    XTEST_CASE(atomics),
    XTEST_CASE(misuse)
)
