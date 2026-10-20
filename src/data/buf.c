/*!
 *  @file libxutils/src/data/buf.c
 *
 *  This source is part of the "libxutils" project
 *  2015-2020  Sandro Kalatozishvili (s.kalatoz@gmail.com)
 *
 * @brief Dynamically allocated byte and data buffers
 */

#include "xstd.h"
#include "buf.h"

uint8_t *XByteData_Dup(const uint8_t *pBuff, size_t nLength)
{
    if (pBuff == NULL || !nLength || nLength == SIZE_MAX) return NULL;

    uint8_t *pData = (uint8_t*)malloc(nLength + 1);
    if (pData == NULL) return NULL;

    memcpy(pData, pBuff, nLength);
    pData[nLength] = '\0';

    return pData;
}

int XByteBuffer_Resize(xbyte_buffer_t *pBuffer, size_t nSize)
{
    XCHECK(((pBuffer != NULL)), XSTDINV);

    if (nSize > INT_MAX)
    {
        pBuffer->nStatus = XSTDERR;
        return pBuffer->nStatus;
    }

    if (!nSize)
    {
        XByteBuffer_Clear(pBuffer);
        return XSTDNON;
    }

    if (pBuffer->pData == NULL)
    {
        pBuffer->pData = (uint8_t*)malloc(nSize);
        if (pBuffer->pData == NULL)
        {
            pBuffer->nStatus = XSTDERR;
            return pBuffer->nStatus;
        }

        pBuffer->nSize = nSize;
        pBuffer->nUsed = 0;
        return (int)nSize;
    }

    if (!pBuffer->nSize)
    {
        if (pBuffer->nUsed >= nSize)
            return (int)pBuffer->nUsed;

        pBuffer->nStatus = XSTDERR;
        return pBuffer->nStatus;
    }

    uint8_t* pOldData = pBuffer->pData;
    pBuffer->pData = (uint8_t*)realloc(pBuffer->pData, nSize);

    if (pBuffer->pData == NULL)
    {
        pBuffer->pData = pOldData;
        pBuffer->nStatus = XSTDERR;
        return pBuffer->nStatus;
    }

    pBuffer->nUsed = (pBuffer->nUsed >= nSize) ? (nSize - 1) : pBuffer->nUsed;
    if (pBuffer->nUsed < nSize) pBuffer->pData[pBuffer->nUsed] = '\0';

    pBuffer->nSize = nSize;
    return (int)pBuffer->nSize;
}

int XByteBuffer_Terminate(xbyte_buffer_t *pBuffer, size_t nPosit)
{
    XCHECK(((pBuffer != NULL)), XSTDINV);
    if (pBuffer->pData == NULL || !pBuffer->nUsed) return XSTDERR;

    size_t nTerminatePosit = XSTD_MIN(pBuffer->nUsed, nPosit);
    size_t nCapacity = pBuffer->nSize ? pBuffer->nSize : pBuffer->nUsed;

    if (nTerminatePosit >= nCapacity &&
        XByteBuffer_Reserve(pBuffer, 1) <= 0)
        return XSTDERR;

    pBuffer->pData[nTerminatePosit] = '\0';
    pBuffer->nUsed = nTerminatePosit;

    return XSTDOK;
}

int XByteBuffer_Reserve(xbyte_buffer_t *pBuffer, size_t nSize)
{
    XCHECK(((pBuffer != NULL)), XSTDINV);
    if (pBuffer->nStatus < 0) return pBuffer->nStatus;

    if (pBuffer->nUsed > INT_MAX ||
        nSize > (size_t)INT_MAX - pBuffer->nUsed)
        return pBuffer->nStatus = XSTDERR;

    size_t nNewSize = pBuffer->nUsed + nSize;
    if (nNewSize <= pBuffer->nSize) return (int)pBuffer->nSize;
    else if (pBuffer->nFast && nNewSize <= INT_MAX / 2) nNewSize *= 2;

    return XByteBuffer_Resize(pBuffer, nNewSize);
}

