/*!
 *  @file libxutils/src/data/xmap.c
 *
 *  This source is part of the "libxutils" project
 *  2019-2020  Sandro Kalatozishvili (s.kalatoz@gmail.com)
 *
 * @brief Implementation of dynamically allocated hash map
 */

#include "str.h"
#include "map.h"

#ifdef _XMAP_USE_CRYPT
#include "crc32.h"
#include "sha256.h"
#endif

#define XFNV_OFFSET_32 2166136261
#define XFNV_PRIME_32  16777619

/* A probe and the hash it starts from are int, so a table has no more
   slots than an int indexes, and its size in bytes has to fit size_t.
   Past INT_MAX the hash came back negative and the probe ran off the table. */
#define XMAP_MAX_SIZE ((uint32_t)XSTD_MIN((size_t)INT_MAX, SIZE_MAX / sizeof(xmap_pair_t)))

static xmap_pair_t* XMap_NewTable(xpool_t *pPool, uint32_t nSize)
{
    xmap_pair_t *pPairs = (xmap_pair_t*)xalloc(pPool, (size_t)nSize * sizeof(xmap_pair_t));
    if (pPairs == NULL) return NULL;

    uint32_t i;
    for (i = 0; i < nSize; i++)
    {
        pPairs[i].eStatus = XMAP_PAIR_UNUSED;
        pPairs[i].pData = NULL;
        pPairs[i].pKey = NULL;
    }

    return pPairs;
}

int XMap_Init(xmap_t *pMap, xpool_t *pPool, uint32_t nSize)
{
    XCHECK((pMap != NULL), XMAP_OINV);

    pMap->nTableSize = 0;
    pMap->clearCb = NULL;
    pMap->pPairs = NULL;
    pMap->nDeleted = 0;
    pMap->nCount = 0;
    pMap->pPool = pPool;
    pMap->eHashType = XMAP_HASH_FNV;
    pMap->bAllowUpdate = XTRUE;
    pMap->bAlloc = XFALSE;

    if (nSize > XMAP_MAX_SIZE) return XMAP_OINV;
    if (!nSize) return XMAP_OK;

    /* A map whose table could not be made has no size. It kept the one it
       asked for, a size no table had, and the next Put() doubled it. */
    pMap->pPairs = XMap_NewTable(pPool, nSize);
    if (pMap->pPairs == NULL) return XMAP_OMEM;

    pMap->nTableSize = nSize;
    return XMAP_OK;
}

xmap_t *XMap_New(xpool_t *pPool, uint32_t nSize)
{
    xmap_t *pMap = (xmap_t*)xalloc(pPool, sizeof(xmap_t));
    XCHECK((pMap != NULL), NULL);

    if (XMap_Init(pMap, pPool, nSize) < 0)
    {
        xfreen(pPool, pMap, sizeof(xmap_t));
        return NULL;
    }

    pMap->bAlloc = XTRUE;
    return pMap;
}

void XMap_Free(xmap_t *pMap)
{
    if (pMap != NULL)
    {
        xpool_t *pPool = pMap->pPool;

        /* With its size, so a pool the table came from can take it back */
        if (pMap->pPairs != NULL)
        {
             xfreen(pPool, pMap->pPairs, (size_t)pMap->nTableSize * sizeof(xmap_pair_t));
             pMap->pPairs = NULL;
        }

        if (pMap->bAlloc)
        {
            xfreen(pPool, pMap, sizeof(xmap_t));
        }
        else
        {
            pMap->nTableSize = 0;
            pMap->clearCb = NULL;
            pMap->nDeleted = 0;
            pMap->nCount = 0;
        }
    }
}

/* Reset() hands over the map and one of its used pairs */
static void XMap_ClearIt(xmap_pair_t *pPair, xmap_t *pMap)
{
    if (pMap->clearCb != NULL)
        pMap->clearCb(pPair);

    pPair->eStatus = XMAP_PAIR_UNUSED;
    pPair->pData = NULL;
    pPair->pKey = NULL;
}

int XMap_Iterate(xmap_t *pMap, xmap_iterator_t itfunc, void *pCtx)
{
    XCHECK((pMap != NULL), XMAP_OINV);
    XCHECK_NL((pMap->pPairs != NULL), XMAP_EINIT);

    if (!XMap_UsedSize(pMap)) return XMAP_EMPTY;
    uint32_t i;

    for (i = 0; i < pMap->nTableSize; i++)
    {
        if (pMap->pPairs[i].eStatus == XMAP_PAIR_USED)
        {
            int nStatus = itfunc(&pMap->pPairs[i], pCtx);
            if (nStatus != XMAP_OK) return nStatus;
        }
    }

    return XMAP_OK;
}

