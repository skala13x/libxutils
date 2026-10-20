/*!
 *  @file libxutils/src/data/array.c
 *
 *  This source is part of the "libxutils" project
 *  2015-2020  Sandro Kalatozishvili (s.kalatoz@gmail.com)
 *
 * @brief Dynamically allocated data holder
 * with some sorting and search algorithms.
 */

#include <string.h>
#include "array.h"

/* Indexes go out as int, so a table has no more slots than
   an int counts, and its size in bytes has to fit size_t */
#define XARRAY_MAX_SLOTS XSTD_MIN((size_t)INT_MAX, SIZE_MAX / sizeof(xarray_data_t*))

/* Ranges up to this many items are sorted by insertion,
    which is faster than partitioning them any further */
#define XARRAY_INSERTION_SORT 16

xarray_data_t *XArray_NewData(xarray_t *pArr, void *pData, size_t nSize, uint32_t nKey)
{
    if (pArr == NULL) return NULL;
    xarray_data_t *pNewData = (xarray_data_t *)xalloc(pArr->pPool, sizeof(xarray_data_t));
    if (pNewData == NULL) return NULL;

    if (pData != NULL && nSize > 0)
    {
        pNewData->pData = xalloc(pArr->pPool, nSize);
        if (pNewData->pData == NULL)
        {
            xfreen(pArr->pPool, pNewData, sizeof(xarray_data_t));
            return NULL;
        }

        memcpy(pNewData->pData, pData, nSize);
        pNewData->nSize = nSize;
    }
    else
    {
        pNewData->pData = pData;
        pNewData->nSize = 0;
    }

    pNewData->pPool = pArr->pPool;
    pNewData->nKey = nKey;
    return pNewData;
}

void XArray_FreeData(xarray_data_t *pArrData)
{
    if (pArrData != NULL)
    {
        if (pArrData->pData && pArrData->nSize)
            xfreen(pArrData->pPool, pArrData->pData, pArrData->nSize);

        xfreen(pArrData->pPool, pArrData, sizeof(xarray_data_t));
    }
}

void XArray_ClearData(xarray_t *pArr, xarray_data_t *pArrData)
{
    if (pArr != NULL &&
        pArrData != NULL &&
        pArr->clearCb != NULL)
    {
        pArr->clearCb(pArrData);
        pArrData->pData = NULL;
        pArrData->nSize = 0;
    }

    XArray_FreeData(pArrData);
}

/* Gives the array a table of nSize empty slots, or no table and no size */
static xarray_data_t** XArray_NewTable(xarray_t *pArr, size_t nSize)
{
    pArr->pData = NULL;
    pArr->nSize = 0;
    if (!nSize) return NULL;

    /* A count whose size in bytes wrapped round got a short table,
       and clearing its slots ran past the end of it */
    if (nSize <= XARRAY_MAX_SLOTS)
        pArr->pData = (xarray_data_t**)xalloc(pArr->pPool, nSize * sizeof(xarray_data_t*));

    if (pArr->pData == NULL) return NULL;
    memset(pArr->pData, 0, nSize * sizeof(xarray_data_t*));
    pArr->nSize = nSize;
    return pArr->pData;
}

void* XArray_Init(xarray_t *pArr, xpool_t *pPool, size_t nSize, uint8_t nFixed)
{
    if (pArr == NULL) return NULL;
    pArr->eStatus = XARRAY_STATUS_EMPTY;
    pArr->clearCb = NULL;
    pArr->nFixed = nFixed;
    pArr->nAlloc = 0;
    pArr->nUsed = 0;
    pArr->pPool = pPool;
    pArr->nHasPool = 0;
    return XArray_NewTable(pArr, nSize);
}

void* XArray_InitPool(xarray_t *pArr, size_t nPoolSize, size_t nSize, uint8_t nFixed)
{
    XCHECK((pArr != NULL), NULL);

    xpool_t *pPool = XPool_Create(nPoolSize);
    if (pPool == NULL)
    {
        XArray_Init(pArr, NULL, XSTDNON, nFixed);
        return NULL;
    }

    void *pData = XArray_Init(pArr, pPool, nSize, nFixed);
    if (nSize && pData == NULL)
    {
        XPool_Destroy(pPool);
        XArray_Init(pArr, NULL, XSTDNON, nFixed);
        return NULL;
    }

    pArr->nHasPool = 1;
    return pData;
}