xbyte_buffer_t* XByteBuffer_New(size_t nSize, int nFastAlloc)
{
    xbyte_buffer_t *pBuffer = (xbyte_buffer_t*)malloc(sizeof(xbyte_buffer_t));
    if (pBuffer == NULL) return NULL;

    if (XByteBuffer_Init(pBuffer, nSize, nFastAlloc) < 0)
    {
        free(pBuffer);
        return NULL;
    }

    pBuffer->nAlloc = 1;
    return pBuffer;
}

int XByteBuffer_Init(xbyte_buffer_t *pBuffer, size_t nSize, int nFastAlloc)
{
    XCHECK((pBuffer), XSTDINV);
    pBuffer->nStatus = XSTDOK;
    pBuffer->nAlloc = XSTDNON;
    pBuffer->nFast = nFastAlloc;
    pBuffer->nSize = 0;
    pBuffer->nUsed = 0;
    pBuffer->pData = NULL;
    return XByteBuffer_Reserve(pBuffer, nSize);
}

void XByteBuffer_Clear(xbyte_buffer_t *pBuffer)
{
    XCHECK_VOID(pBuffer);

    if (pBuffer->pData != NULL &&
        pBuffer->nSize > 0)
        free(pBuffer->pData);

    pBuffer->nStatus = 0;
    pBuffer->nSize = 0;
    pBuffer->nUsed = 0;
    pBuffer->pData = NULL;
}

void XByteBuffer_Free(xbyte_buffer_t **pBuffer)
{
    XCHECK_VOID_NL((pBuffer && *pBuffer));
    xbyte_buffer_t *pByteBuff = *pBuffer;

    XByteBuffer_Clear(pByteBuff);
    if (pByteBuff->nAlloc)
    {
        free(pByteBuff);
        *pBuffer = NULL;
    }
}

void XByteBuffer_Reset(xbyte_buffer_t *pBuffer)
{
    XCHECK_VOID(pBuffer);

    if (pBuffer->pData != NULL && pBuffer->nSize > 0)
        pBuffer->pData[0] = '\0';
    else if (pBuffer->pData == NULL)
        pBuffer->nSize = 0;

    pBuffer->nStatus = 0;
    pBuffer->nUsed = 0;
}

/* A buffer that borrows storage gives up what it owned, which used to be dropped and leaked.
   Storage borrowed out of its own allocation is left alone, so that the view is not of freed memory. */
static void XByteBuffer_Release(xbyte_buffer_t *pBuffer, const uint8_t *pData)
{
    XCHECK_VOID((pBuffer != NULL));
    if (pBuffer->pData == NULL || !pBuffer->nSize) return;

    uintptr_t nOffset = (uintptr_t)pData - (uintptr_t)pBuffer->pData;
    if (pData != NULL && nOffset < pBuffer->nSize) return;

    free(pBuffer->pData);
    pBuffer->pData = NULL;
    pBuffer->nSize = 0;
}

int XByteBuffer_SetData(xbyte_buffer_t *pBuffer, uint8_t *pData, size_t nSize)
{
    XCHECK(pBuffer, XSTDINV);
    XByteBuffer_Release(pBuffer, pData);
    pBuffer->nStatus = XSTDOK;
    pBuffer->nSize = XSTDNON;
    pBuffer->nUsed = nSize;
    pBuffer->pData = pData;
    return (int)pBuffer->nUsed;
}

int XByteBuffer_Set(xbyte_buffer_t *pBuffer, xbyte_buffer_t *pSrc)
{
    XCHECK(((pBuffer != NULL && pSrc != NULL)), XSTDINV);
    XCHECK_NL((pBuffer != pSrc), (int)pBuffer->nUsed);

    XByteBuffer_Release(pBuffer, pSrc->pData);
    pBuffer->nStatus = XSTDOK;
    pBuffer->nSize = XSTDNON;
    pBuffer->nUsed = pSrc->nUsed;
    pBuffer->pData = pSrc->pData;
    return (int)pBuffer->nUsed;
}

int XByteBuffer_OwnData(xbyte_buffer_t *pBuffer, uint8_t *pData, size_t nSize)
{
    XCHECK(pBuffer, XSTDINV);
    XByteBuffer_Clear(pBuffer);
    XByteBuffer_SetData(pBuffer, pData, nSize);
    pBuffer->nSize = nSize;
    return (int)pBuffer->nSize;
}

