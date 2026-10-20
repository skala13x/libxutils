#include "test.h"
#include "array.h"

#ifndef _WIN32
#include <sys/mman.h>
#include <unistd.h>
#endif

static int XTest_lifecycle(void)
{
    xarray_t arr;
    CHECK(XArray_Init(&arr, NULL, 0, 0) == NULL, "array lazy init");
    CHECK(XArray_Used(NULL) == 0 && XArray_Size(NULL) == 0, "array NULL sizes");
    CHECK(XArray_AddData(NULL, NULL, 0) == XARRAY_FAILURE, "array rejects NULL add");

    int values[] = {50, 10, 40, 20, 30};
    uint32_t keys[] = {5, 1, 4, 2, 3};
    for (size_t i = 0; i < 5; i++)
        CHECK(XArray_AddDataKey(&arr, &values[i], sizeof(values[i]), keys[i]) == (int)i, "array keyed add");
    CHECK(arr.nUsed == 5 && arr.nSize >= 5, "array lazy growth");
    CHECK(*(int*)XArray_GetData(&arr, 0) == 50, "array copied data");
    values[0] = -1;
    CHECK(*(int*)XArray_GetData(&arr, 0) == 50, "array owns copy");
    CHECK(XArray_GetData(&arr, 100) == NULL && XArray_GetSize(&arr, 100) == 0, "array out of range");

    XArray_SortBy(&arr, XARRAY_SORTBY_KEY);
    for (size_t i = 0; i < 5; i++) CHECK(XArray_GetKey(&arr, i) == i + 1, "array key sort");
    CHECK(XArray_LinearSearch(&arr, 4) == 3, "array linear search");
    CHECK(XArray_SentinelSearch(&arr, 4) == 3, "array sentinel search");
    CHECK(XArray_DoubleSearch(&arr, 4) == 3, "array double search");
    CHECK(XArray_BinarySearch(&arr, 4) == 3, "array binary search");
    CHECK(XArray_BinarySearch(&arr, 99) == XARRAY_FAILURE, "array search missing");

    xarray_data_t *pRemoved = XArray_Remove(&arr, 1);
    CHECK(pRemoved != NULL && pRemoved->nKey == 2 && arr.nUsed == 4, "array remove");
    XArray_FreeData(pRemoved);

    int inserted = 77;
    CHECK(XArray_InsertData(&arr, 1, &inserted, sizeof(inserted)) != NULL, "array insert");
    CHECK(*(int*)XArray_GetData(&arr, 1) == inserted && arr.nUsed == 5, "array insert position");

    int replacement = 88;
    xarray_data_t *pOld = XArray_SetData(&arr, 0, &replacement, sizeof(replacement));
    CHECK(pOld != NULL && *(int*)XArray_GetData(&arr, 0) == replacement, "array set");
    XArray_FreeData(pOld);

    XArray_Swap(&arr, 0, arr.nUsed - 1);
    CHECK(*(int*)XArray_GetData(&arr, arr.nUsed - 1) == replacement, "array swap");
    XArray_Delete(&arr, arr.nUsed - 1);
    CHECK(arr.nUsed == 4, "array delete");
    XArray_Clear(&arr);
    CHECK(arr.nUsed == 0, "array clear");
    XArray_Destroy(&arr);
    CHECK(arr.pData == NULL && arr.nSize == 0, "array destroy");

    xarray_t fixed;
    CHECK(XArray_Init(&fixed, NULL, 1, 1) != NULL, "fixed array init");
    CHECK(XArray_AddData(&fixed, &replacement, sizeof(replacement)) == 0, "fixed array first add");
    CHECK(XArray_AddData(&fixed, &replacement, sizeof(replacement)) == XARRAY_FAILURE, "fixed array rejects overflow");
    XArray_Destroy(&fixed);
    return 0;
}

static int XTest_reference_model(void)
{
    xarray_t array;
    int reference[128];
    size_t nUsed = 0;
    uint32_t nSeed = 0xd335;
    XArray_Init(&array, NULL, 0, 0);
    for (int nStep = 0; nStep < 5000; nStep++)
    {
        nSeed = nSeed * 1664525u + 1013904223u;
        size_t nIndex = nUsed ? (nSeed >> 8) % nUsed : 0;
        if (!nUsed || ((nSeed & 1) && nUsed < 128))
        {
            CHECK(XArray_InsertData(&array, nIndex, &nStep, sizeof(nStep)) != NULL, "Insert at a modeled position");
            memmove(reference + nIndex + 1, reference + nIndex, (nUsed - nIndex) * sizeof(int));
            reference[nIndex] = nStep;
            nUsed++;
        }
        else
        {
            xarray_data_t *pRemoved = XArray_Remove(&array, nIndex);
            CHECK(pRemoved && *(int*)pRemoved->pData == reference[nIndex], "Removal returns the modeled value");
            XArray_FreeData(pRemoved);
            nUsed--;
            memmove(reference + nIndex, reference + nIndex + 1, (nUsed - nIndex) * sizeof(int));
        }
        CHECK(array.nUsed == nUsed, "Array count agrees after mutation");
        for (size_t i = 0; i < nUsed; i++)
            CHECK(*(int*)XArray_GetData(&array, i) == reference[i], "Array order agrees with model");
    }
    XArray_Destroy(&array);
    return 0;
}

static int XTest_search_boundaries(void)
{
    xarray_t array;
    XArray_Init(&array, NULL, 0, 0);
    CHECK(XArray_BinarySearch(&array, 0) == XARRAY_FAILURE, "Searching an empty array terminates");
    XArray_Delete(&array, 0);
    CHECK(array.nUsed == 0, "Deleting from an empty array cannot underflow its count");
    for (uint32_t i = 0; i < 128; i++)
    {
        uint32_t nKey = i * 2;
        CHECK(XArray_AddDataKey(&array, &nKey, sizeof(nKey), nKey) >= 0, "Insert sorted even keys");
    }
    for (uint32_t i = 0; i < 256; i++)
    {
        int nExpected = i % 2 ? XARRAY_FAILURE : (int)(i / 2);
        CHECK(XArray_BinarySearch(&array, i) == nExpected, "Binary search finds endpoints and rejects gaps");
        CHECK(XArray_LinearSearch(&array, i) == nExpected, "Linear search agrees with independent even-key model");
        CHECK(XArray_SentinelSearch(&array, i) == nExpected, "Sentinel search restores the final entry");
    }
    CHECK(XArray_GetKey(&array, 127) == 254, "Sentinel search must not corrupt the last key");
    XArray_Delete(&array, SIZE_MAX);
    CHECK(array.nUsed == 128, "An invalid delete leaves every live element intact");
    CHECK(XArray_InsertData(&array, SIZE_MAX, &array, sizeof(array)) == NULL, "Reject invalid insertion before allocation");
    XArray_Destroy(&array);
    return 0;
}


/* The comparator receives the entries themselves, not pointers to them. */
static int XTest_CompareKey(const void *pFirst, const void *pSecond, void *pCtx)
{
    (void)pCtx;
    const xarray_data_t *pLeft = (const xarray_data_t*)pFirst;
    const xarray_data_t *pRight = (const xarray_data_t*)pSecond;
    return (int)pLeft->nKey - (int)pRight->nKey;
}

static int XTest_CompareKeyDesc(const void *pFirst, const void *pSecond, void *pCtx)
{
    return -XTest_CompareKey(pFirst, pSecond, pCtx);
}

/* Fills an array with the given keys in the given order. */
static int array_fill(xarray_t *pArr, const uint32_t *pKeys, size_t nCount)
{
    for (size_t i = 0; i < nCount; i++)
        if (XArray_AddDataKey(pArr, (void*)"v", 0, pKeys[i]) < 0) return XSTDERR;
    return XSTDOK;
}