xarray_t* XArray_New(xpool_t *pPool, size_t nSize, uint8_t nFixed)
{
    xarray_t *pArr = (xarray_t*)xalloc(pPool, sizeof(xarray_t));
    if (pArr == NULL) return NULL;

    if (XArray_Init(pArr, pPool, nSize, nFixed) == NULL && nSize)
    {
        xfreen(pPool, pArr, sizeof(xarray_t));
        return NULL;
    }

    pArr->nAlloc = 1;
    return pArr;
}

xarray_t* XArray_NewPool(size_t nPoolSize, size_t nSize, uint8_t nFixed)
{
    xpool_t *pPool = XPool_Create(nPoolSize);
    if (pPool == NULL) return NULL;

    xarray_t *pArr = XArray_New(pPool, nSize, nFixed);
    if (pArr == NULL)
    {
        XPool_Destroy(pPool);
        return NULL;
    }

    pArr->nHasPool = 1;
    return pArr;
}

/* The chunk of the pool chain the block is in */
static xpool_t* XArray_PoolOf(xpool_t *pPool, const void *pData)
{
    const uint8_t *pByte = (const uint8_t*)pData;

    while (pPool != NULL)
    {
        if (pPool->pData != NULL &&
            pByte >= pPool->pData &&
            pByte < pPool->pData + pPool->nSize)
            return pPool;

        pPool = pPool->pNext;
    }

    return NULL;
}

/* Releases every item. A slot past the used ones can hold one too, XArray_Set() stores there. */
static void XArray_ClearItems(xarray_t *pArr)
{
    size_t i;
    for (i = 0; i < pArr->nSize; i++)
    {
        XArray_ClearData(pArr, pArr->pData[i]);
        pArr->pData[i] = NULL;
    }

    pArr->eStatus = XARRAY_STATUS_EMPTY;
    pArr->nUsed = 0;
}

void XArray_Clear(xarray_t *pArr)
{
    XCHECK_VOID_NL((pArr != NULL));
    XArray_ClearItems(pArr);
    if (!pArr->nHasPool || pArr->pPool == NULL) return;

    /* The whole pool goes back, but for the array if it lives there itself.
       XArray_NewPool() made it the first block of its chunk, so the pool is
       reset and that block taken again. Kept out of the reset as a whole,
       the pool only got the items back one by one, it reclaims nothing but
       its last block that way, and every refill grew it. */
    xpool_t *pOwner = XArray_PoolOf(pArr->pPool, pArr);
    if (pOwner != NULL && (uint8_t*)pArr != pOwner->pData) return;

    size_t nSize = pArr->nSize;
    XPool_Reset(pArr->pPool);
    if (pOwner != NULL) pOwner->nUsed = XPOOL_ALIGN(sizeof(xarray_t));
    XArray_NewTable(pArr, nSize);
}

void XArray_Destroy(xarray_t *pArr)
{
    XCHECK_VOID_NL((pArr != NULL));
    XArray_ClearItems(pArr);

    xpool_t *pPool = pArr->pPool;
    uint8_t nAlloc = pArr->nAlloc;
    uint8_t nHasPool = pArr->nHasPool;

    /* With its size, so a pool the table came from can take it back */
    xfreen(pPool, pArr->pData, pArr->nSize * sizeof(xarray_data_t*));
    pArr->pData = NULL;
    pArr->pPool = NULL;
    pArr->nSize = 0;
    pArr->nFixed = 0;
    pArr->nAlloc = 0;
    pArr->nHasPool = 0;

    if (nAlloc) xfreen(pPool, pArr, sizeof(xarray_t));
    if (nHasPool) XPool_Destroy(pPool);
}

void XArray_Free(xarray_t **ppArr)
{
    if (ppArr == NULL ||
        *ppArr == NULL) return;

    xarray_t *pArr = *ppArr;
    XArray_Destroy(pArr);
    *ppArr = NULL;
}

uint8_t XArray_Contains(xarray_t *pArr, size_t nIndex)
{
    if (pArr == NULL || nIndex >= pArr->nSize) return 0;
    return pArr->pData[nIndex] != NULL ? 1 : 0;
}