int XByteBuffer_Own(xbyte_buffer_t *pBuffer, xbyte_buffer_t *pSrc)
{
    XCHECK(((pBuffer != NULL && pSrc != NULL)), XSTDINV);
    XCHECK_NL((pBuffer != pSrc), (int)pBuffer->nSize);

    XByteBuffer_Clear(pBuffer);
    XByteBuffer_Set(pBuffer, pSrc);
    pBuffer->nSize = pSrc->nSize;

    pSrc->pData = NULL;
    pSrc->nSize = XSTDNON;
    pSrc->nUsed = XSTDNON;
    return (int)pBuffer->nSize;
}

int XByteBuffer_Add(xbyte_buffer_t *pBuffer, const uint8_t *pData, size_t nSize)
{
    XCHECK(((pBuffer != NULL)), XSTDINV);
    if (pData == NULL || !nSize) return XSTDNON;
    if (nSize >= INT_MAX) return pBuffer->nStatus = XSTDERR;

    size_t nStorage = XSTD_MAX(pBuffer->nSize, pBuffer->nUsed);
    uintptr_t nOffset = (uintptr_t)pData - (uintptr_t)pBuffer->pData;
    xbool_t bAlias = pBuffer->pData && nOffset < nStorage;

    if (bAlias && (nOffset >= pBuffer->nUsed || nSize > pBuffer->nUsed - nOffset)) return pBuffer->nStatus = XSTDERR;
    if (XByteBuffer_Reserve(pBuffer, nSize + 1) <= 0) return XSTDERR;
    if (bAlias) pData = pBuffer->pData + nOffset;

    memcpy(&pBuffer->pData[pBuffer->nUsed], pData, nSize);
    pBuffer->nUsed += nSize;
    pBuffer->pData[pBuffer->nUsed] = '\0';

    return (int)pBuffer->nUsed;
}

int XByteBuffer_ReadStdin(xbyte_buffer_t *pBuffer)
{
    XCHECK(((pBuffer != NULL)), XSTDINV);
    char sBuffer[XSTR_BIG];
    size_t nRead = 0;

    while ((nRead = fread(sBuffer, 1, sizeof(sBuffer), stdin)) > 0)
    {
        if (XByteBuffer_Add(pBuffer, (const uint8_t*)sBuffer, nRead) <= 0)
            return XSTDERR;
    }

    /* fread() ends the same way at the end of the input and at a read that failed: only the stream can tell */
    if (ferror(stdin)) return XSTDERR;
    return (int)pBuffer->nUsed;
}

int XByteBuffer_AddStr(xbyte_buffer_t *pBuffer, xstring_t *pStr)
{
    XCHECK(((pBuffer != NULL && pStr != NULL)), XSTDINV);
    const uint8_t *pData = (const uint8_t *)pStr->pData;
    return XByteBuffer_Add(pBuffer, pData, pStr->nLength);
}

int XByteBuffer_AddFmt(xbyte_buffer_t *pBuffer, const char *pFmt, ...)
{
    XCHECK(((pBuffer != NULL && pFmt != NULL)), XSTDINV);

    /* A short result is formatted on the stack and copied in. It is never
       formatted into the buffer itself, an argument may point into it. */
    char sStack[XSTR_MIN];
    va_list args;

    va_start(args, pFmt);
    int nFormatted = vsnprintf(sStack, sizeof(sStack), pFmt, args);
    va_end(args);

    if (nFormatted > 0 && (size_t)nFormatted < sizeof(sStack))
        return XByteBuffer_Add(pBuffer, (const uint8_t*)sStack, (size_t)nFormatted);

    size_t nBytes = 0;
    va_start(args, pFmt);
    char *pDest = xstracpyargs(pFmt, args, &nBytes);
    va_end(args);

    if (pDest == NULL)
    {
        pBuffer->nStatus = XSTDERR;
        return XSTDERR;
    }

    int nStatus = XByteBuffer_Add(pBuffer, (uint8_t*)pDest, nBytes);
    free(pDest);

    return nStatus;
}