static int XTest_sorting(void)
{
    const uint32_t keys[] = {5, 1, 4, 2, 3};

    /* Each sort entry point puts the same keys in the same order. */
    xarray_t arr;
    XArray_Init(&arr, NULL, 0, 0);
    CHECK(XArray_Used(&arr) == 0, "A zero sized array starts empty and allocates on demand");
    CHECK(array_fill(&arr, keys, 5) == XSTDOK, "The array is filled");

    XArray_Sort(&arr, XTest_CompareKey, NULL);
    for (size_t i = 0; i < 5; i++)
        CHECK(XArray_GetKey(&arr, i) == (uint32_t)(i + 1), "Sorting orders the entries by key");
    XArray_Destroy(&arr);

    XArray_Init(&arr, NULL, 0, 0);
    CHECK(XArray_Used(&arr) == 0, "A zero sized array starts empty and allocates on demand");
    CHECK(array_fill(&arr, keys, 5) == XSTDOK, "The array is filled");
    XArray_BubbleSort(&arr, XTest_CompareKey, NULL);
    for (size_t i = 0; i < 5; i++)
        CHECK(XArray_GetKey(&arr, i) == (uint32_t)(i + 1), "The bubble sort orders the entries by key");
    XArray_Destroy(&arr);

    XArray_Init(&arr, NULL, 0, 0);
    CHECK(XArray_Used(&arr) == 0, "A zero sized array starts empty and allocates on demand");
    CHECK(array_fill(&arr, keys, 5) == XSTDOK, "The array is filled");
    XArray_QuickSort(&arr, XTest_CompareKey, NULL, 0, (int)XArray_Used(&arr) - 1);
    for (size_t i = 0; i < 5; i++)
        CHECK(XArray_GetKey(&arr, i) == (uint32_t)(i + 1), "The quick sort orders the entries by key");

    /* A reversing comparator reverses the order. */
    XArray_Sort(&arr, XTest_CompareKeyDesc, NULL);
    for (size_t i = 0; i < 5; i++)
        CHECK(XArray_GetKey(&arr, i) == (uint32_t)(5 - i), "A reversing comparator reverses the order");
    XArray_Destroy(&arr);

    /* The built-in key sort agrees with the comparator one. */
    XArray_Init(&arr, NULL, 0, 0);
    CHECK(XArray_Used(&arr) == 0, "A zero sized array starts empty and allocates on demand");
    CHECK(array_fill(&arr, keys, 5) == XSTDOK, "The array is filled");
    XArray_SortBy(&arr, XARRAY_SORTBY_KEY);
    for (size_t i = 0; i < 5; i++)
        CHECK(XArray_GetKey(&arr, i) == (uint32_t)(i + 1), "The built-in key sort orders by key");

    /* Sorting an already sorted array leaves it sorted. */
    XArray_SortBy(&arr, XARRAY_SORTBY_KEY);
    for (size_t i = 0; i < 5; i++)
        CHECK(XArray_GetKey(&arr, i) == (uint32_t)(i + 1), "Re-sorting a sorted array is stable");
    XArray_Destroy(&arr);

    /* Degenerate sizes must not run off either end. */
    for (size_t nCount = 0; nCount <= 2; nCount++)
    {
        XArray_Init(&arr, NULL, 0, 0);
    CHECK(XArray_Used(&arr) == 0, "A zero sized array starts empty and allocates on demand");
        CHECK(array_fill(&arr, keys, nCount) == XSTDOK, "The short array is filled");
        XArray_Sort(&arr, XTest_CompareKey, NULL);
        XArray_SortBy(&arr, XARRAY_SORTBY_KEY);
        CHECK(XArray_Used(&arr) == nCount, "Sorting a short array keeps every entry");
        XArray_Destroy(&arr);
    }

    /* Duplicate keys all survive the sort. */
    const uint32_t dupes[] = {2, 1, 2, 1, 2};
    XArray_Init(&arr, NULL, 0, 0);
    CHECK(XArray_Used(&arr) == 0, "A zero sized array starts empty and allocates on demand");
    CHECK(array_fill(&arr, dupes, 5) == XSTDOK, "The duplicate keys are filled");
    XArray_SortBy(&arr, XARRAY_SORTBY_KEY);
    CHECK(XArray_Used(&arr) == 5, "Sorting keeps every duplicate");
    for (size_t i = 1; i < 5; i++)
        CHECK(XArray_GetKey(&arr, i) >= XArray_GetKey(&arr, i - 1), "Duplicates come out in order");
    XArray_Destroy(&arr);
    return 0;
}

static int XTest_mutation(void)
{
    /* Inserting shifts the tail; setting replaces in place. */
    xarray_t arr;
    XArray_Init(&arr, NULL, 0, 0);
    CHECK(XArray_Used(&arr) == 0, "A zero sized array starts empty and allocates on demand");
    CHECK(XArray_AddData(&arr, (void*)"a", 2) >= 0, "The first entry is added");
    CHECK(XArray_AddData(&arr, (void*)"c", 2) >= 0, "The second entry is added");

    CHECK(XArray_InsertData(&arr, 1, (void*)"b", 2) != NULL, "An entry is inserted in the middle");
    CHECK(XArray_Used(&arr) == 3, "The insert lengthened the array");
    CHECK(strcmp((char*)XArray_GetData(&arr, 0), "a") == 0, "The entry before the insert stayed put");
    CHECK(strcmp((char*)XArray_GetData(&arr, 1), "b") == 0, "The inserted entry is at its index");
    CHECK(strcmp((char*)XArray_GetData(&arr, 2), "c") == 0, "The entry after the insert shifted along");

    /* Replacing hands the displaced entry back, and the caller owns it. */
    xarray_data_t *pDisplaced = XArray_SetData(&arr, 0, (void*)"A", 2);
    CHECK(pDisplaced != NULL, "Replacing hands the displaced entry back");
    CHECK(strcmp((char*)pDisplaced->pData, "a") == 0, "The displaced entry is the one that was there");
    XArray_FreeData(pDisplaced);

    CHECK(strcmp((char*)XArray_GetData(&arr, 0), "A") == 0, "The replacement is stored");
    CHECK(XArray_Used(&arr) == 3, "Replacing does not change the length");

    /* Swapping exchanges two entries and leaves the rest alone. */
    XArray_Swap(&arr, 0, 2);
    CHECK(strcmp((char*)XArray_GetData(&arr, 0), "c") == 0, "The swap moved the far entry to the front");
    CHECK(strcmp((char*)XArray_GetData(&arr, 2), "A") == 0, "The swap moved the front entry to the far end");
    CHECK(strcmp((char*)XArray_GetData(&arr, 1), "b") == 0, "The swap left the middle alone");

    /* A swap with an out of range index changes nothing. */
    XArray_Swap(&arr, 0, 99);
    CHECK(strcmp((char*)XArray_GetData(&arr, 0), "c") == 0, "An out of range swap changes nothing");

    /* Removing hands the entry back; deleting releases it. */
    xarray_data_t *pRemoved = XArray_Remove(&arr, 0);
    CHECK(pRemoved != NULL, "Removing hands the entry back to the caller");
    CHECK(XArray_Used(&arr) == 2, "Removing shortened the array");
    XArray_FreeData(pRemoved);

    XArray_Delete(&arr, 0);
    CHECK(XArray_Used(&arr) == 1, "Deleting shortened the array");
    CHECK(strcmp((char*)XArray_GetData(&arr, 0), "A") == 0, "The surviving entry is the expected one");

    /* Out of range access is reported rather than guessed at. */
    CHECK(XArray_GetData(&arr, 99) == NULL, "An entry past the end is not there");
    CHECK(XArray_Get(&arr, 99) == NULL, "An entry past the end has no record");
    CHECK(XArray_Remove(&arr, 99) == NULL, "An entry past the end cannot be removed");
    CHECK(XArray_Contains(&arr, 99) == 0, "An index past the end is not contained");
    CHECK(XArray_Contains(&arr, 0) == 1, "An index inside the array is contained");

    /* The defaulting accessor returns the caller's fallback. */
    CHECK(strcmp((char*)XArray_GetDataOr(&arr, 99, (void*)"fallback"), "fallback") == 0,
        "An out of range read returns the caller's fallback");
    CHECK(strcmp((char*)XArray_GetDataOr(&arr, 0, (void*)"fallback"), "A") == 0,
        "An in range read returns the stored entry");

    XArray_Destroy(&arr);
    return 0;
}