void XMap_Reset(xmap_t *pMap)
{
    XCHECK_VOID((pMap != NULL));
    XCHECK_VOID_NL((pMap->pPairs != NULL));

    uint32_t i;
    for (i = 0; i < pMap->nTableSize; i++)
    {
        if (pMap->pPairs[i].eStatus == XMAP_PAIR_USED)
            XMap_ClearIt(&pMap->pPairs[i], pMap);
        else if (pMap->pPairs[i].eStatus == XMAP_PAIR_DELETED)
            pMap->pPairs[i].eStatus = XMAP_PAIR_UNUSED;
    }

    pMap->nCount = 0;
    pMap->nDeleted = 0;
}

void XMap_Destroy(xmap_t *pMap)
{
    XMap_Reset(pMap);
    XMap_Free(pMap);
}

#ifdef _XMAP_USE_CRYPT
int XMap_HashMIX(xmap_t *pMap, const char *pStr)
{
    XCHECK((pMap != NULL), XMAP_OINV);
    XCHECK_NL((pStr != NULL), XMAP_OINV);

    if (!pMap->nTableSize) return XMAP_EINIT;
    uint32_t nHash = XCRC32_Compute((unsigned char*)(pStr), strlen(pStr));

    /* Robert Jenkins' 32 bit Mix Function */
    nHash += (nHash << 12);
    nHash ^= (nHash >> 22);
    nHash += (nHash << 4);
    nHash ^= (nHash >> 9);
    nHash += (nHash << 10);
    nHash ^= (nHash >> 2);
    nHash += (nHash << 7);
    nHash ^= (nHash >> 12);

    /* Knuth's Multiplicative Method */
    nHash = (nHash >> 3) * 2654435761;
    return nHash % pMap->nTableSize;
}

int XMap_HashCRC32(xmap_t *pMap, const char *pStr)
{
    XCHECK((pMap != NULL), XMAP_OINV);
    XCHECK_NL((pStr != NULL), XMAP_OINV);

    if (!pMap->nTableSize) return XMAP_EINIT;
    uint32_t nHash = XCRC32_Compute((unsigned char*)(pStr), strlen(pStr));

    nHash = ((nHash >> 16) ^ nHash) * 0x45d9f3b;
    nHash = ((nHash >> 16) ^ nHash) * 0x45d9f3b;
    nHash = (nHash >> 16) ^ nHash;
    return nHash % pMap->nTableSize;
}

int XMap_HashSHA256(xmap_t *pMap, const char *pStr)
{
    XCHECK((pMap != NULL), XMAP_OINV);
    XCHECK_NL((pStr != NULL), XMAP_OINV);
    if (!pMap->nTableSize) return XMAP_EINIT;

    unsigned char hash[XSHA256_LENGTH + 1];
    XSHA256_ComputeSum((char*)hash, sizeof(hash), (const uint8_t*)pStr, strlen(pStr));

    uint32_t nHash = 0;
    for (int i = 0; i < XSHA256_LENGTH; i++)
    {
        nHash ^= hash[i];
        nHash += (nHash << 10);
        nHash ^= (nHash >> 6);
    }

    nHash += (nHash << 3);
    nHash ^= (nHash >> 11);
    nHash += (nHash << 15);

    return nHash % pMap->nTableSize;
}
#endif /* _XMAP_USE_CRYPT */

int XMap_HashFNV(xmap_t *pMap, const char *pStr)
{
    XCHECK((pMap != NULL), XMAP_OINV);
    XCHECK_NL((pStr != NULL), XMAP_OINV);
    if (!pMap->nTableSize) return XMAP_EINIT;
    uint32_t nHash = XFNV_OFFSET_32;

    while (*pStr)
    {
        nHash ^= (uint8_t)*pStr++;
        nHash *= XFNV_PRIME_32;
    }

    return nHash % pMap->nTableSize;
}

int XMap_Hash(xmap_t *pMap, const char *pStr)
{
    XCHECK((pMap != NULL), XMAP_OINV);
    XCHECK((pStr != NULL), XMAP_OINV);

    switch (pMap->eHashType)
    {
        case XMAP_HASH_FNV:
            return XMap_HashFNV(pMap, pStr);
#ifdef _XMAP_USE_CRYPT
        case XMAP_HASH_MIX:
            return XMap_HashMIX(pMap, pStr);
        case XMAP_HASH_CRC32:
            return XMap_HashCRC32(pMap, pStr);
        case XMAP_HASH_SHA256:
            return XMap_HashSHA256(pMap, pStr);
#endif
        default:
            break;
    }

    return XMap_HashFNV(pMap, pStr);
}

