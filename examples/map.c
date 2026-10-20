/*!
 *  @file libxutils/examples/map.c
 *
 *  This source is part of the "libxutils" project
 *  2015-2026  Sandro Kalatozishvili (s.kalatoz@gmail.com)
 *
 * @brief Example of working with hash maps: a word frequency counter.
 * The map keeps pointers, it copies nothing: every word has one entry
 * that holds both the key and the count, and a clear callback frees it
 * when the word is removed or the map is destroyed.
 */

#include "xstd.h"
#include "map.h"
#include "log.h"
#include "str.h"

typedef struct {
    char sWord[XSTR_MICRO];
    int nCount;
} xword_t;

typedef struct {
    const xword_t *pTop;
    int nShown;
} xword_stats_t;

static const char *g_pText =
    "The quick brown fox jumps over the lazy dog. The dog sleeps, "
    "the fox runs, and the quick fox jumps again over the sleeping dog.";

/* Called for every pair that is removed, reset or destroyed */
static void clear_word(xmap_pair_t *pPair)
{
    free(pPair->pData);
}

static xword_t* create_word(const char *pWord)
{
    xword_t *pEntry = (xword_t*)malloc(sizeof(xword_t));
    if (pEntry == NULL) return NULL;

    xstrncpy(pEntry->sWord, sizeof(pEntry->sWord), pWord);
    pEntry->nCount = 0;
    return pEntry;
}

/* Iterator: XMAP_OK goes on to the next pair, any other value stops the iteration */
static int collect_stats(xmap_pair_t *pPair, void *pCtx)
{
    xword_stats_t *pStats = (xword_stats_t*)pCtx;
    const xword_t *pEntry = (const xword_t*)pPair->pData;

    if (pStats->pTop == NULL || pEntry->nCount > pStats->pTop->nCount) pStats->pTop = pEntry;
    if (pEntry->nCount > 1) xlog_wn("%s%s(%d)", pStats->nShown++ ? ", " : " ", pEntry->sWord, pEntry->nCount);

    return XMAP_OK;
}

static XSTATUS count_words(xmap_t *pMap, const char *pText)
{
    /* xstrtok() cuts the text it works on, so it gets a copy */
    char *pCopy = xstrdup(pText);
    if (pCopy == NULL) return XSTDERR;

    char *pContext = NULL;
    char *pWord = xstrtok(pCopy, " ,.", &pContext);

    while (pWord != NULL)
    {
        xstrcase(pWord, XSTR_LOWER);
        xword_t *pEntry = (xword_t*)XMap_Get(pMap, pWord);

        if (pEntry == NULL)
        {
            pEntry = create_word(pWord);

            /* The key points into the entry, so the entry is all there is to free */
            if (pEntry == NULL || XMap_Put(pMap, pEntry->sWord, pEntry) != XMAP_OK)
            {
                free(pEntry);
                free(pCopy);
                return XSTDERR;
            }
        }

        pEntry->nCount++;
        pWord = xstrtok(NULL, " ,.", &pContext);
    }

    free(pCopy);
    return XSTDOK;
}

int main()
{
    xlog_defaults();
    xlog_enable(XLOG_INFO);

//////////////////////////////////////////////////////////////////////////////////////////////
// Count the words of a text
//////////////////////////////////////////////////////////////////////////////////////////////

    xmap_t map;
    if (XMap_Init(&map, NULL, 8) != XMAP_OK)
    {
        xloge("Failed to initialize the map");
        return XSTDERR;
    }

    /* Refuse a key that is there already instead of replacing its pointers */
    map.bAllowUpdate = XFALSE;
    map.clearCb = clear_word;

    if (count_words(&map, g_pText) != XSTDOK)
    {
        xloge("Failed to count the words");
        XMap_Destroy(&map);
        return XSTDERR;
    }

    /* The table started with 8 slots and has grown with the words */
    xlog("Text: %s", g_pText);
    xlog("%d different words, table of %u slots\n", XMap_UsedSize(&map), map.nTableSize);

//////////////////////////////////////////////////////////////////////////////////////////////
// Look up words and walk over every pair
//////////////////////////////////////////////////////////////////////////////////////////////

    const xword_t *pFox = (const xword_t*)XMap_Get(&map, "fox");
    xlog("\"fox\" is used %d times", pFox != NULL ? pFox->nCount : 0);
    xlog("\"cat\" is %s", XMap_Get(&map, "cat") != NULL ? "there" : "not there");

    xword_stats_t stats = { NULL, 0 };
    xlog_wn("Used more than once:");
    XMap_Iterate(&map, collect_stats, &stats);

    xlog(XSTR_EMPTY);
    xlog("The most used word is \"%s\"\n", stats.pTop != NULL ? stats.pTop->sWord : XSTR_EMPTY);

//////////////////////////////////////////////////////////////////////////////////////////////
// A key that exists already, then remove a key
//////////////////////////////////////////////////////////////////////////////////////////////

    xword_t *pDuplicate = create_word("dog");
    int nStatus = pDuplicate != NULL ? XMap_Put(&map, pDuplicate->sWord, pDuplicate) : XMAP_OMEM;

    /* The map did not take the entry, it is still ours to free */
    xlog("Adding \"dog\" again: %s", nStatus == XMAP_EEXIST ? "refused, it exists" : "unexpected result");
    free(pDuplicate);

    /* Remove() calls the clear callback, which frees the entry */
    nStatus = XMap_Remove(&map, "the");
    xlog("Removing \"the\": %s, %d words left", nStatus == XMAP_OK ? "done" : "failed", XMap_UsedSize(&map));

    /* Destroy() calls the clear callback for every pair that is left */
    XMap_Destroy(&map);
    return 0;
}