static int XTest_fixed_capacity(void)
{
    /* A fixed array refuses to grow past its capacity rather than
     * reallocating out from under the caller. */
    xarray_t arr;
    CHECK(XArray_Init(&arr, NULL, 3, 1) != NULL, "A sized array allocates its storage up front");
    CHECK(XArray_Size(&arr) == 3, "The fixed array has the requested capacity");

    for (int i = 0; i < 3; i++)
        CHECK(XArray_AddData(&arr, (void*)"v", 0) == i, "Every entry up to the capacity is accepted");
    CHECK(XArray_Used(&arr) == 3, "The fixed array is full");

    CHECK(XArray_AddData(&arr, (void*)"v", 0) < 0, "An entry past the capacity is refused");
    CHECK(XArray_Used(&arr) == 3, "The refused entry did not displace anything");
    CHECK(XArray_Size(&arr) == 3, "The refused entry did not grow the array");

    /* Freeing a slot makes room again. */
    XArray_Delete(&arr, 0);
    CHECK(XArray_Used(&arr) == 2, "Deleting freed a slot");
    CHECK(XArray_AddData(&arr, (void*)"v", 0) >= 0, "The freed slot accepts a new entry");

    XArray_Destroy(&arr);

    /* A growing array reallocates instead. */
    CHECK(XArray_Init(&arr, NULL, 2, 0) != NULL, "The growing array allocates its initial storage");
    for (int i = 0; i < 64; i++)
        CHECK(XArray_AddData(&arr, (void*)"v", 0) >= 0, "A growing array keeps accepting entries");
    CHECK(XArray_Used(&arr) == 64, "Every entry was stored");
    CHECK(XArray_Size(&arr) >= 64, "The capacity grew to hold them");
    XArray_Destroy(&arr);
    return 0;
}

static int XTest_search_agreement(void)
{
    /* Every search strategy has to agree on a sorted array, since they are
     * interchangeable from the caller's point of view. */
    xarray_t arr;
    XArray_Init(&arr, NULL, 0, 0);
    CHECK(XArray_Used(&arr) == 0, "A zero sized array starts empty and allocates on demand");

    for (uint32_t nKey = 0; nKey < 32; nKey++)
        CHECK(XArray_AddDataKey(&arr, (void*)"v", 0, nKey * 2) >= 0, "Every key is added");
    XArray_SortBy(&arr, XARRAY_SORTBY_KEY);

    for (uint32_t nKey = 0; nKey < 32; nKey++)
    {
        int nExpected = (int)nKey;
        CHECK(XArray_LinearSearch(&arr, nKey * 2) == nExpected, "The linear search finds every key");
        CHECK(XArray_BinarySearch(&arr, nKey * 2) == nExpected, "The binary search finds every key");
        CHECK(XArray_SentinelSearch(&arr, nKey * 2) == nExpected, "The sentinel search finds every key");
        CHECK(XArray_DoubleSearch(&arr, nKey * 2) == nExpected, "The double search finds every key");
    }

    /* A key that is not there is reported missing by every strategy. */
    const uint32_t absent[] = {1, 3, 63, 64, 1000};
    for (size_t i = 0; i < sizeof(absent) / sizeof(*absent); i++)
    {
        CHECK(XArray_LinearSearch(&arr, absent[i]) < 0, "The linear search reports a missing key");
        CHECK(XArray_BinarySearch(&arr, absent[i]) < 0, "The binary search reports a missing key");
        CHECK(XArray_SentinelSearch(&arr, absent[i]) < 0, "The sentinel search reports a missing key");
        CHECK(XArray_DoubleSearch(&arr, absent[i]) < 0, "The double search reports a missing key");
    }

    XArray_Destroy(&arr);

    /* An empty array finds nothing without reading past its storage. */
    XArray_Init(&arr, NULL, 0, 0);
    CHECK(XArray_Used(&arr) == 0, "The empty array starts empty");
    CHECK(XArray_LinearSearch(&arr, 1) < 0, "The linear search on an empty array finds nothing");
    CHECK(XArray_BinarySearch(&arr, 1) < 0, "The binary search on an empty array finds nothing");
    CHECK(XArray_SentinelSearch(&arr, 1) < 0, "The sentinel search on an empty array finds nothing");
    CHECK(XArray_DoubleSearch(&arr, 1) < 0, "The double search on an empty array finds nothing");
    XArray_Destroy(&arr);
    return 0;
}

static int XTest_heap_array(void)
{
    /* The heap array owns itself and clears the caller pointer. */
    xarray_t *pArr = XArray_New(NULL, 8, 0);
    CHECK(pArr != NULL, "A heap array is allocated");
    CHECK(XArray_AddData(pArr, (void*)"x", 0) >= 0, "The heap array accepts entries");
    CHECK(XArray_Used(pArr) == 1, "The heap array counted the entry");

    XArray_Free(&pArr);
    CHECK(pArr == NULL, "Freeing clears the caller pointer");
    XArray_Free(&pArr);
    XArray_Free(NULL);

    /* A pooled array allocates its entries from its own pool. */
    pArr = XArray_NewPool(4096, 8, 0);
    CHECK(pArr != NULL, "A pooled array is allocated");
    for (int i = 0; i < 32; i++)
        CHECK(XArray_AddData(pArr, (void*)"pooled", 0) >= 0, "The pooled array accepts entries");
    CHECK(XArray_Used(pArr) == 32, "Every pooled entry was stored");
    XArray_Free(&pArr);
    CHECK(pArr == NULL, "Freeing the pooled array clears the caller pointer");

    /* Clearing empties an array without releasing it. */
    xarray_t arr;
    XArray_Init(&arr, NULL, 0, 0);
    CHECK(XArray_Used(&arr) == 0, "A zero sized array starts empty and allocates on demand");
    for (int i = 0; i < 8; i++) CHECK(XArray_AddData(&arr, (void*)"x", 0) >= 0, "Entries are added");

    XArray_Clear(&arr);
    CHECK(XArray_Used(&arr) == 0, "Clearing empties the array");
    CHECK(XArray_AddData(&arr, (void*)"x", 0) >= 0, "A cleared array is still usable");

    XArray_Destroy(&arr);
    XArray_Destroy(&arr);
    CHECK(XArray_Used(&arr) == 0, "A repeatedly destroyed array stays empty");
    return 0;
}


static int XTest_release_guards(void)
{
    /* Releasing is the one thing an error path always does, and an error
     * path is exactly where the array it holds may be the null a
     * constructor handed back. Both calls have to answer on that rather
     * than fault on the very line that was meant to clean up. */
    XArray_Clear(NULL);
    XArray_Destroy(NULL);
    XArray_Free(NULL);

    CHECK(XArray_Used(NULL) == 0, "A missing array holds nothing");
    CHECK(XArray_Size(NULL) == 0, "A missing array has no size");
    CHECK(XArray_Get(NULL, 0) == NULL, "A missing array yields no entry");
    CHECK(XArray_GetData(NULL, 0) == NULL, "A missing array yields no data");
    CHECK(XArray_GetSize(NULL, 0) == 0, "A missing array yields no entry size");

    /* A real array survives being cleared twice and destroyed after. */
    xarray_t *pArray = XArray_New(NULL, 4, XFALSE);
    CHECK(pArray != NULL, "An array is created");

    CHECK(XArray_AddData(pArray, "one", 4) >= 0, "An entry is added");
    CHECK(XArray_Used(pArray) == 1, "The entry is counted");

    XArray_Clear(pArray);
    CHECK(XArray_Used(pArray) == 0, "Clearing empties it");

    XArray_Clear(pArray);
    CHECK(XArray_Used(pArray) == 0, "Clearing an empty array is a no-op");

    XArray_Destroy(pArray);

    /* A pooled array of no declared size still comes back usable or null,
     * never as a handle that cannot be written to. */
    xarray_t *pPooled = XArray_NewPool(XSTDNON, XSTDNON, XFALSE);
    if (pPooled != NULL)
    {
        CHECK(XArray_Used(pPooled) == 0, "A pooled array starts empty");
        XArray_Destroy(pPooled);
    }

    return 0;
}