int XMap_GetHash(xmap_t *pMap, const char* pKey)
{
    XCHECK((pMap != NULL), XMAP_OINV);
    XCHECK_NL((pKey != NULL), XMAP_OINV);
    XCHECK_NL((pMap->nTableSize > 0), XMAP_EINIT);
    XCHECK_NL((pMap->pPairs != NULL), XMAP_EINIT);
    XCHECK_NL((pMap->nCount <= pMap->nTableSize), XMAP_OINV);

    int nIndex = XMap_Hash(pMap, pKey);
    if (nIndex < 0) return nIndex;

    int nFirstAvail = -1;
    uint32_t i;

    for (i = 0; i < pMap->nTableSize; i++)
    {
        xmap_pair_t *pPair = &pMap->pPairs[nIndex];

        if (pPair->eStatus == XMAP_PAIR_UNUSED)
        {
            // Return the first available slot for insertion
            return (nFirstAvail >= 0) ? nFirstAvail : nIndex;
        }

        if (pPair->eStatus == XMAP_PAIR_DELETED)
        {
            // Remember the first available slot for potential insertion
            if (nFirstAvail < 0) nFirstAvail = nIndex;
        }
        else if (xstrcmp(pPair->pKey, pKey))
        {
            // Key already exists, return index for update
            return nIndex;
        }

        nIndex = (nIndex + 1) % (int)pMap->nTableSize;
    }

    return (nFirstAvail >= 0) ? nFirstAvail : XMAP_FULL;
}

/* Places a pair of the table being rebuilt. Its keys are unique and the new
   table has a free slot for each of them, so the pair goes to the first free
   slot of its probe: there is no key to compare and nothing that can fail. */
static void XMap_Place(xmap_t *pMap, const xmap_pair_t *pPair)
{
    uint32_t nIndex = (uint32_t)XMap_Hash(pMap, pPair->pKey);

    while (pMap->pPairs[nIndex].eStatus != XMAP_PAIR_UNUSED)
        nIndex = (nIndex + 1) % pMap->nTableSize;

    pMap->pPairs[nIndex] = *pPair;
    pMap->nCount++;
}

/* Moves every entry to a new table of nSize slots, leaving the tombstones behind */
static int XMap_Rebuild(xmap_t *pMap, uint32_t nSize)
{
    xmap_pair_t *pPairs = XMap_NewTable(pMap->pPool, nSize);
    XCHECK((pPairs != NULL), XMAP_OMEM);

    xmap_pair_t *pOld = pMap->pPairs;
    uint32_t i, nOldSize = pMap->nTableSize;

    pMap->pPairs = pPairs;
    pMap->nTableSize = nSize;
    pMap->nDeleted = 0;
    pMap->nCount = 0;

    for (i = 0; pOld != NULL && i < nOldSize; i++)
    {
        if (pOld[i].eStatus == XMAP_PAIR_USED)
            XMap_Place(pMap, &pOld[i]);
    }

    xfreen(pMap->pPool, pOld, (size_t)nOldSize * sizeof(xmap_pair_t));
    return XMAP_OK;
}

int XMap_Realloc(xmap_t *pMap)
{
    XCHECK((pMap != NULL), XMAP_OINV);
    XCHECK((pMap->nTableSize <= XMAP_MAX_SIZE / 2), XMAP_OINV);
    return XMap_Rebuild(pMap, pMap->nTableSize ? pMap->nTableSize * 2 : XMAP_INITIAL_SIZE);
}

int XMap_Put(xmap_t *pMap, char* pKey, void *pValue)
{
    XCHECK((pMap != NULL), XMAP_OINV);
    XCHECK_NL((pKey != NULL), XMAP_OINV);

    if (pMap->pPairs == NULL ||
        ((uint64_t)pMap->nCount * 10 >= (uint64_t)pMap->nTableSize * 7))
    {
        int nStatus = XMap_Realloc(pMap);
        if (nStatus < 0) return nStatus;
    }

    /* Grown before it is full, the table has a slot for a key that is not there */
    int nHash = XMap_GetHash(pMap, pKey);
    if (nHash < 0) return nHash;

    if (pMap->pPairs[nHash].eStatus == XMAP_PAIR_DELETED && pMap->nDeleted > 0) pMap->nDeleted--;
    if (pMap->pPairs[nHash].eStatus != XMAP_PAIR_USED) pMap->nCount++;
    else if (!pMap->bAllowUpdate) return XMAP_EEXIST;

    /* Set the data */
    pMap->pPairs[nHash].pData = pValue;
    pMap->pPairs[nHash].pKey = pKey;
    pMap->pPairs[nHash].eStatus = XMAP_PAIR_USED;

    /* Tombstone pressure trigger. The pair is already stored and a failed
       rebuild keeps the old table, so the insertion has succeeded either way. */
    if (pMap->nDeleted > pMap->nTableSize / 4) XMap_Rebuild(pMap, pMap->nTableSize);

    return XMAP_OK;
}