size_t XArray_Realloc(xarray_t *pArr)
{
    if (pArr == NULL) return 0;
    if (pArr->nFixed) return pArr->nSize;

    size_t nUsed = pArr->nUsed, nSize = pArr->nSize;
    if (!nUsed) return nSize;

    if (nUsed == nSize)
    {
        /* Past this the doubled size in bytes wrapped round, and the
           short table that came back was then filled past its end */
        if (nSize > XARRAY_MAX_SLOTS / 2)
        {
            pArr->eStatus = XARRAY_STATUS_NO_MEMORY;
            return 0;
        }

        nSize *= 2;
    }
    else if (nUsed * 4 < nSize)
    {
        /* Only empty slots are cut. XArray_Set() stores past the used slots,
           and a shrink that cut such an item lost it with nothing to free it. */
        size_t i;
        for (i = nSize / 2; i < nSize; i++)
            if (pArr->pData[i] != NULL) return nSize;

        nSize /= 2;
    }
    else return nSize;

    /* Resized in place where the allocator can */
    xarray_data_t **pData = (xarray_data_t**)xrealloc(pArr->pPool, pArr->pData,
        pArr->nSize * sizeof(xarray_data_t*), nSize * sizeof(xarray_data_t*));

    if (pData == NULL)
    {
        pArr->eStatus = XARRAY_STATUS_NO_MEMORY;
        return 0;
    }

    if (nSize > pArr->nSize)
        memset(pData + pArr->nSize, 0, (nSize - pArr->nSize) * sizeof(xarray_data_t*));

    pArr->pData = pData;
    pArr->nSize = nSize;
    return nSize;
}

size_t XArray_CheckSpace(xarray_t *pArr)
{
    if (pArr == NULL) return 0;
    if (pArr->nUsed < pArr->nSize) return 1;

    if (pArr->pData == NULL)
    {
        /* Storage made on demand is not held to a fixed size */
        pArr->eStatus = XARRAY_STATUS_EMPTY;
        pArr->nFixed = 0;
        return XArray_NewTable(pArr, XARRAY_INITIAL_SIZE) != NULL;
    }

    return (XArray_Realloc(pArr) && pArr->nUsed < pArr->nSize) ? 1 : 0;
}

/* The caller has made room */
static int XArray_Append(xarray_t *pArr, xarray_data_t *pData)
{
    pArr->pData[pArr->nUsed++] = pData;
    return (int)pArr->nUsed - 1;
}

int XArray_Add(xarray_t *pArr, xarray_data_t *pNewData)
{
    if (!XArray_CheckSpace(pArr))
    {
        XArray_ClearData(pArr, pNewData);
        return XARRAY_FAILURE;
    }

    return XArray_Append(pArr, pNewData);
}

int XArray_AddDataKey(xarray_t *pArr, void *pData, size_t nSize, uint32_t nKey)
{
    if (pArr == NULL) return XARRAY_FAILURE;
    xarray_data_t *pNewData = XArray_NewData(pArr, pData, nSize, nKey);

    if (pNewData == NULL)
    {
        pArr->eStatus = XARRAY_STATUS_NO_MEMORY;
        return XARRAY_FAILURE;
    }

    return XArray_Add(pArr, pNewData);
}

int XArray_AddData(xarray_t *pArr, void *pData, size_t nSize)
{
    return XArray_AddDataKey(pArr, pData, nSize, 0);
}

int XArray_PushData(xarray_t *pArr, void *pData, size_t nSize)
{
    if (pArr == NULL) return XARRAY_FAILURE;
    xarray_data_t *pNewData = XArray_NewData(pArr, pData, 0, 0);

    if (pNewData == NULL)
    {
        pArr->eStatus = XARRAY_STATUS_NO_MEMORY;
        return XARRAY_FAILURE;
    }

    pNewData->nSize = nSize;
    return XArray_Add(pArr, pNewData);
}

xarray_data_t* XArray_Get(xarray_t *pArr, size_t nIndex)
{
    if (pArr == NULL || nIndex >= pArr->nSize) return NULL;
    return pArr->pData[nIndex];
}

void* XArray_GetData(xarray_t *pArr, size_t nIndex)
{
    if (pArr == NULL || nIndex >= pArr->nSize) return NULL;
    xarray_data_t *pArrData = pArr->pData[nIndex];
    return pArrData ? pArrData->pData : NULL;
}

void* XArray_GetDataOr(xarray_t *pArr, size_t nIndex, void *pRet)
{
    if (pArr == NULL || nIndex >= pArr->nSize) return pRet;
    xarray_data_t *pArrData = pArr->pData[nIndex];
    return pArrData ? pArrData->pData : pRet;
}