static int XTest_FillCheck(xarray_t *pArr, int nRound)
{
    for (int i = 0; i < 200; i++)
    {
        char sItem[32];
        int nLen = snprintf(sItem, sizeof(sItem), "round-%d-item-%d", nRound, i);
        CHECK(XArray_AddData(pArr, sItem, (size_t)nLen + 1) >= 0, "Add an item after clearing");
    }

    for (int i = 0; i < 200; i++)
    {
        char sItem[32];
        snprintf(sItem, sizeof(sItem), "round-%d-item-%d", nRound, i);
        const char *pItem = (const char*)XArray_GetData(pArr, (size_t)i);
        CHECK(pItem != NULL && strcmp(pItem, sItem) == 0, "Every item reads back intact after the array was cleared");
    }

    CHECK(XArray_Used(pArr) == 200, "The array holds exactly what was added since the last clear");
    return 0;
}

static int XTest_pool_clear_reuse(void)
{
    /* Clearing a pool backed array reset the pool under the array's own table
       (and, for XArray_NewPool(), under the array itself). Refilling it then
       handed out that same memory, and the array came back corrupted. */
    xarray_t *pHeapArr = XArray_NewPool(0, 0, XFALSE);
    CHECK(pHeapArr != NULL, "Create an array that lives in its own pool");

    xarray_t embedded;
    CHECK(XArray_InitPool(&embedded, 0, 4, XFALSE) != NULL, "Create an array that only keeps its items in a pool");

    for (int nRound = 0; nRound < 4; nRound++)
    {
        CHECK(XTest_FillCheck(pHeapArr, nRound) == 0, "Refill the self-pooled array");
        CHECK(XTest_FillCheck(&embedded, nRound) == 0, "Refill the pool backed array");
        XArray_Clear(pHeapArr);
        XArray_Clear(&embedded);
        CHECK(XArray_Used(pHeapArr) == 0 && XArray_Used(&embedded) == 0, "Clearing empties both arrays");
    }

    XArray_Destroy(pHeapArr);
    XArray_Destroy(&embedded);
    return 0;
}

static int XTest_sort_scaling(void)
{
    /* The first element used to be the pivot, so sorted input took quadratic
       time and one recursion level per element. */
    enum
    {
        SORT_ITEMS = 300000
    };

    for (int nMode = 0; nMode < 3; nMode++)
    {
        xarray_t arr;
        XArray_Init(&arr, NULL, SORT_ITEMS, XFALSE);

        for (uint32_t i = 0; i < SORT_ITEMS; i++)
        {
            uint32_t nKey = nMode == 0 ? i : nMode == 1 ? SORT_ITEMS - i : 7;
            CHECK(XArray_AddDataKey(&arr, NULL, 0, nKey) >= 0, "Add a keyed item");
        }

        XArray_Sort(&arr, XTest_CompareKey, NULL);
        for (size_t i = 1; i < SORT_ITEMS; i++)
            CHECK(XArray_GetKey(&arr, i - 1) <= XArray_GetKey(&arr, i), "Sorted, reversed and equal input all come out ordered");

        XArray_Destroy(&arr);
    }

    return 0;
}

static int XTest_sort_unsigned_keys(void)
{
    /* Keys are unsigned. Sorting them by int subtraction put the ones above
       INT_MAX first, so a binary search over the sorted array missed them. */
    const uint32_t keys[] = { 0xFFFFFFF0u, 5, 0x80000000u, 0x7FFFFFFFu, 0, 1234567 };
    const size_t nCount = sizeof(keys) / sizeof(keys[0]);

    xarray_t arr;
    XArray_Init(&arr, NULL, 0, XFALSE);
    for (size_t i = 0; i < nCount; i++)
        CHECK(XArray_AddDataKey(&arr, NULL, 0, keys[i]) >= 0, "Add a key from the whole unsigned range");

    XArray_SortBy(&arr, XARRAY_SORTBY_KEY);
    for (size_t i = 1; i < nCount; i++)
        CHECK(XArray_GetKey(&arr, i - 1) <= XArray_GetKey(&arr, i), "Keys sort in unsigned order");

    for (size_t i = 0; i < nCount; i++)
    {
        int nIndex = XArray_BinarySearch(&arr, keys[i]);
        CHECK(nIndex >= 0 && XArray_GetKey(&arr, (size_t)nIndex) == keys[i], "Every sorted key is found by binary search");
    }

    XArray_Destroy(&arr);
    return 0;
}

static int XTest_insert_holes(void)
{
    /* An insertion in front of an empty slot is an insertion like any other:
       the item is stored and the call succeeds, so XArray_InsertData never
       frees an item the array still holds. */
    xarray_t *pArr = XArray_New(NULL, 4, XFALSE);
    CHECK(pArr != NULL, "Create an array");

    int values[] = { 10, 20, 30 };
    for (size_t i = 0; i < 3; i++)
        CHECK(XArray_AddData(pArr, &values[i], sizeof(values[i])) == (int)i, "Fill the array");

    xarray_data_t *pHole = XArray_Set(pArr, 1, NULL);
    CHECK(pHole != NULL && XArray_GetData(pArr, 1) == NULL, "Punch a hole in the middle");
    XArray_FreeData(pHole);

    int inserted = 15;
    xarray_data_t *pResult = XArray_InsertData(pArr, 1, &inserted, sizeof(inserted));
    CHECK(pResult != NULL, "An insertion in front of an empty slot succeeds");
    CHECK(pArr->nUsed == 4, "It adds one item");
    CHECK(*(int*)XArray_GetData(pArr, 0) == 10 && *(int*)XArray_GetData(pArr, 1) == 15, "It lands where it was asked to");
    CHECK(XArray_GetData(pArr, 2) == NULL && *(int*)XArray_GetData(pArr, 3) == 30, "The hole and the rest move up");

    /* An insertion that finds an item in place reports the item it moved */
    int second = 5;
    xarray_data_t *pMoved = XArray_InsertData(pArr, 0, &second, sizeof(second));
    CHECK(pMoved != NULL && *(int*)pMoved->pData == 10, "An insertion over an item reports the item it moved");
    CHECK(pArr->nUsed == 5 && *(int*)XArray_GetData(pArr, 0) == 5, "And the new item is in front");

    /* At the end, and at an index past the end */
    int last = 99;
    CHECK(XArray_InsertData(pArr, pArr->nUsed, &last, sizeof(last)) != NULL, "An insertion at the end appends");
    CHECK(*(int*)XArray_GetData(pArr, pArr->nUsed - 1) == 99, "The appended item is last");
    CHECK(XArray_InsertData(pArr, pArr->nUsed + 1, &last, sizeof(last)) == NULL, "An insertion past the end is refused");

    /* A hole that is the last used slot */
    pHole = XArray_Set(pArr, pArr->nUsed - 1, NULL);
    CHECK(pHole != NULL && XArray_Get(pArr, pArr->nUsed - 1) == NULL, "Punch a hole at the end");
    XArray_FreeData(pHole);

    size_t nUsed = pArr->nUsed;
    for (int i = 0; i < 40; i++)
        CHECK(XArray_InsertData(pArr, nUsed - 1, &inserted, sizeof(inserted)) != NULL, "Insert repeatedly before a hole");
    CHECK(pArr->nUsed == nUsed + 40 && XArray_GetData(pArr, pArr->nUsed - 1) == NULL, "The hole stays last as the array grows");

    XArray_Destroy(pArr);
    return 0;
}