int XByteBuffer_AddByte(xbyte_buffer_t *pBuffer, uint8_t nByte)
{
    if (XByteBuffer_Reserve(pBuffer, 2) <= 0) return XSTDERR;
    pBuffer->pData[pBuffer->nUsed++] = nByte;
    pBuffer->pData[pBuffer->nUsed] = '\0';
    return (int)pBuffer->nUsed;
}

uint8_t XByteBuffer_GetByte(xbyte_buffer_t *pBuffer, size_t nIndex)
{
    XCHECK(((pBuffer != NULL)), XSTDINV);
    if (nIndex >= pBuffer->nUsed) return 0;
    return pBuffer->pData[nIndex];
}

int XByteBuffer_NullTerm(xbyte_buffer_t *pBuffer)
{
    if (XByteBuffer_Reserve(pBuffer, 1) <= 0) return XSTDERR;
    pBuffer->pData[pBuffer->nUsed] = '\0';
    return (int)pBuffer->nUsed;
}

int XByteBuffer_AddBuff(xbyte_buffer_t *pBuffer, xbyte_buffer_t *pSrc)
{
    XCHECK(((pBuffer != NULL && pSrc != NULL)), XSTDINV);
    if (pSrc->pData == NULL || !pSrc->nUsed) return XSTDERR;
    int nAdded = XByteBuffer_Add(pBuffer, pSrc->pData, pSrc->nUsed);
    return pBuffer->nStatus = nAdded > 0 ? XSTDOK : XSTDERR;
}

int XByteBuffer_Insert(xbyte_buffer_t *pBuffer, size_t nPosit, const uint8_t *pData, size_t nSize)
{
    XCHECK(((pBuffer != NULL)), XSTDINV);
    if (pData == NULL || !nSize) return XSTDNON;
    if (nSize >= INT_MAX) return pBuffer->nStatus = XSTDERR;
    if (nPosit >= pBuffer->nUsed) return XByteBuffer_Add(pBuffer, pData, nSize);

    uint8_t *pCopy = NULL;
    size_t nStorage = XSTD_MAX(pBuffer->nSize, pBuffer->nUsed);
    uintptr_t nOffset = (uintptr_t)pData - (uintptr_t)pBuffer->pData;

    if (pBuffer->pData && nOffset < nStorage)
    {
        if (nOffset >= pBuffer->nUsed || nSize > pBuffer->nUsed - nOffset)
            return pBuffer->nStatus = XSTDERR;

        pCopy = XByteData_Dup(pData, nSize);
        if (pCopy == NULL) return pBuffer->nStatus = XSTDERR;

        pData = pCopy;
    }

    if (XByteBuffer_Reserve(pBuffer, nSize + 1) <= 0)
    {
        free(pCopy);
        return XSTDERR;
    }

    uint8_t *pOffset = &pBuffer->pData[nPosit];
    size_t nTailSize = pBuffer->nUsed - nPosit;

    memmove(pOffset + nSize, pOffset, nTailSize);
    memcpy(&pBuffer->pData[nPosit], pData, nSize);
    free(pCopy);

    pBuffer->nUsed += nSize;
    pBuffer->pData[pBuffer->nUsed] = '\0';
    return (int)pBuffer->nUsed;
}

int XByteBuffer_Remove(xbyte_buffer_t *pBuffer, size_t nPosit, size_t nSize)
{
    XCHECK(((pBuffer != NULL)), XSTDINV);
    if (!nSize || nPosit >= pBuffer->nUsed) return 0;
    nSize = XSTD_MIN(nSize, pBuffer->nUsed - nPosit);

    size_t nTailOffset = nPosit + nSize;
    if (nTailOffset >= pBuffer->nUsed)
    {
        pBuffer->nUsed = nPosit;
        pBuffer->pData[pBuffer->nUsed] = '\0';
        return (int)nSize;
    }

    size_t nTailSize = pBuffer->nUsed - nTailOffset;
    const uint8_t *pTail = &pBuffer->pData[nTailOffset];
    memmove(&pBuffer->pData[nPosit], pTail, nTailSize);

    pBuffer->nUsed -= nSize;
    pBuffer->pData[pBuffer->nUsed] = '\0';

    return (int)nSize;
}