size_t XArray_GetSize(xarray_t *pArr, size_t nIndex)
{
    if (pArr == NULL || nIndex >= pArr->nSize) return 0;
    xarray_data_t *pArrData = pArr->pData[nIndex];
    return pArrData ? pArrData->nSize : 0;
}

uint32_t XArray_GetKey(xarray_t *pArr, size_t nIndex)
{
    if (pArr == NULL || nIndex >= pArr->nSize) return 0;
    xarray_data_t *pArrData = pArr->pData[nIndex];
    return pArrData ? pArrData->nKey : 0;
}

/* Closes the gap of a used slot whose item the caller has taken */
static void XArray_Collapse(xarray_t *pArr, size_t nIndex)
{
    size_t nTail = pArr->nUsed - nIndex - 1;
    memmove(&pArr->pData[nIndex], &pArr->pData[nIndex + 1], nTail * sizeof(xarray_data_t*));
    pArr->pData[--pArr->nUsed] = NULL;
    XArray_Realloc(pArr);
}

xarray_data_t* XArray_Remove(xarray_t *pArr, size_t nIndex)
{
    xarray_data_t *pData = XArray_Get(pArr, nIndex);
    if (pData == NULL) return NULL;

    /* An item XArray_Set() stored past the used slots is taken from its slot.
       Collapsing there cleared the last used slot instead, so that item was
       lost, and the one handed back stayed in the array to be freed again. */
    if (nIndex >= pArr->nUsed) pArr->pData[nIndex] = NULL;
    else XArray_Collapse(pArr, nIndex);
    return pData;
}

void XArray_Delete(xarray_t *pArr, size_t nIndex)
{
    if (pArr == NULL || nIndex >= pArr->nUsed) return;
    XArray_ClearData(pArr, pArr->pData[nIndex]);
    XArray_Collapse(pArr, nIndex);
}

xarray_data_t* XArray_Set(xarray_t *pArr, size_t nIndex, xarray_data_t *pNewData)
{
    if (pArr == NULL || nIndex >= pArr->nSize) return NULL;
    xarray_data_t *pOldData = pArr->pData[nIndex];
    pArr->pData[nIndex] = pNewData;
    return pOldData;
}

xarray_data_t* XArray_SetData(xarray_t *pArr, size_t nIndex, void *pData, size_t nSize)
{
    /* Nothing is created for a slot that does not exist, it would have no owner */
    if (pArr == NULL || nIndex >= pArr->nSize) return NULL;

    xarray_data_t *pNewData = XArray_NewData(pArr, pData, nSize, 0);
    if (pNewData == NULL)
    {
        pArr->eStatus = XARRAY_STATUS_NO_MEMORY;
        return NULL;
    }

    xarray_data_t *pOldData = XArray_Set(pArr, nIndex, pNewData);
    return pOldData;
}

/* The caller has made room and checked that the index is a used slot or the one after them */
static xarray_data_t* XArray_InsertAt(xarray_t *pArr, size_t nIndex, xarray_data_t *pData)
{
    if (nIndex == pArr->nUsed)
    {
        XArray_Append(pArr, pData);
        return pData;
    }

    /* The shift moves empty slots too: an insertion in front of one is as good as any other */
    size_t nNextIndex = nIndex + 1;
    size_t nTail = pArr->nUsed - nIndex;

    memmove(&pArr->pData[nNextIndex], &pArr->pData[nIndex], nTail * sizeof(xarray_data_t*));
    pArr->pData[nIndex] = pData;
    pArr->nUsed++;

    return pArr->pData[nNextIndex] != NULL ? pArr->pData[nNextIndex] : pData;
}

xarray_data_t* XArray_Insert(xarray_t *pArr, size_t nIndex, xarray_data_t *pData)
{
    if (pArr == NULL || pData == NULL || nIndex > pArr->nUsed) return NULL;
    if (!XArray_CheckSpace(pArr)) return NULL;
    return XArray_InsertAt(pArr, nIndex, pData);
}

xarray_data_t* XArray_InsertData(xarray_t *pArr, size_t nIndex, void *pData, size_t nSize)
{
    if (pArr == NULL || nIndex > pArr->nUsed || !XArray_CheckSpace(pArr)) return NULL;

    xarray_data_t *pNewData = XArray_NewData(pArr, pData, nSize, 0);
    if (pNewData == NULL)
    {
        pArr->eStatus = XARRAY_STATUS_NO_MEMORY;
        return NULL;
    }

    return XArray_InsertAt(pArr, nIndex, pNewData);
}

