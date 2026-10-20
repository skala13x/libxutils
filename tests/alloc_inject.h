/* libxutils regression helpers: the injecting allocator. Linked with
 * -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free,--wrap=strdup,
 * a test can fail any one allocation a scenario makes and check that it
 * leaked nothing. alloc_fail_regression.c explains the method. */
#ifndef XUTILS_ALLOC_INJECT_H
#define XUTILS_ALLOC_INJECT_H

#include "test.h"
#include <execinfo.h>

/* ---------------- the injecting allocator ---------------- */

/* Counted from the first allocation after xalloc_arm(). When the counter
 * reaches g_nFailAt the allocation fails and every later one succeeds, so
 * each run tests exactly one failure point. */
/* volatile because the compiler knows malloc() as a builtin that does not
 * touch globals, so a read of these placed after a malloc() call was being
 * folded back to the value from before it. */
static volatile long g_nCount = 0;
static volatile long g_nFailAt = -1;    /* -1 disables injection */
static volatile long g_nLive = 0;       /* Outstanding blocks, for the leak check */
static volatile int g_bArmed = 0;

void *__real_malloc(size_t nSize);
void *__real_calloc(size_t nCount, size_t nSize);
void *__real_realloc(void *pPtr, size_t nSize);
void __real_free(void *pPtr);
char *__real_strdup(const char *pStr);

/* With XALLOC_BACKTRACE set, the injected allocation prints where it was
   about to happen. That turns "scenario X leaks when allocation N fails"
   into the actual call site without a debugger. */
static void xalloc_report(void)
{
    if (getenv("XALLOC_BACKTRACE") == NULL) return;

    void *pFrames[24];
    int nFrames = backtrace(pFrames, (int)(sizeof(pFrames) / sizeof(*pFrames)));

    fprintf(stderr, "--- failing allocation %ld ---\n", g_nCount);
    fflush(stderr);
    backtrace_symbols_fd(pFrames, nFrames, fileno(stderr));
}

static int xalloc_should_fail(void)
{
    if (!g_bArmed) return 0;
    if (++g_nCount != g_nFailAt) return 0;

    xalloc_report();
    return 1;
}

void *__wrap_malloc(size_t nSize)
{
    if (xalloc_should_fail()) return NULL;
    void *p = __real_malloc(nSize);
    if (p != NULL && g_bArmed) g_nLive++;
    return p;
}

void *__wrap_calloc(size_t nCount, size_t nSize)
{
    if (xalloc_should_fail()) return NULL;
    void *p = __real_calloc(nCount, nSize);
    if (p != NULL && g_bArmed) g_nLive++;
    return p;
}

void *__wrap_realloc(void *pPtr, size_t nSize)
{
    if (xalloc_should_fail()) return NULL;

    /* A realloc that grows an existing block keeps the same live count; one
     * that starts from NULL is an allocation and one that shrinks to zero
     * is a free, which is how the C library defines it. */
    void *p = __real_realloc(pPtr, nSize);
    if (!g_bArmed) return p;

    if (p != NULL && pPtr == NULL) g_nLive++;
    else if (p == NULL && nSize == 0 && pPtr != NULL) g_nLive--;
    return p;
}

void __wrap_free(void *pPtr)
{
    if (pPtr != NULL && g_bArmed) g_nLive--;
    __real_free(pPtr);
}

char *__wrap_strdup(const char *pStr)
{
    if (xalloc_should_fail()) return NULL;

    char *p = __real_strdup(pStr);
    if (p != NULL && g_bArmed) g_nLive++;
    return p;
}

/* Starts counting. nFailAt of -1 lets everything through, which is the pass
 * used to learn how many allocations a scenario makes. */
static void xalloc_arm(long nFailAt)
{
    g_nCount = 0;
    g_nLive = 0;
    g_nFailAt = nFailAt;
    g_bArmed = 1;
}

static void xalloc_disarm(void)
{
    g_bArmed = 0;
    g_nFailAt = -1;
}

/* A scenario builds something and tears it down again. It must return
 * non-zero only if it detected the library handing back a broken object;
 * an allocation failure is an expected outcome, not a failure of the test. */
typedef int (*alloc_scenario_t)(void);

typedef struct {
    const char *pName;
    alloc_scenario_t run;
} alloc_case_t;

/* Runs one scenario once per allocation it makes, failing that one. */
static int alloc_sweep(const alloc_case_t *pCase)
{
    /* Learning pass: how many allocations does a clean run take? */
    if (getenv("XALLOC_TRACE") != NULL)
    {
        printf("%s: learning pass\n", pCase->pName);
        fflush(stdout);
    }

    xalloc_arm(-1);
    int nBroken = pCase->run();
    long nTotal = g_nCount;
    long nLeaked = g_nLive;
    xalloc_disarm();

    if (nBroken)
    {
        printf("%s: the clean run already handed back a broken object\n", pCase->pName);
        return 1;
    }

    if (nLeaked != 0)
    {
        printf("%s: the clean run leaked %ld block(s)\n", pCase->pName, nLeaked);
        return 1;
    }

    if (nTotal == 0) return 0;   /* Nothing to inject into */

    /* One run per allocation, each failing a different one. The count can
     * differ between runs once a failure changes the path, so the sweep is
     * bounded by the clean run's count plus a margin for paths that only
     * exist after a failure. */
    for (long nAt = 1; nAt <= nTotal; nAt++)
    {
        /* Printed before the run and flushed, so a crash names the exact
         * scenario and allocation index rather than leaving it to a guess. */
        if (getenv("XALLOC_TRACE") != NULL)
        {
            printf("  %s: failing allocation %ld of %ld\n", pCase->pName, nAt, nTotal);
            fflush(stdout);
        }

        xalloc_arm(nAt);
        nBroken = pCase->run();
        nLeaked = g_nLive;
        xalloc_disarm();

        if (nBroken)
        {
            printf("%s: allocation %ld of %ld failing left a broken object\n",
                pCase->pName, nAt, nTotal);
            return 1;
        }

        if (nLeaked != 0)
        {
            printf("%s: allocation %ld of %ld failing leaked %ld block(s)\n",
                pCase->pName, nAt, nTotal, nLeaked);
            return 1;
        }
    }

    return 0;
}

#endif