static int XTest_null_and_bounds(void)
{
    /* Every accessor copes with a missing array, and a slot that does not
       exist is never written or allocated for. */
    int value = 7;
    CHECK(XArray_GetDataOr(NULL, 0, &value) == &value, "A missing array gives the fallback");
    CHECK(XArray_GetKey(NULL, 0) == 0, "A missing array has no keys");
    CHECK(XArray_Set(NULL, 0, NULL) == NULL, "A missing array has nothing to replace");
    CHECK(XArray_SetData(NULL, 0, &value, sizeof(value)) == NULL, "A missing array cannot be set");
    CHECK(XArray_PushData(NULL, &value, sizeof(value)) == XARRAY_FAILURE, "A missing array cannot be pushed to");
    CHECK(XArray_AddDataKey(NULL, &value, sizeof(value), 1) == XARRAY_FAILURE, "A missing array cannot be added to");
    XArray_Swap(NULL, 0, 1);

    xarray_t *pArr = XArray_New(NULL, 2, XFALSE);
    CHECK(pArr != NULL, "Create an array");
    CHECK(XArray_AddData(pArr, &value, sizeof(value)) == 0, "Add one item");

    CHECK(XArray_SetData(pArr, pArr->nSize, &value, sizeof(value)) == NULL, "A slot past the capacity is not set");
    CHECK(XArray_SetData(pArr, 1000, &value, sizeof(value)) == NULL, "Nor one far past it");
    CHECK(pArr->nUsed == 1 && XArray_GetData(pArr, 1) == NULL, "Nothing was stored");

    int other = 8;
    xarray_data_t *pOld = XArray_SetData(pArr, 1, &other, sizeof(other));
    CHECK(pOld == NULL && *(int*)XArray_GetData(pArr, 1) == 8, "An unused slot within the capacity is set");
    CHECK(pArr->nUsed == 1, "Setting does not change what is counted as used");

    CHECK(XArray_GetDataOr(pArr, 100, &value) == &value, "An index past the capacity gives the fallback");
    CHECK(XArray_GetKey(pArr, 100) == 0, "An index past the capacity has no key");

    XArray_Swap(pArr, 0, 5);
    CHECK(*(int*)XArray_GetData(pArr, 0) == 7, "A swap with a slot past the used ones changes nothing");

    xarray_data_t *pSet = XArray_Set(pArr, 1, NULL);
    XArray_FreeData(pSet);
    XArray_Destroy(pArr);
    return 0;
}

static int XTest_search_holes(void)
{
    /* The searches skip empty slots instead of reading through them */
    xarray_t *pArr = XArray_New(NULL, 8, XFALSE);
    CHECK(pArr != NULL, "Create an array");

    for (uint32_t i = 0; i < 6; i++)
        CHECK(XArray_AddDataKey(pArr, &i, sizeof(i), i * 10) == (int)i, "Add keyed items");

    xarray_data_t *pHole = XArray_Set(pArr, 2, NULL);
    XArray_FreeData(pHole);

    CHECK(XArray_LinearSearch(pArr, 30) == 3, "A linear search passes an empty slot");
    CHECK(XArray_LinearSearch(pArr, 20) == XARRAY_FAILURE, "The key of the removed item is gone");
    CHECK(XArray_DoubleSearch(pArr, 30) == 3, "A double ended search passes an empty slot");
    CHECK(XArray_DoubleSearch(pArr, 20) == XARRAY_FAILURE, "And does not find the removed key");
    CHECK(XArray_SentinelSearch(pArr, 50) == 5, "A sentinel search still finds the last item");
    CHECK(XArray_SentinelSearch(pArr, 40) == 4, "And one before it, past the empty slot");
    CHECK(XArray_SentinelSearch(pArr, 20) == XARRAY_FAILURE, "And does not find the removed key");
    CHECK(XArray_BinarySearch(pArr, 0) == 0, "A binary search finds the first item");

    int nFound = XArray_BinarySearch(pArr, 20);
    CHECK(nFound == XARRAY_FAILURE, "A binary search stopped by an empty slot reports failure");

    /* An empty last slot and an empty first slot */
    pHole = XArray_Set(pArr, 5, NULL);
    XArray_FreeData(pHole);
    CHECK(XArray_SentinelSearch(pArr, 50) == XARRAY_FAILURE, "A sentinel search copes with an empty last slot");
    CHECK(XArray_SentinelSearch(pArr, 40) == 4, "And still finds what is before it");
    CHECK(XArray_GetData(pArr, 5) == NULL, "The empty last slot is left in place");

    pHole = XArray_Set(pArr, 0, NULL);
    XArray_FreeData(pHole);
    CHECK(XArray_BinarySearch(pArr, 0) == XARRAY_FAILURE, "A binary search copes with an empty first slot");
    CHECK(XArray_LinearSearch(pArr, 10) == 1, "A linear search copes with it too");

    XArray_Destroy(pArr);
    return 0;
}

static int g_nArrayCleared;

/* Releases what an item holds, the array frees the item itself */
static void XTest_ReleaseItem(xarray_data_t *pData)
{
    g_nArrayCleared++;
    free(pData->pData);
}

static int XTest_set_past_used(void)
{
    /* An item set past the used slots is not counted as used, as always, but
       it is no longer lost: a shrink cut it off, a removal cleared the last
       used slot in its place, and either way nothing released it. */
    g_nArrayCleared = 0;
    xarray_t *pArr = XArray_New(NULL, 16, XFALSE);
    CHECK(pArr != NULL, "Create an array");
    pArr->clearCb = XTest_ReleaseItem;

    for (int i = 0; i < 3; i++) CHECK(XArray_AddData(pArr, strdup("used"), 0) == i, "Add an item");
    CHECK(XArray_SetData(pArr, 12, strdup("set"), 0) == NULL && XArray_Used(pArr) == 3, "Set an item past them");
    CHECK(XArray_SetData(pArr, 5, strdup("near"), 0) == NULL && XArray_Used(pArr) == 3, "And one closer to them");

    /* Deleting leaves too few used slots for the table, but it does not shrink past the items */
    XArray_Delete(pArr, 0);
    XArray_Delete(pArr, 0);
    CHECK(XArray_Used(pArr) == 1 && XArray_Size(pArr) == 16, "The table keeps the slot of the far item");
    CHECK(!strcmp((char*)XArray_GetData(pArr, 12), "set") && !strcmp((char*)XArray_GetData(pArr, 5), "near"),
        "And both items are where they were set");

    /* Removing the far one takes it from its slot, and the table can shrink again */
    xarray_data_t *pFar = XArray_Remove(pArr, 12);
    CHECK(pFar != NULL && !strcmp((char*)pFar->pData, "set") && XArray_Get(pArr, 12) == NULL, "The far item is taken out");
    CHECK(XArray_Used(pArr) == 1 && !strcmp((char*)XArray_GetData(pArr, 0), "used"), "The used one stays");
    XArray_ClearData(pArr, pFar);

    XArray_Delete(pArr, 0);
    CHECK(XArray_Used(pArr) == 0 && !strcmp((char*)XArray_GetData(pArr, 5), "near"), "Nothing used, the near item is kept");
    CHECK(XArray_Remove(pArr, 99) == NULL, "A slot past the table is not removed");
    CHECK(g_nArrayCleared == 4, "Every deleted or removed item was released once");

    XArray_Destroy(pArr);
    CHECK(g_nArrayCleared == 5, "And the one left with the array");
    return 0;
}

static int XTest_FillKeys(xarray_t *pArr, const uint32_t *pKeys, size_t nCount)
{
    static const char sData[64] = "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijk";
    for (size_t i = 0; i < nCount; i++)
        CHECK(XArray_AddDataKey(pArr, (void*)sData, 1 + pKeys[i] % 60, pKeys[i]) == (int)i, "Add a keyed item");
    return 0;
}

/* The items come first in order, the empty slots after them */
static int XTest_HolesLast(xarray_t *pArr, size_t nItems, int bBySize)
{
    for (size_t i = 0; i < XArray_Used(pArr); i++)
    {
        xarray_data_t *pData = XArray_Get(pArr, i);
        CHECK((pData != NULL) == (i < nItems), "Every item is in front of every empty slot");
        if (pData == NULL || i == 0) continue;

        xarray_data_t *pPrev = XArray_Get(pArr, i - 1);
        CHECK(bBySize ? pPrev->nSize <= pData->nSize : pPrev->nKey <= pData->nKey, "The items are in order");
    }
    return 0;
}

static int XTest_CompareItems(const void *pFirst, const void *pSecond, void *pCtx)
{
    /* A comparator of the caller's is given the empty slots, as it always was */
    if (pFirst == NULL || pSecond == NULL)
    {
        *(int*)pCtx = 1;
        return (pFirst == NULL) - (pSecond == NULL);
    }

    const xarray_data_t *pLeft = (const xarray_data_t*)pFirst;
    const xarray_data_t *pRight = (const xarray_data_t*)pSecond;
    return (pLeft->nKey > pRight->nKey) - (pLeft->nKey < pRight->nKey);
}