void XArray_Swap(xarray_t *pArr, size_t nIndex1, size_t nIndex2)
{
    if (pArr == NULL ||
        nIndex1 >= pArr->nUsed ||
        nIndex2 >= pArr->nUsed) return;

    xarray_data_t *pData1 = pArr->pData[nIndex1];
    pArr->pData[nIndex1] = pArr->pData[nIndex2];
    pArr->pData[nIndex2] = pData1;
}

/* Empty slots sort after every item. SortBy() read through them. */
static int XArray_CompareSize(const void *pData1, const void *pData2, void *pCtx)
{
    (void)pCtx;
    xarray_data_t *pFirst = (xarray_data_t*)pData1;
    xarray_data_t *pSecond = (xarray_data_t*)pData2;
    if (pFirst == NULL || pSecond == NULL) return (pFirst == NULL) - (pSecond == NULL);
    return (pFirst->nSize > pSecond->nSize) - (pFirst->nSize < pSecond->nSize);
}

static int XArray_CompareKey(const void *pData1, const void *pData2, void *pCtx)
{
    (void)pCtx;
    xarray_data_t *pFirst = (xarray_data_t*)pData1;
    xarray_data_t *pSecond = (xarray_data_t*)pData2;
    if (pFirst == NULL || pSecond == NULL) return (pFirst == NULL) - (pSecond == NULL);
    return (pFirst->nKey > pSecond->nKey) - (pFirst->nKey < pSecond->nKey);
}

int XArray_Partitioning(xarray_t *pArr, xarray_comparator_t compare, void *pCtx, int nStart, int nFinish)
{
    XCHECK((pArr != NULL && compare != NULL), nStart);
    xarray_data_t *pPivot = pArr->pData[nStart + (nFinish - nStart) / 2];
    int i = nStart - 1, j = nFinish + 1;

    for (;;)
    {
        /* With a comparator that answers the same way every time, the scans stop
           inside the range by themselves. One that does not, like a subtraction
           that overflows, ran them past the ends of the table. */
        do i++; while (i < nFinish && compare((void*)pArr->pData[i], (void*)pPivot, pCtx) < 0);
        do j--; while (j > nStart && compare((void*)pArr->pData[j], (void*)pPivot, pCtx) > 0);

        /* And both parts are smaller than the range, so the sort ends whatever it is told */
        if (i >= j) return j < nFinish ? j : nFinish - 1;

        xarray_data_t *pData = pArr->pData[i];
        pArr->pData[i] = pArr->pData[j];
        pArr->pData[j] = pData;
    }
}

static void XArray_InsertionSort(xarray_t *pArr, xarray_comparator_t compare, void *pCtx, int nStart, int nFinish)
{
    int i, j;
    for (i = nStart + 1; i <= nFinish; i++)
    {
        xarray_data_t *pData = pArr->pData[i];

        for (j = i - 1; j >= nStart && compare((void*)pArr->pData[j], (void*)pData, pCtx) > 0; j--)
            pArr->pData[j + 1] = pArr->pData[j];

        pArr->pData[j + 1] = pData;
    }
}

static void XArray_SortRange(xarray_t *pArr, xarray_comparator_t compare, void *pCtx, int nStart, int nFinish)
{
    while (nFinish - nStart >= XARRAY_INSERTION_SORT)
    {
        int nPartitioning = XArray_Partitioning(pArr, compare, pCtx, nStart, nFinish);

        /* Recurse into the smaller side and loop over the larger one, so
           the stack stays logarithmic in the number of elements. */
        if (nPartitioning - nStart < nFinish - nPartitioning)
        {
            XArray_SortRange(pArr, compare, pCtx, nStart, nPartitioning);
            nStart = nPartitioning + 1;
        }
        else
        {
            XArray_SortRange(pArr, compare, pCtx, nPartitioning + 1, nFinish);
            nFinish = nPartitioning;
        }
    }

    XArray_InsertionSort(pArr, compare, pCtx, nStart, nFinish);
}

void XArray_QuickSort(xarray_t *pArr, xarray_comparator_t compare, void *pCtx, int nStart, int nFinish)
{
    XCHECK_VOID_NL((pArr != NULL && compare != NULL));
    if (nStart >= nFinish) return;
    XCHECK_VOID_NL((nStart >= 0 && (size_t)nFinish < pArr->nUsed));
    XArray_SortRange(pArr, compare, pCtx, nStart, nFinish);
}

