/*!
 *  @file libxutils/examples/array.c
 *
 *  This source is part of the "libxutils" project
 *  2015-2026  Sandro Kalatozishvili (s.kalatoz@gmail.com)
 *
 * @brief Example of working with dynamic arrays: a small leaderboard.
 * Records are copied into the array with a key, sorted by score with a
 * custom comparator, sorted by key and found with a binary search,
 * inserted and removed. A second array keeps pointers it does not copy
 * and frees them with a clear callback.
 */

#include "xstd.h"
#include "array.h"
#include "log.h"
#include "str.h"

typedef struct {
    uint32_t nID;
    char sName[XSTR_MICRO];
    uint32_t nScore;
} xplayer_t;

static const xplayer_t g_players[] = {
    { 1004, "alice", 2450 },
    { 1001, "bob", 1980 },
    { 1007, "carol", 3120 },
    { 1003, "dave", 2210 },
    { 1009, "erin", 2875 },
    { 1002, "frank", 1640 }
};

/* Comparators get the array elements (xarray_data_t) and return <0, 0 or >0 */
static int compare_score(const void *pData1, const void *pData2, void *pCtx)
{
    (void)pCtx;
    const xplayer_t *pFirst = (const xplayer_t*)((const xarray_data_t*)pData1)->pData;
    const xplayer_t *pSecond = (const xplayer_t*)((const xarray_data_t*)pData2)->pData;

    /* The highest score comes first */
    return (pSecond->nScore > pFirst->nScore) - (pSecond->nScore < pFirst->nScore);
}

static int compare_name(const void *pData1, const void *pData2, void *pCtx)
{
    (void)pCtx;
    const char *pFirst = (const char*)((const xarray_data_t*)pData1)->pData;
    const char *pSecond = (const char*)((const xarray_data_t*)pData2)->pData;
    return strcmp(pFirst, pSecond);
}

/* The array owns the strings it points to: they are freed on clear and delete */
static void clear_name(xarray_data_t *pArrData)
{
    free(pArrData->pData);
}

static void print_players(xarray_t *pArr, const char *pTitle)
{
    xlog("%s (used %zu of %zu slots):", pTitle, XArray_Used(pArr), XArray_Size(pArr));
    size_t i;

    for (i = 0; i < XArray_Used(pArr); i++)
    {
        const xplayer_t *pPlayer = (const xplayer_t*)XArray_GetData(pArr, i);
        xlog("  %zu. %-6s id(%u) score(%u)", i + 1, pPlayer->sName, XArray_GetKey(pArr, i), pPlayer->nScore);
    }

    xlog(XSTR_EMPTY);
}

int main()
{
    xlog_defaults();
    xlog_enable(XLOG_INFO);

//////////////////////////////////////////////////////////////////////////////////////////////
// Copy the records into the array, the player ID is the key of each element
//////////////////////////////////////////////////////////////////////////////////////////////

    xarray_t players;
    XArray_Init(&players, NULL, 4, XFALSE);
    size_t i;

    for (i = 0; i < sizeof(g_players) / sizeof(g_players[0]); i++)
    {
        /* The data is copied, so the array does not depend on g_players */
        const xplayer_t *pPlayer = &g_players[i];
        if (XArray_AddDataKey(&players, (void*)pPlayer, sizeof(xplayer_t), pPlayer->nID) < 0)
        {
            xloge("Failed to add player: %s", pPlayer->sName);
            XArray_Destroy(&players);
            return XSTDERR;
        }
    }

    /* The array was created for 4 elements and has grown by itself */
    print_players(&players, "Players as added");

//////////////////////////////////////////////////////////////////////////////////////////////
// Sort with a custom comparator
//////////////////////////////////////////////////////////////////////////////////////////////

    XArray_Sort(&players, compare_score, NULL);
    print_players(&players, "Leaderboard");

//////////////////////////////////////////////////////////////////////////////////////////////
// Sort by key and find an element with a binary search
//////////////////////////////////////////////////////////////////////////////////////////////

    /* A binary search needs the elements sorted by their keys */
    XArray_SortBy(&players, XARRAY_SORTBY_KEY);
    print_players(&players, "Sorted by ID");

    int nIndex = XArray_BinarySearch(&players, 1007);
    if (nIndex < 0)
    {
        xloge("Player 1007 was not found");
        XArray_Destroy(&players);
        return XSTDERR;
    }

    const xplayer_t *pFound = (const xplayer_t*)XArray_GetData(&players, nIndex);
    xlog("Player 1007 is %s at index %d", pFound->sName, nIndex);
    xlog("Player 1005 is %s\n", XArray_BinarySearch(&players, 1005) < 0 ? "not found" : "found");

//////////////////////////////////////////////////////////////////////////////////////////////
// Insert and remove elements
//////////////////////////////////////////////////////////////////////////////////////////////

    /* An element made in advance keeps its key, it belongs to the caller until inserted */
    const xplayer_t newcomer = { 1000, "grace", 2990 };
    xarray_data_t *pNewData = XArray_NewData(&players, (void*)&newcomer, sizeof(newcomer), newcomer.nID);

    if (pNewData == NULL || XArray_Insert(&players, 0, pNewData) == NULL)
    {
        xloge("Failed to insert player: %s", newcomer.sName);
        XArray_FreeData(pNewData);
        XArray_Destroy(&players);
        return XSTDERR;
    }

    xlog("Inserted %s at the front", newcomer.sName);

    /* Remove() hands the element over to the caller, Delete() frees it */
    nIndex = XArray_BinarySearch(&players, 1002);
    if (nIndex >= 0)
    {
        xarray_data_t *pRemoved = XArray_Remove(&players, (size_t)nIndex);
        xlog("Removed %s", ((const xplayer_t*)pRemoved->pData)->sName);
        XArray_FreeData(pRemoved);
    }

    size_t nLast = XArray_Used(&players) - 1;
    xlog("Deleted %s\n", ((const xplayer_t*)XArray_GetData(&players, nLast))->sName);
    XArray_Delete(&players, nLast);
    print_players(&players, "Board after the changes");

//////////////////////////////////////////////////////////////////////////////////////////////
// Keep pointers instead of copies, a clear callback frees them
//////////////////////////////////////////////////////////////////////////////////////////////

    xarray_t names;
    XArray_Init(&names, NULL, 4, XFALSE);
    names.clearCb = clear_name;

    for (i = 0; i < XArray_Used(&players); i++)
    {
        const xplayer_t *pPlayer = (const xplayer_t*)XArray_GetData(&players, i);
        char *pName = xstrdup(pPlayer->sName);

        /* A zero size stores the pointer itself instead of a copy */
        if (pName == NULL || XArray_AddData(&names, pName, 0) < 0)
        {
            xloge("Failed to add name: %s", pPlayer->sName);
            free(pName);
            XArray_Destroy(&names);
            XArray_Destroy(&players);
            return XSTDERR;
        }
    }

    XArray_Sort(&names, compare_name, NULL);
    xlog_wn("Names in order:");

    for (i = 0; i < XArray_Used(&names); i++)
        xlog_wn(" %s", (const char*)XArray_GetData(&names, i));

    xlog(XSTR_EMPTY);

    /* Destroy() clears every element, the callback frees the strings */
    XArray_Destroy(&names);
    XArray_Destroy(&players);
    return 0;
}