static int XTest_sort_holes(void)
{
    /* The comparators of SortBy() read through an empty slot. They put it
       last now, which a comparator of the caller's can do as well. */
    const size_t counts[] = {7, 40, 301};
    for (size_t c = 0; c < sizeof(counts) / sizeof(*counts); c++)
    {
        size_t nCount = counts[c];
        uint32_t keys[301];
        for (size_t i = 0; i < nCount; i++) keys[i] = (uint32_t)((i * 7919u + 13u) % 257u);

        for (int nSort = 0; nSort < 4; nSort++)
        {
            xarray_t *pArr = XArray_New(NULL, 0, XFALSE);
            CHECK(pArr != NULL && XTest_FillKeys(pArr, keys, nCount) == 0, "Fill an array");

            for (size_t i = 0; i < nCount; i += 5) XArray_FreeData(XArray_Set(pArr, i, NULL));
            XArray_FreeData(XArray_Set(pArr, nCount - 1, NULL));

            size_t nHoles = 0;
            for (size_t i = 0; i < nCount; i++) nHoles += XArray_Get(pArr, i) == NULL;

            int bGivenHole = 0;
            if (nSort == 0) XArray_SortBy(pArr, XARRAY_SORTBY_KEY);
            else if (nSort == 1) XArray_SortBy(pArr, XARRAY_SORTBY_SIZE);
            else if (nSort == 2) XArray_Sort(pArr, XTest_CompareItems, &bGivenHole);
            else XArray_BubbleSort(pArr, XTest_CompareItems, &bGivenHole);

            CHECK(nSort < 2 || bGivenHole, "A comparator of the caller's still sees the empty slots");
            CHECK(XArray_Used(pArr) == nCount, "Sorting keeps every slot");
            CHECK(XTest_HolesLast(pArr, nCount - nHoles, nSort == 1) == 0, "Empty slots sort last");
            XArray_Destroy(pArr);
        }
    }
    return 0;
}

static uint32_t g_nCompareSeed;
static int XTest_AlwaysLess(const void *a, const void *b, void *c) { (void)a; (void)b; (void)c; return -1; }
static int XTest_AlwaysMore(const void *a, const void *b, void *c) { (void)a; (void)b; (void)c; return 1; }
static int XTest_Coin(const void *a, const void *b, void *c)
{
    (void)a; (void)b; (void)c;
    g_nCompareSeed = g_nCompareSeed * 1103515245u + 12345u;
    return (int)((g_nCompareSeed >> 16) % 3u) - 1;
}

static int XTest_broken_comparator(void)
{
    /* The partition trusted the comparator to stop its scans at the pivot,
       and one that does not answer consistently ran them off the table. The
       order it gets is anything, but the sort ends and keeps every item. */
    const xarray_comparator_t compares[] = {XTest_AlwaysLess, XTest_AlwaysMore, XTest_Coin};
    const size_t counts[] = {17, 100, 1000};
    g_nCompareSeed = 0x5eed;

    for (size_t c = 0; c < sizeof(counts) / sizeof(*counts); c++)
    {
        for (size_t f = 0; f < sizeof(compares) / sizeof(*compares); f++)
        {
            xarray_t arr;
            XArray_Init(&arr, NULL, counts[c], XFALSE);
            for (uint32_t i = 0; i < counts[c]; i++)
                CHECK(XArray_AddDataKey(&arr, NULL, 0, (i * 31u) % (uint32_t)counts[c]) >= 0, "Add a key");

            XArray_Sort(&arr, compares[f], NULL);
            XArray_SortBy(&arr, XARRAY_SORTBY_KEY);
            for (size_t i = 0; i < counts[c]; i++)
                CHECK(XArray_GetKey(&arr, i) == i, "Every item is still there once");
            XArray_Destroy(&arr);
        }
    }
    return 0;
}

static int XTest_CountCompare(const void *pFirst, const void *pSecond, void *pCtx)
{
    (*(int*)pCtx)++;
    return XTest_CompareKey(pFirst, pSecond, NULL);
}

static int XTest_bubble_passes(void)
{
    /* A pass that swaps nothing ends the bubble sort */
    xarray_t arr;
    int nCalls = 0;
    XArray_Init(&arr, NULL, 0, XFALSE);
    for (uint32_t i = 0; i < 100; i++) CHECK(XArray_AddDataKey(&arr, NULL, 0, i) >= 0, "Add a sorted key");
    XArray_BubbleSort(&arr, XTest_CountCompare, &nCalls);
    CHECK(nCalls == 99, "Sorted input takes one pass");
    XArray_Destroy(&arr);

    nCalls = 0;
    XArray_Init(&arr, NULL, 0, XFALSE);
    for (uint32_t i = 0; i < 100; i++) CHECK(XArray_AddDataKey(&arr, NULL, 0, 99 - i) >= 0, "Add a reversed key");
    XArray_BubbleSort(&arr, XTest_CountCompare, &nCalls);
    CHECK(nCalls == 99 * 100 / 2, "Reversed input takes every pass");
    for (size_t i = 0; i < 100; i++) CHECK(XArray_GetKey(&arr, i) == i, "And comes out in order");
    XArray_Destroy(&arr);
    return 0;
}

static int XTest_CompareU32(const void *pFirst, const void *pSecond)
{
    uint32_t nFirst = *(const uint32_t*)pFirst, nSecond = *(const uint32_t*)pSecond;
    return (nFirst > nSecond) - (nFirst < nSecond);
}

static int XTest_sort_model(void)
{
    /* Every size around the switch between partitioning and insertion, against qsort */
    uint32_t nSeed = 0xa11;
    for (size_t nCount = 0; nCount <= 70; nCount++)
    {
        for (int nPattern = 0; nPattern < 3; nPattern++)
        {
            uint32_t keys[70], model[70];
            for (size_t i = 0; i < nCount; i++)
            {
                nSeed = nSeed * 1664525u + 1013904223u;
                keys[i] = nPattern == 0 ? nSeed : nPattern == 1 ? nSeed % 4u : (uint32_t)(nCount - i);
                model[i] = keys[i];
            }
            qsort(model, nCount, sizeof(*model), XTest_CompareU32);

            xarray_t arr;
            XArray_Init(&arr, NULL, 0, XFALSE);
            for (size_t i = 0; i < nCount; i++) CHECK(XArray_AddDataKey(&arr, NULL, 0, keys[i]) >= 0, "Add a key");
            XArray_SortBy(&arr, XARRAY_SORTBY_KEY);
            for (size_t i = 0; i < nCount; i++) CHECK(XArray_GetKey(&arr, i) == model[i], "The sort agrees with qsort");
            XArray_Destroy(&arr);
        }
    }
    return 0;
}