int XByteBuffer_Delete(xbyte_buffer_t *pBuffer, size_t nPosit, size_t nSize)
{
    XCHECK(((pBuffer != NULL)), XSTDINV);
    XByteBuffer_Remove(pBuffer, nPosit, nSize);

    if (pBuffer->nUsed >= INT_MAX ||
        XByteBuffer_Resize(pBuffer, pBuffer->nUsed + 1) <= 0)
        return XSTDERR;

    pBuffer->pData[pBuffer->nUsed] = '\0';
    return pBuffer->nStatus;
}

int XByteBuffer_Advance(xbyte_buffer_t *pBuffer, size_t nSize)
{
    XCHECK(((pBuffer != NULL)), XSTDINV);
    XByteBuffer_Delete(pBuffer, 0, nSize);
    return (int)pBuffer->nUsed;
}

xbool_t XByteBuffer_HasData(xbyte_buffer_t *pBuffer)
{
    return (pBuffer && pBuffer->pData &&
        pBuffer->nUsed) ? XTRUE : XFALSE;
}

int XDataBuffer_Init(xdata_buffer_t *pBuffer, size_t nSize, int nFixed)
{
    XCHECK(((pBuffer != NULL)), XSTDINV);
    memset(pBuffer, 0, sizeof(*pBuffer));
    if (!nSize && !nFixed) nSize = 1;

    if (!nSize || nSize > INT_MAX ||
        nSize > SIZE_MAX / sizeof(void*)) return XSTDERR;

    pBuffer->pData = (void**)calloc(nSize, sizeof(void*));
    if (pBuffer->pData == NULL)
    {
        pBuffer->nSize = 0;
        return XSTDERR;
    }

    pBuffer->nStatus = 0;
    pBuffer->clearCb = NULL;
    pBuffer->nFixed = nFixed;
    pBuffer->nSize = nSize;
    pBuffer->nUsed = 0;
    return (int)nSize;
}

int XDataBuffer_Realloc(xdata_buffer_t *pBuffer)
{
    XCHECK(((pBuffer != NULL)), XSTDINV);
    if (pBuffer->nFixed) return (int)pBuffer->nSize;

    void* pOldData = pBuffer->pData;
    void *pData = NULL;
    size_t nSize = 0;

    if (pBuffer->nUsed == pBuffer->nSize)
    {
        if (pBuffer->nSize > INT_MAX / 2 || pBuffer->nSize > SIZE_MAX / sizeof(void*) / 2) return XSTDERR;
        nSize = pBuffer->nSize ? pBuffer->nSize * 2 : 1;
        pData = realloc(pBuffer->pData, sizeof(void*) * nSize);
    }
    else if (pBuffer->nUsed > 0 && ((float)pBuffer->nUsed / (float)pBuffer->nSize) < 0.25)
    {
        nSize = pBuffer->nSize / 2;
        pData = realloc(pBuffer->pData, sizeof(void*) * nSize);
    }
    else return (int)pBuffer->nSize;

    if (pData != NULL)
    {
        pBuffer->pData = (void**)pData;
        pBuffer->nSize = nSize;

        size_t i;
        for (i = pBuffer->nUsed; i < nSize; i++)
            pBuffer->pData[i] = NULL;

        return (int)pBuffer->nSize;
    }

    pBuffer->pData = pOldData;
    return XSTDERR;
}

void XDataBuffer_Clear(xdata_buffer_t *pBuffer)
{
    XCHECK_VOID((pBuffer != NULL));
    unsigned int i;

    for (i = 0; i < pBuffer->nSize; i++)
    {
        if (pBuffer->clearCb != NULL)
            pBuffer->clearCb(pBuffer->pData[i]);

        pBuffer->pData[i] = NULL;
    }

    pBuffer->nUsed = 0;
}