void XArray_Sort(xarray_t *pArr, xarray_comparator_t compare, void *pCtx)
{
    if (pArr == NULL || compare == NULL || !pArr->nUsed) return;
    XArray_QuickSort(pArr, compare, pCtx, 0, (int)pArr->nUsed-1);
}

void XArray_SortBy(xarray_t *pArr, int nSortBy)
{
    if (nSortBy == XARRAY_SORTBY_SIZE)
        XArray_Sort(pArr, XArray_CompareSize, NULL);
    else if (nSortBy == XARRAY_SORTBY_KEY)
        XArray_Sort(pArr, XArray_CompareKey, NULL);
}

void XArray_BubbleSort(xarray_t *pArr, xarray_comparator_t compare, void *pCtx)
{
    XCHECK_VOID_NL((pArr != NULL && compare != NULL));
    xbool_t bSwapped = XTRUE;
    size_t i, nCount = pArr->nUsed;

    /* A pass that swaps nothing has found the rest in order */
    while (nCount > 1 && bSwapped)
    {
        bSwapped = XFALSE;

        for (i = 0; i + 1 < nCount; i++)
        {
            if (compare((void*)pArr->pData[i], (void*)pArr->pData[i + 1], pCtx) > 0)
            {
                xarray_data_t *pData = pArr->pData[i];
                pArr->pData[i] = pArr->pData[i + 1];
                pArr->pData[i + 1] = pData;
                bSwapped = XTRUE;
            }
        }

        nCount--;
    }
}

static int XArray_FindKey(xarray_t *pArr, uint32_t nKey, size_t nCount)
{
    size_t i;
    for (i = 0; i < nCount; i++)
    {
        xarray_data_t *pData = pArr->pData[i];
        if (pData != NULL && nKey == pData->nKey) return (int)i;
    }

    return XARRAY_FAILURE;
}

int XArray_LinearSearch(xarray_t *pArr, uint32_t nKey)
{
    if (pArr == NULL) return XARRAY_FAILURE;
    return XArray_FindKey(pArr, nKey, pArr->nUsed);
}

int XArray_SentinelSearch(xarray_t *pArr, uint32_t nKey)
{
    if (pArr == NULL || !pArr->nUsed) return XARRAY_FAILURE;
    size_t nLast = pArr->nUsed - 1;

    /* A search only reads. Parking the sentinel in the last slot raced every
       other reader, and one that saved another's sentinel as the last item
       put the address of a variable on a finished stack frame in the array. */
    xarray_data_t *pLast = pArr->pData[nLast];
    if (pLast != NULL && pLast->nKey == nKey) return (int)nLast;

    return XArray_FindKey(pArr, nKey, nLast);
}

int XArray_DoubleSearch(xarray_t *pArr, uint32_t nKey)
{
    if (pArr == NULL || !pArr->nUsed) return XARRAY_FAILURE;
    int nFront = 0, nBack = (int)pArr->nUsed - 1;

    while (nFront <= nBack)
    {
        xarray_data_t *pData = pArr->pData[nFront];
        if (pData != NULL && nKey == pData->nKey) return nFront;

        pData = pArr->pData[nBack];
        if (pData != NULL && nKey == pData->nKey) return nBack;

        nFront++;
        nBack--;
    }

    return XARRAY_FAILURE;
}

int XArray_BinarySearch(xarray_t *pArr, uint32_t nKey)
{
    if (pArr == NULL || !pArr->nUsed) return XARRAY_FAILURE;
    int nLeft = 0, nRight = (int)pArr->nUsed - 1;

    xarray_data_t *pData = pArr->pData[nLeft];
    if (pData != NULL && pData->nKey == nKey) return nLeft;

    while (nLeft <= nRight)
    {
        int nMiddle = nLeft + (nRight - nLeft) / 2;
        pData = pArr->pData[nMiddle];

        /* An empty slot has no key to steer by */
        if (pData == NULL) return XARRAY_FAILURE;
        else if (pData->nKey < nKey) nLeft = nMiddle + 1;
        else if (pData->nKey == nKey) return nMiddle;
        else nRight = nMiddle - 1;
    }

    return XARRAY_FAILURE;
}

size_t XArray_Used(xarray_t *pArr)
{
    if (pArr == NULL) return 0;
    return pArr->nUsed;
}

size_t XArray_Size(xarray_t *pArr)
{
    if (pArr == NULL) return 0;
    return pArr->nSize;
}