static int XTest_search_read_only(void)
{
#ifdef _WIN32
    return 77;
#else
    /* A search only reads. The sentinel search wrote a sentinel into the
       last slot and put the item back, racing every other reader, and a
       reader that saved another's sentinel left a stack address there. On a
       read only table that write faults. */
    long nPage = sysconf(_SC_PAGESIZE);
    CHECK(nPage >= (long)(16 * sizeof(xarray_data_t*)), "A page holds the table");

    xarray_data_t **pTable = (xarray_data_t**)mmap(NULL, (size_t)nPage, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(pTable != MAP_FAILED, "Map a page for the table");

    xarray_data_t items[16];
    for (uint32_t i = 0; i < 16; i++)
    {
        items[i].pPool = NULL;
        items[i].pData = NULL;
        items[i].nSize = 0;
        items[i].nKey = i * 2;
        pTable[i] = &items[i];
    }
    CHECK(mprotect(pTable, (size_t)nPage, PROT_READ) == 0, "Make the table read only");

    xarray_t arr;
    memset(&arr, 0, sizeof(arr));
    arr.pData = pTable;
    arr.nSize = arr.nUsed = 16;

    for (uint32_t nKey = 0; nKey < 34; nKey++)
    {
        int nExpected = (nKey % 2 || nKey >= 32) ? XARRAY_FAILURE : (int)(nKey / 2);
        CHECK(XArray_SentinelSearch(&arr, nKey) == nExpected, "The sentinel search reads a read only table");
        CHECK(XArray_LinearSearch(&arr, nKey) == nExpected, "So does the linear search");
        CHECK(XArray_DoubleSearch(&arr, nKey) == nExpected, "And the double ended one");
        CHECK(XArray_BinarySearch(&arr, nKey) == nExpected, "And the binary one");
    }

    munmap(pTable, (size_t)nPage);
    return 0;
#endif
}

static int XTest_pool_growth(void)
{
    /* An array made by XArray_NewPool() lives in its own pool, so a clear
       could not reset the pool. The items went back one by one to a pool that
       only reclaims its last block, and every refill grew it. */
    xarray_t *pArr = XArray_NewPool(4096, 0, XFALSE);
    CHECK(pArr != NULL && (uint8_t*)pArr == pArr->pPool->pData, "The array is the first block of its pool");

    xarray_t embedded;
    CHECK(XArray_InitPool(&embedded, 4096, 0, XFALSE) == NULL && embedded.pPool != NULL, "An array with only its items pooled");

    size_t nPoolSize = 0, nEmbeddedSize = 0;
    for (int nRound = 0; nRound < 10; nRound++)
    {
        CHECK(XTest_FillCheck(pArr, nRound) == 0 && XTest_FillCheck(&embedded, nRound) == 0, "Refill the arrays");
        XArray_Clear(pArr);
        XArray_Clear(&embedded);
        CHECK(XArray_Used(pArr) == 0 && XArray_Size(pArr) >= 200, "Clearing keeps the table size");
        CHECK((uint8_t*)pArr == pArr->pPool->pData, "And the array where it is");

        if (nRound == 1)
        {
            nPoolSize = XPool_GetSize(pArr->pPool);
            nEmbeddedSize = XPool_GetSize(embedded.pPool);
        }
        else if (nRound > 1)
        {
            CHECK(XPool_GetSize(pArr->pPool) == nPoolSize, "Refilling a cleared array does not grow its pool");
            CHECK(XPool_GetSize(embedded.pPool) == nEmbeddedSize, "Nor that of an array outside its pool");
        }
    }

    XArray_Destroy(pArr);
    XArray_Destroy(&embedded);

    /* An array that is in a pool, but not at the start of a chunk, keeps the pool as it is */
    xpool_t *pPool = XPool_Create(4096);
    CHECK(pPool != NULL && xalloc(pPool, 64) != NULL, "A pool with a block in front");
    xarray_t *pInner = XArray_New(pPool, 4, XFALSE);
    CHECK(pInner != NULL, "An array after it");
    pInner->nHasPool = 1;
    CHECK(XArray_AddData(pInner, (void*)"kept", 5) == 0, "Add an item");
    size_t nUsed = XPool_GetUsed(pPool);
    XArray_Clear(pInner);
    CHECK(XArray_Used(pInner) == 0 && XPool_GetUsed(pPool) <= nUsed && XPool_GetUsed(pPool) > 64, "The pool is not reset");
    XArray_Destroy(pInner);

    /* And one that claims a pool it does not have has nothing to reset */
    xarray_t *pHeap = XArray_New(NULL, 4, XFALSE);
    CHECK(pHeap != NULL && XArray_AddData(pHeap, (void*)"x", 2) == 0, "A heap array");
    pHeap->nHasPool = 1;
    XArray_Clear(pHeap);
    CHECK(XArray_Used(pHeap) == 0 && XArray_Size(pHeap) == 4, "Clearing it keeps its table");
    XArray_Destroy(pHeap);
    return 0;
}

static int XTest_limits(void)
{
    /* A size in bytes that wrapped round got a short table, which was then
       cleared or filled past its end. */
    const size_t nMaxSlots = XSTD_MIN((size_t)INT_MAX, SIZE_MAX / sizeof(xarray_data_t*));
    xarray_t arr;
    CHECK(XArray_Init(&arr, NULL, SIZE_MAX, XFALSE) == NULL, "A table whose size in bytes wraps round is refused");
    CHECK(arr.nSize == 0 && arr.pData == NULL, "And nothing is claimed");
    CHECK(XArray_Init(&arr, NULL, nMaxSlots + 1, XFALSE) == NULL && arr.nSize == 0, "So is one an int can not index");
    XArray_Destroy(&arr);

    /* A table that would double past the limit does not grow. It is a stand
       in that is never read past its first slot. */
    xarray_data_t *pSlot = NULL;
    xarray_t full;
    XArray_Init(&full, NULL, 0, XFALSE);
    full.pData = &pSlot;
    full.nSize = full.nUsed = nMaxSlots / 2 + 1;

    CHECK(XArray_Realloc(&full) == 0 && full.eStatus == XARRAY_STATUS_NO_MEMORY, "Doubling past the limit is refused");
    CHECK(full.nSize == nMaxSlots / 2 + 1 && full.pData == &pSlot, "And leaves the table as it was");
    CHECK(XArray_AddData(&full, (void*)"x", 2) == XARRAY_FAILURE, "No data is added to it");
    CHECK(XArray_PushData(&full, NULL, 0) == XARRAY_FAILURE && XArray_Add(&full, NULL) == XARRAY_FAILURE, "Nor an item");
    CHECK(XArray_InsertData(&full, 0, (void*)"x", 2) == NULL, "Nor is anything inserted");
    CHECK(full.nUsed == nMaxSlots / 2 + 1 && pSlot == NULL, "Nothing was written");
    return 0;
}

/* Exported, though not declared in the header */
int XArray_Partitioning(xarray_t *pArr, xarray_comparator_t compare, void *pCtx, int nStart, int nFinish);
size_t XArray_CheckSpace(xarray_t *pArr);

static int XTest_add_ownership(void)
{
    /* What a failed add does with the data is unchanged. An item that was
       made and found no room goes to the clear callback, XArray_Add() always
       released what it was given, and the data of a refused insertion stays
       with the caller. */
    g_nArrayCleared = 0;
    xarray_t arr;
    CHECK(XArray_Init(&arr, NULL, 1, XTRUE) != NULL, "A fixed array of one slot");
    arr.clearCb = XTest_ReleaseItem;

    CHECK(XArray_AddData(&arr, strdup("first"), 0) == 0, "The slot is filled");
    CHECK(XArray_AddData(&arr, strdup("refused"), 0) == XARRAY_FAILURE && g_nArrayCleared == 1, "A refused add is released");
    CHECK(XArray_AddDataKey(&arr, strdup("keyed"), 0, 9) == XARRAY_FAILURE && g_nArrayCleared == 2, "So is a keyed one");
    CHECK(XArray_PushData(&arr, strdup("pushed"), 7) == XARRAY_FAILURE && g_nArrayCleared == 3, "And a pushed buffer");

    xarray_data_t *pItem = XArray_NewData(&arr, strdup("item"), 0, 0);
    CHECK(pItem != NULL, "An item is made");
    CHECK(XArray_Insert(&arr, 0, pItem) == NULL && g_nArrayCleared == 3, "An item that does not fit is not inserted");
    CHECK(XArray_InsertData(&arr, 1, (void*)"x", 2) == NULL, "Nor is data");
    CHECK(XArray_Add(&arr, pItem) == XARRAY_FAILURE && g_nArrayCleared == 4, "A failed XArray_Add() releases the item");

    XArray_Destroy(&arr);
    CHECK(g_nArrayCleared == 5, "The stored item is released with the array");

    /* The partition step is exported as before, and refuses what it can not use */
    CHECK(XArray_Partitioning(&arr, NULL, NULL, 3, 7) == 3, "No comparator partitions nothing");
    CHECK(XArray_Partitioning(NULL, XTest_CompareKey, NULL, 3, 7) == 3, "Nor does no array");
    CHECK(XArray_CheckSpace(NULL) == 0, "And no array has no space");
    return 0;
}

static int XTest_index_guards(void)
{
    CHECK(XArray_NewData(NULL, NULL, 0, 0) == NULL, "No array makes no item");
    CHECK(XArray_Init(NULL, NULL, 4, XFALSE) == NULL, "No array is initialized");
    CHECK(XArray_Remove(NULL, 0) == NULL, "No array has nothing to remove");

    /* An item offered to no array is released, as on any failure of XArray_Add() */
    xarray_t owner;
    XArray_Init(&owner, NULL, 0, XFALSE);
    CHECK(XArray_Add(NULL, XArray_NewData(&owner, (void*)"x", 2, 0)) == XARRAY_FAILURE, "No array takes an item");

    xarray_t *pArr = XArray_New(NULL, 8, XFALSE);
    CHECK(pArr != NULL, "Create an array");
    for (uint32_t i = 0; i < 3; i++) CHECK(XArray_AddDataKey(pArr, NULL, 0, i) == (int)i, "Add an item");

    XArray_Swap(pArr, 99, 0);
    CHECK(XArray_GetKey(pArr, 0) == 0, "A swap from past the used slots changes nothing");
    CHECK(XArray_Remove(pArr, 3) == NULL && XArray_Used(pArr) == 3, "A slot past the used ones is not removed");

    XArray_FreeData(XArray_Set(pArr, 1, NULL));
    CHECK(XArray_Remove(pArr, 1) == NULL && XArray_Used(pArr) == 3, "An empty slot is not removed");

    /* A fixed array with no storage gets some when it is used */
    xarray_t lazy;
    XArray_Init(&lazy, NULL, 0, XTRUE);
    for (uint32_t i = 0; i < 20; i++) CHECK(XArray_AddDataKey(&lazy, NULL, 0, i) == (int)i, "Fill an array made without storage");
    XArray_Destroy(&lazy);

    XArray_Destroy(pArr);
    return 0;
}

static int XTest_api_guards(void)
{
    xarray_t local;
    CHECK(XArray_InitPool(NULL, 64, 2, XFALSE) == NULL, "A pool array needs a holder");
    CHECK(XArray_Used(NULL) == 0 && XArray_Size(NULL) == 0 && !XArray_Contains(NULL, 0), "A missing array is empty");
    CHECK(XArray_Realloc(NULL) == 0, "A missing array has no space");
    CHECK(XArray_GetSize(NULL, 0) == 0, "A missing array has no sizes");
    XArray_ClearData(NULL, NULL);
    XArray_Delete(NULL, 0);
    XArray_Sort(NULL, XTest_CompareKey, NULL);
    XArray_QuickSort(NULL, XTest_CompareKey, NULL, 0, 1);
    XArray_BubbleSort(NULL, XTest_CompareKey, NULL);
    CHECK(XArray_Insert(NULL, 0, NULL) == NULL && XArray_InsertData(NULL, 0, NULL, 0) == NULL, "A missing array takes nothing");
    CHECK(XArray_LinearSearch(NULL, 1) == XARRAY_FAILURE && XArray_SentinelSearch(NULL, 1) == XARRAY_FAILURE,
        "A missing array has nothing to find");
    CHECK(XArray_DoubleSearch(NULL, 1) == XARRAY_FAILURE && XArray_BinarySearch(NULL, 1) == XARRAY_FAILURE,
        "A missing array has nothing to find");

    /* An empty array that never grew keeps its size */
    XArray_Init(&local, NULL, 0, XFALSE);
    CHECK(XArray_Realloc(&local) == 0, "An array of no slots keeps no slots");
    CHECK(XArray_LinearSearch(&local, 1) == XARRAY_FAILURE && XArray_SentinelSearch(&local, 1) == XARRAY_FAILURE,
        "An empty array has nothing to find");
    CHECK(XArray_DoubleSearch(&local, 1) == XARRAY_FAILURE && XArray_BinarySearch(&local, 1) == XARRAY_FAILURE,
        "An empty array has nothing to find");
    XArray_Sort(&local, XTest_CompareKey, NULL);
    XArray_BubbleSort(&local, XTest_CompareKey, NULL);
    XArray_Destroy(&local);

    xarray_t *pArr = XArray_New(NULL, 4, XFALSE);
    CHECK(pArr != NULL, "Create an array");

    /* A pushed buffer is owned, not copied, and released with the array */
    char *pOwned = strdup("owned");
    CHECK(pOwned != NULL && XArray_PushData(pArr, pOwned, strlen(pOwned) + 1) == 0, "Push an owned buffer");
    CHECK(XArray_GetData(pArr, 0) == pOwned && XArray_GetSize(pArr, 0) == 6, "The pushed buffer is kept as it is");
    CHECK(XArray_GetSize(pArr, 3) == 0 && XArray_GetSize(pArr, 100) == 0, "An empty or missing slot has no size");
    int value = 1, fallback = 2;
    CHECK(XArray_GetDataOr(pArr, 3, &fallback) == &fallback && XArray_GetKey(pArr, 3) == 0, "An empty slot has nothing");

    CHECK(XArray_Insert(pArr, 0, NULL) == NULL, "Nothing is inserted for no data");
    CHECK(XArray_Insert(pArr, 2, (xarray_data_t*)&value) == NULL, "Nothing is inserted past the used slots");
    CHECK(XArray_InsertData(pArr, 2, &value, sizeof(value)) == NULL, "No data is inserted past the used slots");
    CHECK(XArray_Set(pArr, 4, NULL) == NULL, "A slot past the capacity is not set");

    for (uint32_t i = 1; i <= 3; i++) CHECK(XArray_AddDataKey(pArr, &i, sizeof(i), 10 - i) >= 0, "Add keyed items");
    XArray_Swap(pArr, 0, 4);
    CHECK(XArray_GetData(pArr, 0) == pOwned, "A swap with a slot past the used ones changes nothing");

    XArray_QuickSort(pArr, NULL, NULL, 0, 3);
    XArray_QuickSort(pArr, XTest_CompareKey, NULL, 0, 4);
    XArray_QuickSort(pArr, XTest_CompareKey, NULL, -1, 3);
    XArray_Sort(pArr, NULL, NULL);
    XArray_BubbleSort(pArr, NULL, NULL);
    XArray_SortBy(pArr, -1);
    CHECK(XArray_GetData(pArr, 0) == pOwned && XArray_GetKey(pArr, 3) == 7, "No refused sort moved anything");

    XArray_SortBy(pArr, XARRAY_SORTBY_KEY);
    CHECK(XArray_GetKey(pArr, 0) == 0 && XArray_GetKey(pArr, 1) == 7 && XArray_GetKey(pArr, 3) == 9, "Sort by key");

    /* The double ended search passes an empty slot at its back end */
    xarray_data_t *pLast = XArray_Set(pArr, 3, NULL);
    CHECK(XArray_DoubleSearch(pArr, 8) == 2 && XArray_DoubleSearch(pArr, 0) == 0, "Search past an empty last slot");
    XArray_Delete(pArr, 3);
    CHECK(XArray_Used(pArr) == 3, "An empty used slot is deleted");
    XArray_Delete(pArr, 3);
    CHECK(XArray_Used(pArr) == 3, "A slot past the used ones is not deleted");
    XArray_FreeData(pLast);
    XArray_ClearData(NULL, XArray_Remove(pArr, 0));
    CHECK(XArray_Used(pArr) == 2, "A removed item is released without its array");

    XArray_Destroy(pArr);
    return 0;
}

XTEST_MAIN(
    XTEST_CASE(api_guards),
    XTEST_CASE(pool_clear_reuse),
    XTEST_CASE(sort_scaling),
    XTEST_CASE(sort_unsigned_keys),
    XTEST_CASE(lifecycle),
    XTEST_CASE(reference_model),
    XTEST_CASE(search_boundaries),
    XTEST_CASE(sorting),
    XTEST_CASE(mutation),
    XTEST_CASE(fixed_capacity),
    XTEST_CASE(search_agreement),
    XTEST_CASE(heap_array),
    XTEST_CASE(release_guards),
    XTEST_CASE(insert_holes),
    XTEST_CASE(null_and_bounds),
    XTEST_CASE(search_holes),
    XTEST_CASE(set_past_used),
    XTEST_CASE(sort_holes),
    XTEST_CASE(broken_comparator),
    XTEST_CASE(bubble_passes),
    XTEST_CASE(sort_model),
    XTEST_CASE(search_read_only),
    XTEST_CASE(pool_growth),
    XTEST_CASE(limits),
    XTEST_CASE(add_ownership),
    XTEST_CASE(index_guards)
)