void XDataBuffer_Destroy(xdata_buffer_t *pBuffer)
{
    XCHECK_VOID((pBuffer != NULL));
    XDataBuffer_Clear(pBuffer);

    if (pBuffer->pData != NULL)
    {
        free(pBuffer->pData);
        pBuffer->pData = NULL;
    }

    pBuffer->nSize = pBuffer->nUsed = 0;
}

int XDataBuffer_Add(xdata_buffer_t *pBuffer, void *pData)
{
    XCHECK(((pBuffer != NULL)), XSTDINV);

    if (pBuffer->nUsed >= pBuffer->nSize &&
        (XDataBuffer_Realloc(pBuffer) <= 0 ||
        pBuffer->nUsed >= pBuffer->nSize))
        return XSTDERR;

    pBuffer->pData[pBuffer->nUsed++] = pData;
    return (int)pBuffer->nUsed - 1;
}

void* XDataBuffer_Set(xdata_buffer_t *pBuffer, unsigned int nIndex, void *pData)
{
    XCHECK(((pBuffer != NULL)), NULL);
    void *pOldData = NULL;

    if (nIndex < pBuffer->nSize)
    {
        pOldData = pBuffer->pData[nIndex];
        pBuffer->pData[nIndex] = pData;

        if (pBuffer->nUsed <= nIndex)
            pBuffer->nUsed = (size_t)nIndex + 1;
    }

    return pOldData;
}

void* XDataBuffer_Get(xdata_buffer_t *pBuffer, unsigned int nIndex)
{
    XCHECK(((pBuffer != NULL)), NULL);
    if (nIndex >= pBuffer->nUsed) return NULL;
    return pBuffer->pData[nIndex];
}

void* XDataBuffer_Pop(xdata_buffer_t *pBuffer, unsigned int nIndex)
{
    void *pRetVal = XDataBuffer_Get(pBuffer, nIndex);
    if (pRetVal == NULL) return NULL;

    size_t nTail = pBuffer->nUsed - (size_t)nIndex - 1;
    if (nTail) memmove(&pBuffer->pData[nIndex], &pBuffer->pData[nIndex + 1], nTail * sizeof(void*));

    pBuffer->pData[--pBuffer->nUsed] = NULL;
    XDataBuffer_Realloc(pBuffer);
    return pRetVal;
}

int XRingBuffer_Init(xring_buffer_t *pBuffer, size_t nSize)
{
    XCHECK(((pBuffer != NULL)), XSTDINV);
    memset(pBuffer, 0, sizeof(*pBuffer));
    if (!nSize || nSize > INT_MAX || nSize > SIZE_MAX / sizeof(xbyte_buffer_t*)) return 0;

    pBuffer->pData = (xbyte_buffer_t**)calloc(nSize, sizeof(xbyte_buffer_t*));
    if (pBuffer->pData == NULL) return 0;

    pBuffer->nSize = nSize;
    pBuffer->nFront = 0;
    pBuffer->nBack = 0;
    pBuffer->nUsed = 0;
    return (int)pBuffer->nSize;
}

void XRingBuffer_Reset(xring_buffer_t *pBuffer)
{
    XCHECK_VOID((pBuffer != NULL));
    unsigned long i;

    for (i = 0; i < pBuffer->nSize; i++)
    {
        if (pBuffer->pData[i] != NULL)
        {
            XByteBuffer_Clear(pBuffer->pData[i]);
            free(pBuffer->pData[i]);
            pBuffer->pData[i] = NULL;
        }
    }

    pBuffer->nFront = 0;
    pBuffer->nBack = 0;
    pBuffer->nUsed = 0;
}

void XRingBuffer_Destroy(xring_buffer_t *pBuffer)
{
    XCHECK_VOID((pBuffer != NULL));
    XRingBuffer_Reset(pBuffer);
    free(pBuffer->pData);
    pBuffer->pData = NULL;
    pBuffer->nSize = 0;
}

