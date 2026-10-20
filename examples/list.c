/*!
 *  @file libxutils/examples/list.c
 *
 *  This source is part of the "libxutils" project
 *  2015-2026  Sandro Kalatozishvili (s.kalatoz@gmail.com)
 *
 * @brief Example of working with linked lists: a small job scheduler.
 * Jobs are queued at the back and the front of a list, found with a
 * search callback, cancelled, and finally run round-robin on a ring
 * until every job is done. The list owns the jobs: a clear callback
 * frees each one when its node is unlinked.
 */

#include "xstd.h"
#include "list.h"
#include "log.h"
#include "str.h"

typedef struct {
    char sName[XSTR_MICRO];
    int nWork;
} xjob_t;

static xjob_t* create_job(const char *pName, int nWork)
{
    xjob_t *pJob = (xjob_t*)malloc(sizeof(xjob_t));
    if (pJob == NULL) return NULL;

    xstrncpy(pJob->sName, sizeof(pJob->sName), pName);
    pJob->nWork = nWork;
    return pJob;
}

/* Called for the data of every node that is unlinked or cleared, pCtx counts them */
static void clear_job(void *pCtx, void *pData)
{
    size_t *pFreed = (size_t*)pCtx;
    (*pFreed)++;
    free(pData);
}

/* Search callback: >0 found, 0 keep searching, <0 stop the search */
static int match_name(void *pUserPtr, xlist_t *pNode)
{
    const xjob_t *pJob = (const xjob_t*)pNode->data.pData;
    return xstrcmp(pJob->sName, (const char*)pUserPtr) ? 1 : 0;
}

static void print_jobs(xlist_t *pList, const char *pTitle)
{
    xlog_wn("%s:", pTitle);
    xlist_t *pNode = XList_GetHead(pList);

    while (pNode != NULL)
    {
        const xjob_t *pJob = (const xjob_t*)pNode->data.pData;
        xlog_wn(" %s(%d)", pJob->sName, pJob->nWork);
        pNode = pNode->pNext;
    }

    xlog(XSTR_EMPTY);
}

static xlist_t* queue_job(xlist_t *pList, const char *pName, int nWork, xbool_t bUrgent)
{
    xjob_t *pJob = create_job(pName, nWork);
    if (pJob == NULL) return NULL;

    /* New nodes take over the clear callback and its context from the list */
    xlist_t *pNode = bUrgent ? XList_PushFront(pList, pJob, 0) : XList_PushBack(pList, pJob, 0);
    if (pNode == NULL) free(pJob);

    return pNode;
}

int main()
{
    xlog_defaults();
    xlog_enable(XLOG_INFO);
    size_t nFreed = 0;

//////////////////////////////////////////////////////////////////////////////////////////////
// Queue the jobs: the first node makes the list, the others are pushed to it
//////////////////////////////////////////////////////////////////////////////////////////////

    xjob_t *pFirst = create_job("compile", 3);
    xlist_t *pQueue = pFirst != NULL ? XList_New(pFirst, 0, clear_job, &nFreed) : NULL;

    if (pQueue == NULL)
    {
        xloge("Failed to create the job queue");
        free(pFirst);
        return XSTDERR;
    }

    if (queue_job(pQueue, "test", 2, XFALSE) == NULL ||
        queue_job(pQueue, "package", 1, XFALSE) == NULL ||
        queue_job(pQueue, "deploy", 2, XFALSE) == NULL ||
        queue_job(pQueue, "hotfix", 1, XTRUE) == NULL)
    {
        xloge("Failed to queue a job");
        XList_Clear(pQueue);
        return XSTDERR;
    }

    /* Any node is a handle to the whole list, the head is the urgent job now */
    pQueue = XList_GetHead(pQueue);
    print_jobs(pQueue, "Queued");

//////////////////////////////////////////////////////////////////////////////////////////////
// Find a job and change it in place
//////////////////////////////////////////////////////////////////////////////////////////////

    xlist_t *pNode = XList_Search(pQueue, "test", match_name);
    if (pNode == NULL)
    {
        xloge("The test job was not found");
        XList_Clear(pQueue);
        return XSTDERR;
    }

    ((xjob_t*)pNode->data.pData)->nWork = 4;
    print_jobs(pQueue, "More tests");

//////////////////////////////////////////////////////////////////////////////////////////////
// Cancel a job: the node is unlinked and the clear callback frees the job
//////////////////////////////////////////////////////////////////////////////////////////////

    /* Remove() returns a neighbour of the removed node, the node itself is gone */
    xlist_t *pNeighbour = XList_Remove(pQueue, "package", match_name);
    if (pNeighbour != NULL) pQueue = XList_GetHead(pNeighbour);
    print_jobs(pQueue, "Package cancelled");

//////////////////////////////////////////////////////////////////////////////////////////////
// Run the jobs round-robin: the ring goes on until the last job is unlinked
//////////////////////////////////////////////////////////////////////////////////////////////

    pNode = XList_MakeRing(pQueue);
    xlog("The queue is a ring now: %s", XList_IsRing(pNode) ? "yes" : "no");
    size_t nSteps = 0;

    while (pNode != NULL)
    {
        xjob_t *pJob = (xjob_t*)pNode->data.pData;
        pJob->nWork--;
        nSteps++;

        if (pJob->nWork > 0)
        {
            xlog("  step %zu: %s, %d left", nSteps, pJob->sName, pJob->nWork);
            pNode = pNode->pNext;
            continue;
        }

        /* Unlink() frees the node and returns the next one, NULL after the last one */
        xlog("  step %zu: %s is done", nSteps, pJob->sName);
        pNode = XList_Unlink(pNode);
    }

    xlog("All jobs are done in %zu steps, %zu jobs were freed", nSteps, nFreed);
    return nFreed == 5 ? 0 : XSTDERR;
}