int XMap_PutPair(xmap_t *pMap, xmap_pair_t *pPair)
{
    XCHECK((pMap != NULL), XMAP_OINV);
    XCHECK((pPair != NULL), XMAP_OINV);
    return XMap_Put(pMap, pPair->pKey, pPair->pData);
}

int XMap_Update(xmap_t *pMap, int nHash, void *pValue)
{
    XCHECK((pMap != NULL), XMAP_OINV);
    XCHECK_NL((nHash >= 0), XMAP_OINV);

    if ((uint32_t)nHash >= pMap->nTableSize)
        return XMAP_MISSING;

    /* Only an entry there is can be updated: a free or deleted slot has no key, and taking it as an entry gave
       the map one that every lookup, walk and clear callback then met with no key, or with one already freed */
    xmap_pair_t *pPair = &pMap->pPairs[nHash];
    if (pPair->eStatus != XMAP_PAIR_USED) return XMAP_MISSING;

    pPair->pData = pValue;
    return XMAP_OK;
}

xmap_pair_t *XMap_GetPair(xmap_t *pMap, const char* pKey)
{
    XCHECK((pMap != NULL), NULL);
    XCHECK_NL((pKey != NULL), NULL);
    XCHECK_NL((pMap->pPairs != NULL), NULL);

    uint32_t i;
    int nIndex = XMap_Hash(pMap, pKey);
    if (nIndex < 0) return NULL;

    for (i = 0; i < pMap->nTableSize; i++)
    {
        xmap_pair_t *pPair = &pMap->pPairs[nIndex];

        if (pPair->eStatus == XMAP_PAIR_UNUSED) break;
        if (pPair->eStatus == XMAP_PAIR_USED &&
            xstrcmp(pPair->pKey, pKey)) return pPair;

        nIndex = (nIndex + 1) % (int)pMap->nTableSize;
    }

    return NULL;
}

void* XMap_GetIndex(xmap_t *pMap, const char* pKey, int *pIndex)
{
    XCHECK((pMap != NULL), NULL);
    XCHECK((pKey != NULL), NULL);
    XCHECK((pIndex != NULL), NULL);
    XCHECK_NL((pMap->pPairs != NULL), NULL);

    *pIndex = -1;
    uint32_t i;

    *pIndex = XMap_Hash(pMap, pKey);
    if (*pIndex < 0) return NULL;

    for (i = 0; i < pMap->nTableSize; i++)
    {
        xmap_pair_t *pPair = &pMap->pPairs[*pIndex];

        if (pPair->eStatus == XMAP_PAIR_UNUSED) break;
        if (pPair->eStatus == XMAP_PAIR_USED &&
            xstrcmp(pPair->pKey, pKey)) return pPair->pData;

        *pIndex = (*pIndex + 1) % (int)pMap->nTableSize;
    }

    *pIndex = -1;
    return NULL;
}

void* XMap_Get(xmap_t *pMap, const char* pKey)
{
    XCHECK((pMap != NULL), NULL);
    XCHECK_NL((pKey != NULL), NULL);
    int nDummy = 0;
    return XMap_GetIndex(pMap, pKey, &nDummy);
}

int XMap_Remove(xmap_t *pMap, const char* pKey)
{
    XCHECK((pMap != NULL), XMAP_OINV);
    XCHECK_NL((pKey != NULL), XMAP_OINV);
    XCHECK_NL((pMap->pPairs != NULL), XMAP_EINIT);

    int nIndex = XMap_Hash(pMap, pKey);
    if (nIndex < 0) return nIndex;

    uint32_t i;
    for (i = 0; i < pMap->nTableSize; i++)
    {
        xmap_pair_t *pPair = &pMap->pPairs[nIndex];
        if (pPair->eStatus == XMAP_PAIR_UNUSED) break;

        if (pPair->eStatus == XMAP_PAIR_USED && xstrcmp(pPair->pKey, pKey))
        {
            if (pMap->clearCb != NULL) pMap->clearCb(pPair);
            if (pMap->nCount > 0) pMap->nCount--;

            pPair->eStatus = XMAP_PAIR_DELETED;
            pPair->pData = NULL;
            pPair->pKey = NULL;

            pMap->nDeleted++;
            return XMAP_OK;
        }

        nIndex = (nIndex + 1) % (int)pMap->nTableSize;
    }

    return XMAP_MISSING;
}

int XMap_UsedSize(xmap_t *pMap)
{
    XCHECK((pMap != NULL), XMAP_OINV);
    return (int)pMap->nCount;
}