void XRingBuffer_Update(xring_buffer_t *pBuffer, int nAdd)
{
    XCHECK_VOID((pBuffer != NULL));
    if (!pBuffer->nSize) return;

    if (nAdd)
    {
        if (pBuffer->nUsed >= pBuffer->nSize) return;
        pBuffer->nBack++;
        pBuffer->nUsed++;
    }
    else
    {
        if (!pBuffer->nUsed) return;
        pBuffer->nFront++;
        pBuffer->nUsed--;
    }

    if ((size_t)pBuffer->nFront >= pBuffer->nSize) pBuffer->nFront = 0;
    if ((size_t)pBuffer->nBack >= pBuffer->nSize) pBuffer->nBack = 0;

}

void XRingBuffer_Advance(xring_buffer_t *pBuffer)
{
    XCHECK_VOID((pBuffer != NULL));
    if (!pBuffer->nUsed || !pBuffer->nSize) return;

    xbyte_buffer_t *pBuffData = pBuffer->pData[pBuffer->nFront];
    if (pBuffData != NULL) XByteBuffer_Clear(pBuffData);

    XRingBuffer_Update(pBuffer, 0);
}

int XRingBuffer_AddData(xring_buffer_t *pBuffer, const uint8_t* pData, size_t nSize)
{
    XCHECK(((pBuffer != NULL)), XSTDINV);
    if (pBuffer->nUsed >= pBuffer->nSize) return 0;

    xbyte_buffer_t *pBuffData = pBuffer->pData[pBuffer->nBack];
    if (pBuffData == NULL)
    {
        pBuffData = malloc(sizeof(xbyte_buffer_t));
        if (pBuffData == NULL) return 0;
        XByteBuffer_Init(pBuffData, 0, 0);
        pBuffer->pData[pBuffer->nBack] = pBuffData;
    }

    XByteBuffer_Reset(pBuffData);
    int nStatus = XByteBuffer_Add(pBuffData, pData, nSize);
    if (nStatus > 0) XRingBuffer_Update(pBuffer, 1);
    return nStatus;
}

/* Adds an element, making room for it in a full ring by dropping the oldest. The new element is made first and the
   oldest dropped only once it is there: nothing to add, or an add that fails, used to drop it all the same. */
int XRingBuffer_AddDataAdv(xring_buffer_t *pBuffer, const uint8_t* pData, size_t nSize)
{
    XCHECK(((pBuffer != NULL)), XSTDINV);

    if (pBuffer->nUsed < pBuffer->nSize || !pBuffer->nSize)
        return XRingBuffer_AddData(pBuffer, pData, nSize);
    else if (pData == NULL || !nSize) return 0;

    xbyte_buffer_t staged;
    XByteBuffer_Init(&staged, 0, 0);

    int nStatus = XByteBuffer_Add(&staged, pData, nSize);
    if (nStatus <= 0) return nStatus;

    /* In a full ring the slot the next element goes to is the oldest one's */
    XRingBuffer_Advance(pBuffer);
    XByteBuffer_Own(pBuffer->pData[pBuffer->nBack], &staged);
    XRingBuffer_Update(pBuffer, 1);
    return nStatus;
}

int XRingBuffer_GetData(xring_buffer_t *pBuffer, uint8_t** pData, size_t* pSize)
{
    XCHECK(((pBuffer != NULL)), XSTDINV);
    if (!pBuffer->nUsed || !pData || !pSize) return 0;

    xbyte_buffer_t *pBuffData = pBuffer->pData[pBuffer->nFront];
    if (pBuffData == NULL) return 0;

    *pData = pBuffData->pData;
    *pSize = pBuffData->nUsed;

    return 1;
}

int XRingBuffer_Pop(xring_buffer_t *pBuffer, uint8_t* pData, size_t nSize)
{
    XCHECK(((pBuffer != NULL)), XSTDINV);
    size_t nCopySize = 0;

    if (!pBuffer->nUsed || !pData || !nSize) return (int)nCopySize;
    xbyte_buffer_t *pBuffData = pBuffer->pData[pBuffer->nFront];

    if (pBuffData != NULL)
    {
        nCopySize = XSTD_MIN(nSize, pBuffData->nUsed);
        memcpy(pData, pBuffData->pData, nCopySize);
        XByteBuffer_Clear(pBuffData);
    }

    XRingBuffer_Update(pBuffer, 0);
    return (int)nCopySize;
}
