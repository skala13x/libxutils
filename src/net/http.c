/*!
 *  @file libxutils/src/net/http.c
 *
 *  This source is part of the "libxutils" project
 *  2018-2020  Sandro Kalatozishvili (s.kalatoz@gmail.com)
 *
 * @brief This source includes implementation of
 * the HTTP request/response parser and assembler.
 *
 */

#include "xstd.h"
#include "xver.h"
#include "str.h"
#include "http.h"
#include "crypt.h"
#include "base64.h"

/* A chunk size line, its extensions included, is never longer than this */
#define XHTTP_CHUNK_LINE_MAX    XHTTP_HEADER_SIZE

/* Where a chunked body is, byte by byte (RFC 9112, 7.1). Every line ends in CRLF: a lone LF is not
   taken as the end of one, so no reader of the same bytes can see a different end of the body. */
enum {
    XHTTP_CHUNK_START = 0,  /* At a chunk size line */
    XHTTP_CHUNK_SIZE,       /* In its hex digits */
    XHTTP_CHUNK_SIZE_WS,    /* In whitespace after them, before an extension */
    XHTTP_CHUNK_EXT,        /* In the chunk extensions */
    XHTTP_CHUNK_SIZE_LF,    /* At the LF of the size line */
    XHTTP_CHUNK_DATA,       /* In the body bytes of a chunk */
    XHTTP_CHUNK_DATA_CR,    /* At the CRLF that follows them */
    XHTTP_CHUNK_DATA_LF,
    XHTTP_CHUNK_TRAILER,    /* At a trailer field line, or at the empty line that ends the body */
    XHTTP_CHUNK_FIELD,      /* In a trailer field line */
    XHTTP_CHUNK_FIELD_LF,
    XHTTP_CHUNK_LAST_LF,    /* At the LF of the empty line */
    XHTTP_CHUNK_DONE,
    XHTTP_CHUNK_ERROR
};

/* What frames the body, over all of the header's lines: the transfer codings its Transfer-Encoding lines list
   (RFC 9110, 5.3), and the length its Content-Length lines give (RFC 9112, 6.3) */
typedef struct {
    size_t nLines;
    int nChunked;
    xbool_t bLast;
    size_t nLength;             /* The length every Content-Length value gives */
    size_t nLengths;            /* How many values gave it */
    xbool_t bLengthInvalid;     /* One that is not a length, or not the same one */
} xhttp_coding_t;

static void XHTTP_AddCodings(xhttp_coding_t *pCoding, const char *pList, size_t nLength);
static xhttp_framing_t XHTTP_GetFraming(xhttp_t *pHttp, xhttp_coding_t *pCoding, xbool_t *pValid);
typedef struct XHTTPCode {
    const int nCode;
    const char* pDesc;
} xhttp_code_t;

static xhttp_code_t g_XTTPCodes[] =
{
    { 100, "Continue" },
    { 101, "Switching Protocol" },
    { 102, "Processing" },
    { 103, "Early Hints" },
    { 200, "OK" },
    { 201, "Created" },
    { 202, "Accepted" },
    { 203, "Non-Authoritative Information" },
    { 204, "No Content" },
    { 205, "Reset Content" },
    { 206, "Partial Content" },
    { 300, "Multiple Choice" },
    { 301, "Moved Permanently" },
    { 302, "Found" },
    { 303, "See Other" },
    { 304, "Not Modified" },
    { 305, "Use Proxy" },
    { 306, "Unused" },
    { 307, "Temporary Redirect" },
    { 308, "Permanent Redirect" },
    { 400, "Bad Request" },
    { 401, "Unauthorized" },
    { 402, "Payment Required" },
    { 403, "Forbidden" },
    { 404, "Not Found" },
    { 405, "Method Not Allowed" },
    { 406, "Not Acceptable" },
    { 407, "Proxy Authentication Required" },
    { 408, "Request Timeout" },
    { 409, "Conflict" },
    { 410, "Gone" },
    { 411, "Length Required" },
    { 412, "Precondition Failed" },
    { 413, "Payload Too Large" },
    { 414, "URI Too Long" },
    { 415, "Unsupported Media Type" },
    { 416, "Requested Range Not Satisfiable" },
    { 417, "Expectation Failed" },
    { 418, "I'm a teapot" },
    { 500, "Internal Server Error" },
    { 501, "Not Implemented" },
    { 502, "Bad Gateway" },
    { 503, "Service Unavailable" },
    { 504, "Gateway Timeout" },
    { 505, "HTTP Version Not Supported" },
    { 506, "Variant Also Negotiates" },
    { 507, "Insufficient Storage" },
    { 508, "Loop Detected" },
    { 510, "Not Extended" },
    { 511, "Network Authentication Required" },
    { -1, "Unknown "}
};

const char* XHTTP_GetStatusStr(xhttp_status_t eStatus)
{
    switch (eStatus)
    {
        case XHTTP_EINIT:
            return "Failed to init HTTP request";
        case XHTTP_EASSEMBLE:
            return "Failed to assemble HTTP request";
        case XHTTP_ECONNECT:
            return "Failed to connect remote server";
        case XHTTP_ERESOLVE:
            return "Failed to resolve remote address";
        case XHTTP_EAUTH:
            return "Failed to setup auth basic header";
        case XHTTP_ELINK:
            return "Invalid or unsupported address (link)";
        case XHTTP_EPROTO:
            return "Invalid or unsupported protocol in link";
        case XHTTP_EWRITE:
            return "Failed to send request to remote server";
        case XHTTP_EREAD:
            return "Failed to read HTTP packet from the network";
        case XHTTP_ETIMEO:
            return "Failed to set receive timeout on the socket";
        case XHTTP_ESETHDR:
            return "Failed to append header field to the request";
        case XHTTP_EEXISTS:
            return "Header already exists in the HTTP header table";
        case XHTTP_EALLOC:
            return "Failed to allocate memory for HTTP packet buffer";
        case XHTTP_EFDMODE:
            return "Non-blocking file descriptor is not allowed for this operation";
        case XHTTP_BIGHDR:
            return "HTTP header is not detected in the bytes of active limit";
        case XHTTP_BIGCNT:
            return "HTTP Packet payload is greater than the active limit";
        case XHTTP_INCOMPLETE:
            return "Data does not contain HTTP packet or it is incomplete";
        case XHTTP_TERMINATED:
            return "Termination was requested from the HTTP callback";
        case XHTTP_COMPLETE:
            return "Succesfully parsed HTTP packet header and body";
        case XHTTP_PARSED:
            return "Succesfully parsed HTTP packet header";
        case XHTTP_INVALID:
            return "Invalid or unsupported HTTP packet";
        default:
            break;
    }

    return "Unknown status";
}

const char* XHTTP_GetCodeStr(int nCode)
{
    int i;
    for (i = 0;; i++)
    {
        if (g_XTTPCodes[i].nCode == -1 ||
            g_XTTPCodes[i].nCode == nCode)
                return g_XTTPCodes[i].pDesc;
    }

    return "Unknown";
}

const char *XHTTP_GetMethodStr(xhttp_method_t eMethod)
{
    switch(eMethod)
    {
        case XHTTP_PUT: return "PUT";
        case XHTTP_GET: return "GET";
        case XHTTP_POST: return "POST";
        case XHTTP_DELETE: return "DELETE";
        case XHTTP_OPTIONS: return "OPTIONS";
        case XHTTP_DUMMY: return "DUMMY";
        default: break;
    }

    return "UNKNOWN";
}

xhttp_method_t XHTTP_GetMethodType(const char *pData)
{
    if (pData == NULL) return XHTTP_DUMMY;
    if (!strncmp(pData, "GET", 3)) return XHTTP_GET;
    if (!strncmp(pData, "PUT", 3)) return XHTTP_PUT;
    if (!strncmp(pData, "POST", 4)) return XHTTP_POST;
    if (!strncmp(pData, "DELETE", 6)) return XHTTP_DELETE;
    if (!strncmp(pData, "OPTIONS", 7)) return XHTTP_OPTIONS;
    return XHTTP_DUMMY;
}

xbool_t XHTTP_IsSuccessCode(xhttp_t *pHandle)
{
    XCHECK_NL((pHandle != NULL), XFALSE);
    return (pHandle->nStatusCode >= 200 &&
            pHandle->nStatusCode < 300) ?
            XTRUE : XFALSE;
}

static int XHTTP_Callback(xhttp_t *pHandle, xhttp_cbtype_t eType, const void *pData, size_t nLength)
{
    if (pData == NULL || !nLength) return XSTDERR;
    else if (pHandle->callback == NULL) return XSTDUSR;
    else if (!XHTTP_XCHECK_FLAG(pHandle->nCbTypes, eType)) return XSTDUSR;

    xhttp_ctx_t cbCtx;
    cbCtx.pData = pData;
    cbCtx.nLength = nLength;
    cbCtx.eCbType = eType;
    cbCtx.eStatus = XHTTP_NONE;
    return pHandle->callback(pHandle, &cbCtx);
}

static xhttp_status_t XHTTP_StatusCb(xhttp_t *pHandle, xhttp_status_t eStatus)
{
    xhttp_cbtype_t eType = eStatus < XHTTP_TERMINATED ? XHTTP_ERROR : XHTTP_STATUS;
    if (pHandle != NULL && XHTTP_XCHECK_FLAG(pHandle->nCbTypes, eType) && pHandle->callback != NULL)
    {
        char sBuffer[XHTTP_OPTION_MAX];
        const char *pStr = XHTTP_GetStatusStr(eStatus);
        size_t nLength = xstrncpyf(sBuffer, sizeof(sBuffer), "%s", pStr);

        xhttp_ctx_t cbCtx;
        cbCtx.eCbType = eType;
        cbCtx.eStatus = eStatus;
        cbCtx.nLength = nLength;
        cbCtx.pData = sBuffer;

        if (pHandle->callback(pHandle, &cbCtx) < 0)
            eStatus = XHTTP_TERMINATED;
    }

    return eStatus;
}

static void XHTTP_HeaderClearCb(xmap_pair_t *pPair)
{
    if (pPair != NULL)
    {
        if (pPair->pData != NULL) free(pPair->pData);
        if (pPair->pKey != NULL) free(pPair->pKey);
    }
}

/* Where an assembled message goes. Without data it only counts, so the same pass sizes a message and then
   writes it, and the buffer is grown once for the whole of it instead of once for every line. */
typedef struct {
    uint8_t *pData;
    size_t nLength;
} xhttp_writer_t;

static void XHTTP_Put(xhttp_writer_t *pWriter, const char *pStr, size_t nLength)
{
    if (pWriter->pData != NULL) memcpy(pWriter->pData + pWriter->nLength, pStr, nLength);
    pWriter->nLength += nLength;
}

static int XHTTP_HeaderWriteCb(xmap_pair_t *pPair, void *pContext)
{
    xhttp_writer_t *pWriter = (xhttp_writer_t*)pContext;
    const char *pHeader = (const char *)pPair->pKey;
    const char *pValue = (const char *)pPair->pData;

    XHTTP_Put(pWriter, pHeader, strlen(pHeader));
    XHTTP_Put(pWriter, ": ", 2);
    XHTTP_Put(pWriter, pValue, strlen(pValue));
    XHTTP_Put(pWriter, "\r\n", 2);
    return XMAP_OK;
}

/* The start line, the header fields and the empty line after them */
static void XHTTP_WriteHeader(xhttp_t *pHttp, xhttp_writer_t *pWriter)
{
    if (pHttp->eType == XHTTP_REQUEST)
    {
        const char *pMethod = XHTTP_GetMethodStr(pHttp->eMethod);
        XHTTP_Put(pWriter, pMethod, strlen(pMethod));
        XHTTP_Put(pWriter, " ", 1);
        XHTTP_Put(pWriter, pHttp->sUri, strlen(pHttp->sUri));
        XHTTP_Put(pWriter, " HTTP/", 6);
        XHTTP_Put(pWriter, pHttp->sVersion, strlen(pHttp->sVersion));
        XHTTP_Put(pWriter, "\r\n", 2);
    }
    else if (pHttp->eType == XHTTP_RESPONSE)
    {
        const char *pCodeStr = XHTTP_GetCodeStr(pHttp->nStatusCode);
        char sCode[8];

        int nCode = snprintf(sCode, sizeof(sCode), "%d", pHttp->nStatusCode);
        XHTTP_Put(pWriter, "HTTP/", 5);
        XHTTP_Put(pWriter, pHttp->sVersion, strlen(pHttp->sVersion));
        XHTTP_Put(pWriter, " ", 1);
        XHTTP_Put(pWriter, sCode, (size_t)nCode);
        XHTTP_Put(pWriter, " ", 1);
        XHTTP_Put(pWriter, pCodeStr, strlen(pCodeStr));
        XHTTP_Put(pWriter, "\r\n", 2);
    }

    if (pHttp->headerMap.nCount > 0) XMap_Iterate(&pHttp->headerMap, XHTTP_HeaderWriteCb, pWriter);
    XHTTP_Put(pWriter, "\r\n", 2);
}

static int XHTTP_MapIt(xmap_pair_t *pPair, void *pContext)
{
    xmap_t *pDstMap = (xmap_t*)pContext;

    /* The destination frees its own entries, so it has to own them. Storing
       the source's pointers left the copy holding freed strings the moment
       the source was cleared, and freed each of them twice after that. */
    char *pKey = xstrdup((const char*)pPair->pKey);
    char *pValue = xstrdup((const char*)pPair->pData);

    if (pKey == NULL || pValue == NULL ||
        XMap_Put(pDstMap, pKey, pValue) != XMAP_OK)
    {
        free(pValue);
        free(pKey);
        return XMAP_STOP;
    }

    return XMAP_OK;
}

int XHTTP_SetCallback(xhttp_t *pHttp, xhttp_cb_t callback, void *pCbCtx, uint16_t nCbTypes)
{
    if (pHttp == NULL) return XSTDERR;
    pHttp->callback = callback;
    pHttp->nCbTypes = nCbTypes;
    pHttp->pUserCtx = pCbCtx;
    return XSTDOK;
}

int XHTTP_Init(xhttp_t *pHttp, xhttp_method_t eMethod, size_t nSize)
{
    XCHECK_NL((pHttp != NULL), XSTDERR);
    pHttp->nHeaderLength = pHttp->nContentLength = 0;
    pHttp->nStatusCode = pHttp->nHeaderCount = 0;
    pHttp->nAllocated = pHttp->nComplete = 0;
    pHttp->sVersion[0] = pHttp->sUri[0] = '\0';
    pHttp->sUnixAddr[0] = '\0';

    pHttp->callback = NULL;
    pHttp->pUserCtx = NULL;
    pHttp->nAllowUpdate = 0;
    pHttp->nKeepAlive = 0;
    pHttp->nCbTypes = 0;
    pHttp->nTimeout = 0;

    pHttp->nContentMax = XHTTP_PACKAGE_MAX;
    pHttp->nHeaderMax = XHTTP_HEADER_MAX;

    pHttp->eMethod = eMethod;
    pHttp->eType = XHTTP_INITIAL;

    pHttp->eFraming = XHTTP_FRAME_NONE;
    XHTTP_InitChunked(&pHttp->chunked);

    XByteBuffer_Init(&pHttp->bodyData, XSTDNON, XFALSE);
    pHttp->nBodyClaimed = 0;

    XMap_Init(&pHttp->headerMap, NULL, 0);
    pHttp->headerMap.clearCb = XHTTP_HeaderClearCb;

    xbyte_buffer_t *pBuffer = &pHttp->rawData;
    return XByteBuffer_Init(pBuffer, nSize, XSTDNON);
}

size_t XHTTP_SetUnixAddr(xhttp_t *pHttp, const char *pUnixAddr)
{
    XCHECK_NL((pHttp && xstrused(pUnixAddr)), XSTDERR);
    return xstrncpy(pHttp->sUnixAddr, sizeof(pHttp->sUnixAddr), pUnixAddr);
}

int XHTTP_InitRequest(xhttp_t *pHttp, xhttp_method_t eMethod, const char *pUri, const char *pVer)
{
    int nStatus = XHTTP_Init(pHttp, eMethod, XHTTP_HEADER_SIZE);
    if (nStatus <= 0) return nStatus;

    const char *pVersion = pVer != NULL ? pVer : XHTTP_VER_DEFAULT;
    const char *pFixedUrl = pUri != NULL ? pUri : "\\";

    xstrncpy(pHttp->sVersion, sizeof(pHttp->sVersion), pVersion);
    xstrncpy(pHttp->sUri, sizeof(pHttp->sUri), pFixedUrl);

    pHttp->eType = XHTTP_REQUEST;
    return nStatus;
}

int XHTTP_InitResponse(xhttp_t *pHttp, uint16_t nStatusCode, const char *pVer)
{
    int nStatus = XHTTP_Init(pHttp, XHTTP_DUMMY, XHTTP_HEADER_SIZE);
    if (nStatus <= 0) return XSTDERR;

    const char *pVersion = pVer != NULL ? pVer : XHTTP_VER_DEFAULT;
    xstrncpy(pHttp->sVersion, sizeof(pHttp->sVersion), pVersion);

    pHttp->nStatusCode = nStatusCode;
    pHttp->eType = XHTTP_RESPONSE;
    return nStatus;
}

void XHTTP_Reset(xhttp_t *pHttp, xbool_t bHard)
{
    XCHECK_VOID_NL((pHttp != NULL));
    if (bHard)
    {
        XByteBuffer_Clear(&pHttp->rawData);
        XByteBuffer_Init(&pHttp->rawData, XSTDNON, XFALSE);

        XMap_Destroy(&pHttp->headerMap);
        XMap_Init(&pHttp->headerMap, NULL, XSTDNON);
        pHttp->headerMap.clearCb = XHTTP_HeaderClearCb;
    }
    else
    {
        XByteBuffer_Reset(&pHttp->rawData);
        XMap_Reset(&pHttp->headerMap);
    }

    pHttp->nContentLength = 0;
    pHttp->nHeaderLength = 0;
    pHttp->nHeaderCount = 0;
    pHttp->nStatusCode = 0;
    pHttp->nComplete = 0;
    pHttp->nBodyClaimed = 0;
    pHttp->sUri[0] = '\0';

    pHttp->eMethod = XHTTP_DUMMY;
    pHttp->eType = XHTTP_INITIAL;
    pHttp->eFraming = XHTTP_FRAME_NONE;

    XHTTP_InitChunked(&pHttp->chunked);
    XByteBuffer_Clear(&pHttp->bodyData);
}

xhttp_t *XHTTP_Alloc(xhttp_method_t eMethod, size_t nDataSize)
{
    xhttp_t *pHeader = (xhttp_t*)malloc(sizeof(xhttp_t));
    if (pHeader == NULL) return NULL;

    XHTTP_Init(pHeader, eMethod, nDataSize);
    xbyte_buffer_t *pBuffer = &pHeader->rawData;

    if (pBuffer->nStatus >= 0)
    {
        pHeader->nAllocated = XTRUE;
        return pHeader;
    }

    free(pHeader);
    return NULL;
}

int XHTTP_Copy(xhttp_t *pDst, xhttp_t *pSrc)
{
    if (pDst == NULL || pSrc == NULL) return XSTDERR;
    XHTTP_Init(pDst, pSrc->eMethod, XSTDNON);

    xbyte_buffer_t *pSrcBuff = &pSrc->rawData;
    xbyte_buffer_t *pDstBuff = &pDst->rawData;

    if (pSrcBuff->nUsed && XByteBuffer_Add(pDstBuff, pSrcBuff->pData, pSrcBuff->nUsed) < 0) return XSTDERR;
    xstrncpy(pDst->sUnixAddr, sizeof(pDst->sUnixAddr), pSrc->sUnixAddr);
    xstrncpy(pDst->sVersion, sizeof(pDst->sVersion), pSrc->sVersion);
    xstrncpy(pDst->sUri, sizeof(pDst->sUri), pSrc->sUri);

    xmap_t *pSrcMap = &pSrc->headerMap;
    xmap_t *pDstMap = &pDst->headerMap;
    pDstMap->clearCb = XHTTP_HeaderClearCb;

    if (XMap_Iterate(pSrcMap, XHTTP_MapIt, pDstMap) != XMAP_OK)
    {
        XHTTP_Clear(pDst);
        return XSTDERR;
    }

    pDst->nHeaderCount = (uint16_t)pDstMap->nCount;
    pDst->nContentLength = pSrc->nContentLength;
    pDst->nHeaderLength = pSrc->nHeaderLength;

    pDst->pUserCtx = pSrc->pUserCtx;
    pDst->callback = pSrc->callback;
    pDst->nCbTypes = pSrc->nCbTypes;

    pDst->nAllowUpdate = pSrc->nAllowUpdate;
    pDst->nContentMax = pSrc->nContentMax;
    pDst->nHeaderMax = pSrc->nHeaderMax;
    pDst->nStatusCode = pSrc->nStatusCode;
    pDst->nComplete = pSrc->nComplete;
    pDst->nTimeout = pSrc->nTimeout;
    pDst->eType = pSrc->eType;

    pDst->nBodyClaimed = pSrc->nBodyClaimed;
    pDst->eFraming = pSrc->eFraming;
    pDst->chunked = pSrc->chunked;

    /* A decoded body is the copy's own as well; the reserve keeps an empty one told apart from none */
    const xbyte_buffer_t *pBody = &pSrc->bodyData;
    if (pBody->pData != NULL &&
        (XByteBuffer_Reserve(&pDst->bodyData, pBody->nUsed + 1) <= 0 ||
        (pBody->nUsed && XByteBuffer_Add(&pDst->bodyData, pBody->pData, pBody->nUsed) <= 0)))
    {
        XHTTP_Clear(pDst);
        return XSTDERR;
    }

    return XSTDOK;
}

void XHTTP_Clear(xhttp_t *pHttp)
{
    XCHECK_VOID_NL((pHttp != NULL));
    pHttp->headerMap.clearCb = XHTTP_HeaderClearCb;
    pHttp->nComplete = XFALSE;

    XMap_Destroy(&pHttp->headerMap);
    XByteBuffer_Clear(&pHttp->rawData);
    XByteBuffer_Clear(&pHttp->bodyData);
}

void XHTTP_Free(xhttp_t **pHttp)
{
    XCHECK_VOID_NL((pHttp && *pHttp));
    xhttp_t *pHandle = *pHttp;

    XHTTP_Clear(pHandle);
    if (pHandle->nAllocated)
    {
        free(pHandle);
        *pHttp = NULL;
    }
}

static xmap_pair_t* XHTTP_FindHeader(xhttp_t *pHttp, const char *pHeader)
{
    if (pHttp == NULL || pHeader == NULL) return NULL;
    xmap_t *pMap = &pHttp->headerMap;
    xmap_pair_t *pPair = XMap_GetPair(pMap, pHeader);
    if (pPair != NULL || pMap->pPairs == NULL) return pPair;

    /* Preserve the caller's spelling on the wire while matching HTTP field
       names without case sensitivity, including builder-created headers. */
    size_t nHeaderSize = strlen(pHeader) + 1;
    for (uint32_t i = 0; i < pMap->nTableSize; i++)
    {
        pPair = &pMap->pPairs[i];
        if (pPair->eStatus == XMAP_PAIR_USED && xstrncasecmp(pPair->pKey, pHeader, nHeaderSize)) return pPair;
    }
    return NULL;
}

static int XHTTP_PutHeader(xhttp_t *pHttp, const char *pHeader, const char *pValue, size_t nLength)
{
    xmap_pair_t *pPair = XHTTP_FindHeader(pHttp, pHeader);
    int nStatus = 0;

    if (pPair != NULL)
    {
        /* Setting a header to the value it already holds changes nothing,
           so it succeeds without needing update permission. Anything else
           is a real overwrite and stays behind nAllowUpdate. */
        size_t nCurrent = strlen((const char*)pPair->pData);
        if (xstrncmpn((const char*)pPair->pData, nCurrent, pValue, nLength))
        {
            nStatus = (int)pHttp->headerMap.nCount;
            return nStatus > 0 ? nStatus : XSTDERR;
        }

        XCHECK(pHttp->nAllowUpdate, XSTDEXC);
        char *pUpdated = xstrdup(pValue);
        XCHECK(pUpdated, XSTDERR);

        free(pPair->pData);
        pPair->pData = pUpdated;
    }
    else
    {
        char *pCopy = xstrdup(pValue);
        XCHECK(pCopy, XSTDERR);

        char *pKey = xstrdup(pHeader);
        XCHECK_CALL(pKey, free, pCopy, XSTDERR);

        nStatus = XMap_Put(&pHttp->headerMap, pKey, pCopy);
        if (nStatus != XMAP_OK)
        {
            free(pCopy);
            free(pKey);
            return XSTDERR;
        }
    }

    return XSTDOK;
}

int XHTTP_AddHeader(xhttp_t *pHttp, const char *pHeader, const char *pStr, ...)
{
    XCHECK_NL((pHttp && xstrused(pHeader) && pStr), XSTDERR);
    XCHECK_NL((strpbrk(pHeader, "\r\n:") == NULL), XSTDERR);

    char sOption[XHTTP_OPTION_MAX];
    const char *pOption = sOption;
    char *pAllocated = NULL;
    size_t nLength = 0;
    int nStatus = 0;

    va_list args, argsCopy;
    va_start(args, pStr);
    va_copy(argsCopy, args);
    nLength = xstrncpyarg(sOption, sizeof(sOption), pStr, args);
    va_end(args);

    /*
        vsnprintf() truncates instead of failing, so a value that filled the
        stack buffer is formatted again into an allocation. Header values are
        short in the common case and never leave the buffer; the ones that do
        are the ones that matter, since an asymmetrically signed bearer token
        runs past a kilobyte and a clipped Authorization header reaches the
        server as a corrupt credential rather than a reportable error.
    */
    if (nLength >= sizeof(sOption) - 1)
    {
        size_t nFullLength = 0;
        pAllocated = xstracpyargs(pStr, argsCopy, &nFullLength);

        if (pAllocated != NULL)
        {
            pOption = pAllocated;
            nLength = nFullLength;
        }
    }

    va_end(argsCopy);
    if (nLength >= sizeof(sOption) - 1 && pAllocated == NULL) return XSTDERR;

    if (nLength)
    {
        if (strpbrk(pOption, "\r\n") != NULL)
        {
            free(pAllocated);
            return XSTDERR;
        }

        nStatus = XHTTP_PutHeader(pHttp, pHeader, pOption, nLength);
        if (nStatus < 0)
        {
            free(pAllocated);
            return nStatus;
        }
    }

    free(pAllocated);
    nStatus = (int)pHttp->headerMap.nCount;
    XCHECK_NL((nStatus > 0), XSTDERR);

    pHttp->nComplete = XFALSE;
    return nStatus;
}

size_t XHTTP_GetAuthToken(char *pToken, size_t nSize, const char *pUser, const char *pPass)
{
    XCHECK_NL((pToken && nSize && pUser && pPass), XSTDNON);
    pToken[0] = '\0';

    char *pPlain = xstracpy("%s:%s", pUser, pPass);
    if (pPlain == NULL) return XSTDNON;

    size_t nLength = strlen(pPlain);
    char *pEncodedToken = XBase64_Encrypt((const uint8_t*)pPlain, &nLength);
    free(pPlain);
    if (pEncodedToken == NULL) return XSTDNON;

    size_t nDstBytes = nLength < nSize ? xstrncpy(pToken, nSize, pEncodedToken) : 0;
    free(pEncodedToken);
    return nDstBytes;
}

int XHTTP_SetAuthBasic(xhttp_t *pHttp, const char *pUser, const char *pPwd)
{
    XCHECK_NL((pHttp != NULL), XSTDERR);
    if (!xstrused(pUser) || !xstrused(pPwd)) return XSTDNON;
    xbool_t nAllowUpdate = pHttp->nAllowUpdate;
    int nStatus = 0;

    char *pPlain = xstracpy("%s:%s", pUser, pPwd);
    if (pPlain == NULL) return XSTDERR;

    size_t nLength = strlen(pPlain);
    char *pEncodedToken = XBase64_Encrypt((const uint8_t*)pPlain, &nLength);
    free(pPlain);
    if (pEncodedToken == NULL) return XSTDERR;

    pHttp->nAllowUpdate = XTRUE;
    nStatus = XHTTP_AddHeader(pHttp, "Authorization", "Basic %s", pEncodedToken);

    pHttp->nAllowUpdate = nAllowUpdate;
    free(pEncodedToken);

    return nStatus;
}

/* The fields an assembled message carries for what it was handed: the length of a body, and the keep-alive the
   handle asks for. They go in the header map, so that the handle describes the message it was assembled into. */
static xhttp_status_t XHTTP_AddFraming(xhttp_t *pHttp, const uint8_t *pContent, size_t nLength)
{
    xbool_t nAllowUpdate = pHttp->nAllowUpdate;
    int nStatus = XSTDOK;

    pHttp->nAllowUpdate = XTRUE;
    if (pContent != NULL) nStatus = XHTTP_AddHeader(pHttp, "Content-Length", "%zu", nLength);

    if (nStatus > 0 && pHttp->nKeepAlive && XHTTP_GetHeader(pHttp, "Connection") == NULL)
        nStatus = XHTTP_AddHeader(pHttp, "Connection", "keep-alive");

    pHttp->nAllowUpdate = nAllowUpdate;
    return nStatus > 0 ? XHTTP_NONE : XHTTP_EALLOC;
}

/* Writes the message at the end of pBuffer, growing it once at most, and leaves pBuffer as it was on failure */
static xhttp_status_t XHTTP_WriteMessage(xhttp_t *pHttp, xbyte_buffer_t *pBuffer,
                                         const uint8_t *pContent, size_t nLength, size_t *pHeaderLength)
{
    /* A body in the buffer's content moves if the buffer grows. One in the rest of
       its storage, past nUsed, is where the message is written, and is refused. */
    size_t nStorage = XSTD_MAX(pBuffer->nSize, pBuffer->nUsed);
    uintptr_t nOffset = (uintptr_t)pContent - (uintptr_t)pBuffer->pData;
    xbool_t bAlias = nLength && pBuffer->pData != NULL && nOffset < nStorage;
    XCHECK((!bAlias || (nOffset < pBuffer->nUsed && nLength <= pBuffer->nUsed - nOffset)), XHTTP_INVALID);

    xhttp_writer_t writer = { NULL, 0 };
    XHTTP_WriteHeader(pHttp, &writer);
    size_t nHeaderLength = writer.nLength;

    /* The byte buffer holds at most INT_MAX bytes and keeps them terminated, one byte past the message */
    size_t nRoom = pBuffer->nUsed < (size_t)INT_MAX ? (size_t)INT_MAX - pBuffer->nUsed : 0;
    XCHECK((nHeaderLength < nRoom && nLength < nRoom - nHeaderLength), XHTTP_BIGCNT);

    XCHECK((XByteBuffer_Reserve(pBuffer, nHeaderLength + nLength + 1) > 0), XHTTP_EALLOC);
    if (bAlias) pContent = pBuffer->pData + nOffset;

    writer.pData = pBuffer->pData + pBuffer->nUsed;
    writer.nLength = 0;

    XHTTP_WriteHeader(pHttp, &writer);
    if (nLength) memcpy(writer.pData + nHeaderLength, pContent, nLength);

    pBuffer->nUsed += nHeaderLength + nLength;
    pBuffer->pData[pBuffer->nUsed] = '\0';

    if (pHeaderLength != NULL) *pHeaderLength = nHeaderLength;
    return XHTTP_NONE;
}

xhttp_status_t XHTTP_WriteTo(xhttp_t *pHttp, xbyte_buffer_t *pBuffer, const uint8_t *pContent, size_t nLength)
{
    XCHECK((pHttp != NULL && pBuffer != NULL), XHTTP_INVALID);
    nLength = (pContent != NULL) ? nLength : 0;

    xhttp_status_t eStatus = XHTTP_AddFraming(pHttp, pContent, nLength);
    XCHECK_NL((eStatus == XHTTP_NONE), eStatus);

    return XHTTP_WriteMessage(pHttp, pBuffer, pContent, nLength, NULL);
}

xbyte_buffer_t* XHTTP_Assemble(xhttp_t *pHttp, const uint8_t *pContent, size_t nLength)
{
    XCHECK_NL((pHttp != NULL), NULL);
    if (pHttp->nComplete) return &pHttp->rawData;
    nLength = (pContent != NULL) ? nLength : 0;

    xbyte_buffer_t *pBuffer = &pHttp->rawData;
    pHttp->nHeaderLength = 0;
    pHttp->nHeaderCount = 0;

    /* The message is written over what the handle held, which keeps its allocation, unless the body is taken from
       there: the message then goes to new storage, and the old one is released once the body was copied out */
    uintptr_t nOffset = (uintptr_t)pContent - (uintptr_t)pBuffer->pData;

    xbyte_buffer_t previous;
    XByteBuffer_Init(&previous, XSTDNON, XFALSE);

    if (pBuffer->nSize > 0 && (!nLength || nOffset >= pBuffer->nSize))
    {
        /* Just reset existing buffer */
        XByteBuffer_Reset(pBuffer);
    }
    else
    {
        previous = *pBuffer;
        XByteBuffer_Init(pBuffer, XSTDNON, previous.nFast);
    }

    size_t nHeaderLength = 0;
    xhttp_status_t eStatus = XHTTP_AddFraming(pHttp, pContent, nLength);
    if (eStatus == XHTTP_NONE) eStatus = XHTTP_WriteMessage(pHttp, pBuffer, pContent, nLength, &nHeaderLength);

    XByteBuffer_Clear(&previous);
    XCHECK_NL((eStatus == XHTTP_NONE), NULL);

    pHttp->nHeaderLength = nHeaderLength;
    pHttp->nHeaderCount = (uint16_t)pHttp->headerMap.nCount;

    /* The framing the assembled header announces */
    const char *pEncoding = XHTTP_GetHeader(pHttp, "Transfer-Encoding");
    xhttp_coding_t coding;
    xbool_t bValid = XFALSE;

    memset(&coding, 0, sizeof(coding));
    if (pEncoding != NULL) XHTTP_AddCodings(&coding, pEncoding, strlen(pEncoding));
    pHttp->eFraming = XHTTP_GetFraming(pHttp, &coding, &bValid);

    XHTTP_InitChunked(&pHttp->chunked);
    XByteBuffer_Clear(&pHttp->bodyData);
    pHttp->nBodyClaimed = 0;

    pHttp->nContentLength = nLength;
    pHttp->nComplete = 1;
    return pBuffer;
}

const char* XHTTP_GetHeader(xhttp_t *pHttp, const char* pHeader)
{
    xmap_pair_t *pPair = XHTTP_FindHeader(pHttp, pHeader);
    return pPair != NULL ? (const char*)pPair->pData : NULL;
}

char* XHTTP_GetHeaderRaw(xhttp_t *pHttp)
{
    if (pHttp == NULL || !pHttp->nHeaderLength) return NULL;
    else if (pHttp->rawData.nUsed < pHttp->nHeaderLength) return NULL;

    char *pHeaderStr = (char*)malloc(pHttp->nHeaderLength + 1);
    if (pHeaderStr == NULL) return NULL;

    size_t nLength = xstrncpys(
        pHeaderStr,
        pHttp->nHeaderLength + 1,
        (char*)pHttp->rawData.pData,
        pHttp->nHeaderLength
    );

    if (!nLength)
    {
        free(pHeaderStr);
        return NULL;
    }

    return pHeaderStr;
}

const uint8_t* XHTTP_GetBody(xhttp_t *pHttp)
{
    XCHECK_NL((pHttp && pHttp->nHeaderLength), NULL);
    xbyte_buffer_t *pBuffer = &pHttp->rawData;

    /* A chunked body decoded apart from the bytes as they arrived */
    if (pHttp->bodyData.pData != NULL)
        return pHttp->bodyData.nUsed ? pHttp->bodyData.pData : NULL;

    XCHECK_NL((pBuffer->nUsed > pHttp->nHeaderLength), NULL);
    return &pBuffer->pData[pHttp->nHeaderLength];
}

size_t XHTTP_GetBodySize(xhttp_t *pHttp)
{
    XCHECK_NL((pHttp && pHttp->nHeaderLength), XSTDNON);
    const xbyte_buffer_t *pBuffer = &pHttp->rawData;
    if (pHttp->bodyData.pData != NULL) return pHttp->bodyData.nUsed;

    XCHECK_NL((pBuffer->nUsed > pHttp->nHeaderLength), XSTDNON);
    return pBuffer->nUsed - pHttp->nHeaderLength;
}

size_t XHTTP_GetExtraSize(xhttp_t *pHttp)
{
    XCHECK_NL((pHttp != NULL && pHttp->nHeaderLength), XSTDNON);

    /* A body decoded apart left the chunks where they arrived, and the next message after them */
    if (pHttp->bodyData.pData != NULL)
    {
        size_t nPacket = pHttp->nHeaderLength + pHttp->chunked.nPosit;
        return pHttp->rawData.nUsed > nPacket ? pHttp->rawData.nUsed - nPacket : XSTDNON;
    }

    size_t nPayloadSize = XHTTP_GetBodySize(pHttp);
    XCHECK_NL(nPayloadSize, XSTDNON);

    /* A message without a body leaves everything after its header to the next one, and a body that ends with the
       connection, or a chunked one not over yet, has nothing after it. The rest end where nContentLength does. */
    if (pHttp->eFraming == XHTTP_FRAME_NONE) return nPayloadSize;
    else if (pHttp->eFraming == XHTTP_FRAME_CLOSE) return XSTDNON;
    else if (pHttp->eFraming == XHTTP_FRAME_CHUNKED &&
             pHttp->chunked.nState != XHTTP_CHUNK_DONE) return XSTDNON;

    XCHECK_NL((nPayloadSize > pHttp->nContentLength), XSTDNON);
    return nPayloadSize - pHttp->nContentLength;
}

const uint8_t* XHTTP_GetExtraData(xhttp_t *pHttp)
{
    size_t nExtraSize = XHTTP_GetExtraSize(pHttp);
    XCHECK_NL(nExtraSize, NULL);

    size_t nBodyLen = pHttp->bodyData.pData != NULL ? pHttp->chunked.nPosit : pHttp->nContentLength;
    size_t nPacketLen = pHttp->nHeaderLength + nBodyLen;
    const xbyte_buffer_t *pBuffer = &pHttp->rawData;

    XCHECK_NL((pBuffer->nUsed > nPacketLen), NULL);
    return &pBuffer->pData[nPacketLen];
}

size_t XHTTP_GetPacketSize(xhttp_t *pHttp)
{
    XCHECK_NL((pHttp != NULL), XSTDNON);
    const xbyte_buffer_t *pBuffer = &pHttp->rawData;
    return pBuffer->nUsed - XHTTP_GetExtraSize(pHttp);
}

static int XHTTP_HexDigit(uint8_t nByte)
{
    if (nByte >= '0' && nByte <= '9') return nByte - '0';
    if (nByte >= 'a' && nByte <= 'f') return nByte - 'a' + 10;
    if (nByte >= 'A' && nByte <= 'F') return nByte - 'A' + 10;
    return XSTDERR;
}

/* Takes one byte of a chunk line. A size line is bounded on its own, the trailer section as a whole. */
static int XHTTP_ChunkByte(xhttp_chunked_t *pChunked, uint8_t nByte)
{
    int nDigit = XHTTP_HexDigit(nByte);
    size_t nLimit = XHTTP_CHUNK_LINE_MAX;

    /* Control bytes never belong in a line, other than a tab and the CR that ends it */
    xbool_t bControl = (nByte < 0x20 && nByte != '\t' && nByte != '\r') || nByte == 0x7f;

    /* The CR that ends a size line is not part of it */
    if (nByte == '\r' &&
        (pChunked->nState == XHTTP_CHUNK_SIZE ||
         pChunked->nState == XHTTP_CHUNK_SIZE_WS ||
         pChunked->nState == XHTTP_CHUNK_EXT))
    {
        pChunked->nState = XHTTP_CHUNK_SIZE_LF;
        return XSTDOK;
    }

    switch (pChunked->nState)
    {
        case XHTTP_CHUNK_START:
            if (nDigit < 0) return XSTDERR;
            pChunked->nLeft = (size_t)nDigit;
            pChunked->nState = XHTTP_CHUNK_SIZE;
            pChunked->nLine = 0;
            break;
        case XHTTP_CHUNK_SIZE:
            if (nDigit >= 0)
            {
                if (pChunked->nLeft > (SIZE_MAX >> 4)) return XSTDERR;
                pChunked->nLeft = (pChunked->nLeft << 4) | (size_t)nDigit;
            }
            else if (nByte == ';') pChunked->nState = XHTTP_CHUNK_EXT;
            else if (nByte == ' ' || nByte == '\t') pChunked->nState = XHTTP_CHUNK_SIZE_WS;
            else return XSTDERR;
            break;
        case XHTTP_CHUNK_SIZE_WS:
            if (nByte == ';') pChunked->nState = XHTTP_CHUNK_EXT;
            else if (nByte != ' ' && nByte != '\t') return XSTDERR;
            break;
        case XHTTP_CHUNK_EXT:
            if (bControl) return XSTDERR;
            break;
        case XHTTP_CHUNK_SIZE_LF:
            if (nByte != '\n') return XSTDERR;
            pChunked->nState = pChunked->nLeft ? XHTTP_CHUNK_DATA : XHTTP_CHUNK_TRAILER;
            pChunked->nLine = 0;
            return XSTDOK;
        case XHTTP_CHUNK_DATA_CR:
            if (nByte != '\r') return XSTDERR;
            pChunked->nState = XHTTP_CHUNK_DATA_LF;
            return XSTDOK;
        case XHTTP_CHUNK_DATA_LF:
            if (nByte != '\n') return XSTDERR;
            pChunked->nState = XHTTP_CHUNK_START;
            return XSTDOK;
        case XHTTP_CHUNK_TRAILER:
            if (bControl) return XSTDERR;
            pChunked->nState = nByte == '\r' ? XHTTP_CHUNK_LAST_LF : XHTTP_CHUNK_FIELD;
            nLimit = XHTTP_HEADER_MAX;
            break;
        case XHTTP_CHUNK_FIELD:
            if (bControl) return XSTDERR;
            if (nByte == '\r') pChunked->nState = XHTTP_CHUNK_FIELD_LF;
            nLimit = XHTTP_HEADER_MAX;
            break;
        case XHTTP_CHUNK_FIELD_LF:
            if (nByte != '\n') return XSTDERR;
            pChunked->nState = XHTTP_CHUNK_TRAILER;
            nLimit = XHTTP_HEADER_MAX;
            break;
        case XHTTP_CHUNK_LAST_LF:
            if (nByte != '\n') return XSTDERR;
            pChunked->nState = XHTTP_CHUNK_DONE;
            return XSTDOK;
        default:
            return XSTDERR;
    }

    return ++pChunked->nLine <= nLimit ? XSTDOK : XSTDERR;
}

/* Takes the framing bytes of pData[*pPosit, nSize) up to the next run of body bytes, and then takes that run too.
   Returns XSTDOK with the run at pData[*pRun] for *pRunSize bytes, XSTDNON once the input is used up or the body
   ended, and XSTDERR for malformed coding, which every later call reports again. */
static int XHTTP_ChunkStep(xhttp_chunked_t *pChunked, const uint8_t *pData, size_t nSize,
    size_t *pPosit, size_t *pRun, size_t *pRunSize)
{
    size_t nPosit = *pPosit;
    if (pChunked->nState == XHTTP_CHUNK_ERROR) return XSTDERR;

    while (nPosit < nSize && pChunked->nState != XHTTP_CHUNK_DONE)
    {
        if (pChunked->nState == XHTTP_CHUNK_DATA)
        {
            size_t nRun = XSTD_MIN(nSize - nPosit, pChunked->nLeft);
            *pRunSize = nRun;
            *pRun = nPosit;
            *pPosit = nPosit + nRun;

            pChunked->nPosit += nRun;
            pChunked->nBody += nRun;
            pChunked->nLeft -= nRun;

            if (!pChunked->nLeft) pChunked->nState = XHTTP_CHUNK_DATA_CR;
            return XSTDOK;
        }

        pChunked->nPosit++;
        if (XHTTP_ChunkByte(pChunked, pData[nPosit++]) < 0)
        {
            pChunked->nState = XHTTP_CHUNK_ERROR;
            *pPosit = nPosit;
            return XSTDERR;
        }
    }

    *pPosit = nPosit;
    return XSTDNON;
}

void XHTTP_InitChunked(xhttp_chunked_t *pChunked)
{
    XCHECK_VOID_NL(pChunked);
    memset(pChunked, 0, sizeof(*pChunked));
    pChunked->nState = XHTTP_CHUNK_START;
}

/* Scans a chunked body for its end without decoding it. pData holds the body from its first byte, as much of it
   as there is so far, and each call goes on from where the last one stopped: a body that arrives in pieces is
   looked at once, however many pieces it arrives in. */
xhttp_status_t XHTTP_ScanChunked(xhttp_chunked_t *pChunked, const uint8_t *pData, size_t nSize)
{
    XCHECK_NL((pChunked != NULL), XHTTP_INVALID);
    if (pChunked->nState == XHTTP_CHUNK_DONE) return XHTTP_COMPLETE;
    XCHECK_NL((pChunked->nPosit <= nSize && (pData != NULL || !nSize)), XHTTP_INVALID);

    size_t nPosit = pChunked->nPosit;
    size_t nRun = 0, nRunSize = 0;
    int nStatus;

    while ((nStatus = XHTTP_ChunkStep(pChunked, pData, nSize, &nPosit, &nRun, &nRunSize)) == XSTDOK);
    if (nStatus < 0) return XHTTP_INVALID;

    return pChunked->nState == XHTTP_CHUNK_DONE ? XHTTP_COMPLETE : XHTTP_INCOMPLETE;
}

/* Decodes the chunked body buffered after the header where it is, for the socket readers, which go on decoding
   what they read into the same place. Each run of body bytes moves up behind the one before it, and once the
   body is complete whatever followed it moves up behind the body: rawData ends up holding the header,
   nContentLength body bytes and the next message. Malformed coding leaves the bytes before it decoded. */
static int XHTTP_DecodeBuffered(xhttp_t *pHttp)
{
    xbyte_buffer_t *pBuffer = &pHttp->rawData;
    uint8_t *pBody = pBuffer->pData + pHttp->nHeaderLength;
    size_t nSize = pBuffer->nUsed - pHttp->nHeaderLength;
    size_t nPosit = 0, nDecoded = 0;
    size_t nRun = 0, nRunSize = 0;
    int nStatus;

    while ((nStatus = XHTTP_ChunkStep(&pHttp->chunked, pBody, nSize, &nPosit, &nRun, &nRunSize)) == XSTDOK)
    {
        memmove(pBody + nDecoded, pBody + nRun, nRunSize);
        nDecoded += nRunSize;
    }

    size_t nExtra = pHttp->chunked.nState == XHTTP_CHUNK_DONE ? nSize - nPosit : XSTDNON;
    if (nExtra) memmove(pBody + nDecoded, pBody + nPosit, nExtra);

    pBuffer->nUsed = pHttp->nHeaderLength + nDecoded + nExtra;
    pBuffer->pData[pBuffer->nUsed] = XSTR_NUL;

    if (pHttp->chunked.nState == XHTTP_CHUNK_DONE) pHttp->nContentLength = nDecoded;
    return nStatus < 0 ? XSTDERR : XSTDOK;
}

/* Decodes a chunked body that arrived whole into bodyData, the size the scan of it found, and leaves rawData
   as it arrived: the caller's buffer can be parsed again or passed on, and the message still ends where it
   did on the wire. The allocation is never empty, which is how a decoded body is told from none. */
static xhttp_status_t XHTTP_DecodeApart(xhttp_t *pHttp, size_t nDecoded)
{
    const uint8_t *pBody = pHttp->rawData.pData + pHttp->nHeaderLength;
    size_t nSize = pHttp->rawData.nUsed - pHttp->nHeaderLength;
    size_t nPosit = 0, nRun = 0, nRunSize = 0;

    if (XByteBuffer_Init(&pHttp->bodyData, nDecoded + 1, XFALSE) <= 0)
    {
        XByteBuffer_Clear(&pHttp->bodyData);
        return XHTTP_EALLOC;
    }

    /* Every run fits what was reserved for it */
    while (XHTTP_ChunkStep(&pHttp->chunked, pBody, nSize, &nPosit, &nRun, &nRunSize) == XSTDOK)
        XByteBuffer_Add(&pHttp->bodyData, pBody + nRun, nRunSize);

    pHttp->nContentLength = pHttp->bodyData.nUsed;
    return XHTTP_COMPLETE;
}

/* Takes the transfer codings one Transfer-Encoding line lists, in the order they were applied */
static void XHTTP_AddCodings(xhttp_coding_t *pCoding, const char *pList, size_t nLength)
{
    size_t nPosit = 0;
    pCoding->nLines++;

    while (nPosit < nLength)
    {
        while (nPosit < nLength && (pList[nPosit] == ' ' || pList[nPosit] == '\t' || pList[nPosit] == ',')) nPosit++;
        if (nPosit >= nLength) break;

        size_t nEnd = nPosit;
        while (nEnd < nLength && pList[nEnd] != ',') nEnd++;

        size_t nToken = nEnd - nPosit;
        while (pList[nPosit + nToken - 1] == ' ' || pList[nPosit + nToken - 1] == '\t') nToken--;

        pCoding->bLast = (nToken == 7 && xstrncasecmp(&pList[nPosit], "chunked", 7)) ? XTRUE : XFALSE;
        if (pCoding->bLast) pCoding->nChunked++;
        nPosit = nEnd;
    }
}

static void XHTTP_AddLengths(xhttp_coding_t *pCoding, const char *pList, size_t nLength)
{
    const char *pEnd = pList + nLength;

    for (;;)
    {
        while (pList < pEnd && (*pList == ' ' || *pList == '\t')) pList++;

        size_t nValue = 0, nDigits = 0;
        while (pList < pEnd && *pList >= '0' && *pList <= '9')
        {
            size_t nDigit = (size_t)(*pList++ - '0');
            if (nValue > (SIZE_MAX - nDigit) / 10)
            {
                pCoding->bLengthInvalid = XTRUE;
                return;
            }

            nValue = nValue * 10 + nDigit;
            nDigits++;
        }

        while (pList < pEnd && (*pList == ' ' || *pList == '\t')) pList++;

        if (!nDigits || (pList < pEnd && *pList != ','))
        {
            pCoding->bLengthInvalid = XTRUE;
            return;
        }

        if (pCoding->nLengths++ && nValue != pCoding->nLength)
        {
            pCoding->bLengthInvalid = XTRUE;
            return;
        }

        pCoding->nLength = nValue;
        if (pList >= pEnd) return;

        /* The comma, which another length has to follow */
        pList++;
    }
}

/* How the body of a message with these headers ends (RFC 9112, 6.3). A request body that has a transfer
   coding but is not chunked has no length that can be known, and that is no message at all. */
static xhttp_framing_t XHTTP_GetFraming(xhttp_t *pHttp, xhttp_coding_t *pCoding, xbool_t *pValid)
{
    *pValid = XTRUE;

    if (pHttp->eType == XHTTP_RESPONSE &&
        (pHttp->nStatusCode < 200 ||
         pHttp->nStatusCode == 204 ||
         pHttp->nStatusCode == 304))
        return XHTTP_FRAME_NONE;

    /* Chunked is applied last and only once, or the body is not chunked at all (RFC 9112, 6.1) */
    if (pCoding->nLines)
    {
        if (pCoding->bLast && pCoding->nChunked == 1) return XHTTP_FRAME_CHUNKED;
        *pValid = pHttp->eType == XHTTP_RESPONSE ? XTRUE : XFALSE;
        return XHTTP_FRAME_CLOSE;
    }

    if (XHTTP_GetHeader(pHttp, "Content-Length") != NULL) return XHTTP_FRAME_LENGTH;
    return pHttp->eType == XHTTP_RESPONSE ? XHTTP_FRAME_CLOSE : XHTTP_FRAME_NONE;
}

/* Whether the buffer holds the whole message: by its length, by the end of its chunks, or not until the connection
   closes. Content-Type has nothing to do with it. A chunked body is decoded once all of it has arrived, and the
   buffer is never written to, so that a parse of the same bytes from the start finds the same thing. */
static xhttp_status_t XHTTP_CheckComplete(xhttp_t *pHttp)
{
    pHttp->nComplete = XFALSE;

    if (pHttp->eFraming == XHTTP_FRAME_NONE) pHttp->nComplete = XTRUE;
    else if (pHttp->eFraming == XHTTP_FRAME_LENGTH)
        pHttp->nComplete = pHttp->nContentLength <= XHTTP_GetBodySize(pHttp) ? XTRUE : XFALSE;
    else if (pHttp->eFraming == XHTTP_FRAME_CHUNKED)
    {
        /* A body already decoded in part is the readers' to go on with */
        if (pHttp->chunked.nState == XHTTP_CHUNK_DONE) pHttp->nComplete = XTRUE;
        else if (!pHttp->chunked.nPosit)
        {
            const uint8_t *pBody = pHttp->rawData.pData + pHttp->nHeaderLength;
            size_t nSize = pHttp->rawData.nUsed - pHttp->nHeaderLength;

            xhttp_chunked_t scan;
            XHTTP_InitChunked(&scan);

            xhttp_status_t eStatus = XHTTP_ScanChunked(&scan, pBody, nSize);
            if (eStatus == XHTTP_INVALID) return XHTTP_INVALID;

            if (eStatus == XHTTP_COMPLETE)
            {
                if (XHTTP_DecodeApart(pHttp, scan.nBody) != XHTTP_COMPLETE) return XHTTP_EALLOC;
                pHttp->nComplete = XTRUE;
            }
        }
    }

    return pHttp->nComplete ? XHTTP_COMPLETE : XHTTP_PARSED;
}

/* The body bytes in the buffer that belong to the message, rather than to the one after it */
static size_t XHTTP_GetFramedSize(xhttp_t *pHttp)
{
    size_t nBodySize = XHTTP_GetBodySize(pHttp);
    if (pHttp->eFraming == XHTTP_FRAME_NONE) return XSTDNON;
    else if (pHttp->eFraming == XHTTP_FRAME_CLOSE) return nBodySize;
    else if (pHttp->eFraming == XHTTP_FRAME_CHUNKED && pHttp->chunked.nState != XHTTP_CHUNK_DONE) return nBodySize;
    return XSTD_MIN(nBodySize, pHttp->nContentLength);
}

static xhttp_method_t XHTTP_ParseMethod(xhttp_t *pHttp)
{
    const char *pData = (const char *)pHttp->rawData.pData;
    return XHTTP_GetMethodType(pData);
}

static xhttp_type_t XHTTP_ParseType(xhttp_t *pHttp)
{
    const char *pData = (const char*)pHttp->rawData.pData;
    if (!strncmp(pData, "HTTP", 4)) return XHTTP_RESPONSE;
    return XHTTP_REQUEST;
}

static size_t XHTTP_ParseVersion(xhttp_t *pHttp)
{
    const char *pEndPos = (pHttp->eType == XHTTP_REQUEST) ? "\r" : " ";
    const char *pData = (const char *)pHttp->rawData.pData;

    size_t nLength = xstrncuts(pHttp->sVersion, sizeof(pHttp->sVersion), pData, "HTTP/", pEndPos);
    if (!nLength) return XSTDNON;

    size_t i;
    for (i = 0; i < nLength; i++)
    {
        char cByte = pHttp->sVersion[i];
        if (cByte == '\r' || cByte == '\n' || cByte == ' ')
        {
            pHttp->sVersion[0] = XSTR_NUL;
            return XSTDNON;
        }
    }

    return nLength;
}

static uint16_t XHTTP_ParseCode(xhttp_t *pHttp)
{
    char sField[XHTTP_FIELD_MAX];
    const char *pData = (const char *)pHttp->rawData.pData;
    size_t nSize = xstrncuts(sField, sizeof(sField), pData, "HTTP/", NULL);
    size_t nSkip = strlen(pHttp->sVersion) + 1;
    return (nSize > 0 && nSkip < nSize) ? (uint16_t)atoi(&sField[nSkip]) : 0;
}

static size_t XHTTP_ParseHeaderLength(const char *pHdrStr)
{
    int nPosit = xstrsrc(pHdrStr, "\r\n\r\n");
    if (nPosit <= 0) return 0;
    return (size_t)nPosit + 4;
}

static xbool_t XHTTP_GetKeepAlive(xhttp_t *pHttp)
{
    const char *pConnHeader = XHTTP_GetHeader(pHttp, "Connection");
    return xstrncasecmp(pConnHeader, "keep-alive", 10);
}

static size_t XHTTP_ParseUrl(xhttp_t *pHttp)
{
    if (pHttp->eType == XHTTP_RESPONSE) return XSTDOK;
    const char *pHeader = (const char *)pHttp->rawData.pData;
    const char *pMethod = XHTTP_GetMethodStr(pHttp->eMethod);

    char sTmpUrl[XHTTP_URL_MAX];
    sTmpUrl[0] = XSTR_NUL;
    size_t nPosit = 0;

    size_t nLength = xstrncuts(sTmpUrl, sizeof(sTmpUrl), pHeader, pMethod, "HTTP/");
    if (!nLength) return XSTDNON;
    nLength -= 1;

    while (nPosit < sizeof(sTmpUrl) - 1 && sTmpUrl[nPosit] == XSTR_SPACE_CHAR) nPosit++;
    while (nLength > 0 && sTmpUrl[nLength] == XSTR_SPACE_CHAR) sTmpUrl[nLength--] = XSTR_NUL;
    return xstrncpy(pHttp->sUri, sizeof(pHttp->sUri), &sTmpUrl[nPosit]);
}

/* Stores one "Name: value" line. Field names are kept lowercase and the first
   occurrence of a name wins; a line without a name or without a value, once
   the whitespace around it is gone (RFC 7230, 3.2.4), carries nothing.

   The fields that frame the body are looked at on every line all the same, an
   empty one included. Transfer-Encoding is a list that all of its lines add up
   to (RFC 9110, 5.3), and so is Content-Length, whose values have to be one and
   the same length (RFC 9112, 6.3): XHTTP_AddLengths() says whether they are. */
static int XHTTP_ParseHeaderLine(xhttp_t *pHttp, const char *pLine, size_t nLength, xhttp_coding_t *pCoding)
{
    const char *pColon = (const char*)memchr(pLine, ':', nLength);
    if (pColon == NULL || pColon == pLine) return XSTDOK;

    size_t nNameLen = (size_t)(pColon - pLine);
    size_t nValuePos = nNameLen;

    while ((nValuePos < nLength) &&
           (pLine[nValuePos] == ' ' ||
            pLine[nValuePos] == '\t' ||
            pLine[nValuePos] == ':')) nValuePos++;

    while ((nLength > nValuePos) &&
           (pLine[nLength - 1] == ' ' ||
            pLine[nLength - 1] == '\t')) nLength--;

    if (nNameLen == 14 && xstrncasecmp(pLine, "content-length", 14))
        XHTTP_AddLengths(pCoding, pLine + nValuePos, nLength - nValuePos);

    if (nValuePos >= nLength) return XSTDOK;
    char *pName = (char*)malloc(nNameLen + 1);
    if (pName == NULL) return XSTDERR;

    xstrncases(pName, nNameLen + 1, XSTR_LOWER, pLine, nNameLen);
    xmap_pair_t *pPair = XMap_GetPair(&pHttp->headerMap, pName);

    if (xstrcmp(pName, "transfer-encoding"))
        XHTTP_AddCodings(pCoding, pLine + nValuePos, nLength - nValuePos);

    if (pPair != NULL)
    {
        free(pName);
        return XSTDOK;
    }

    size_t nValueLen = nLength - nValuePos;
    char *pValue = (char*)malloc(nValueLen + 1);

    if (pValue == NULL)
    {
        free(pName);
        return XSTDERR;
    }

    memcpy(pValue, pLine + nValuePos, nValueLen);
    pValue[nValueLen] = XSTR_NUL;

    if (XMap_Put(&pHttp->headerMap, pName, pValue) != XMAP_OK)
    {
        free(pName);
        free(pValue);
        return XSTDERR;
    }

    return XSTDOK;
}

/* The header lines follow the start line, one per CRLF */
static int XHTTP_ParseHeaders(xhttp_t *pHttp, xhttp_coding_t *pCoding)
{
    const char *pLine = strstr((const char *)pHttp->rawData.pData, "\r\n");
    const char *pLimit = (const char *)pHttp->rawData.pData + pHttp->nHeaderLength - 1;
    int nStatus = XSTDOK;

    while (pLine != NULL && nStatus == XSTDOK)
    {
        pLine += 2;
        const char *pEnd = (const char*)memchr(pLine, '\r', (size_t)(pLimit - pLine));

        while (pEnd != NULL && pEnd[1] != '\n')
            pEnd = (const char*)memchr(pEnd + 1, '\r', (size_t)(pLimit - pEnd - 1));

        size_t nLength = (size_t)((pEnd != NULL ? pEnd : pLimit) - pLine);

        nStatus = XHTTP_ParseHeaderLine(pHttp, pLine, nLength, pCoding);
        pLine = pEnd;
    }

    pHttp->nHeaderCount = (uint16_t)pHttp->headerMap.nCount;
    return nStatus;
}

int XHTTP_AppendData(xhttp_t *pHttp, uint8_t* pData, size_t nSize)
{
    XCHECK_NL((pHttp != NULL), XSTDERR);
    xbyte_buffer_t *pBuffer = &pHttp->rawData;
    return XByteBuffer_Add(pBuffer, pData, nSize);
}

int XHTTP_InitParser(xhttp_t *pHttp, uint8_t* pData, size_t nSize)
{
    XCHECK_NL((pHttp != NULL), XSTDERR);
    XHTTP_Init(pHttp, XHTTP_DUMMY, XSTDNON);
    int nStatus = XHTTP_AppendData(pHttp, pData, nSize);
    return nSize > 0 && nStatus <= 0 ? XSTDERR : XSTDOK;
}

/* Parses the header, which the caller has made a string of its own */
static xhttp_status_t XHTTP_ParseHeader(xhttp_t *pHttp)
{
    pHttp->eType = XHTTP_ParseType(pHttp);
    if (!XHTTP_ParseVersion(pHttp)) return XHTTP_INVALID;

    if (pHttp->eType == XHTTP_RESPONSE)
    {
        pHttp->nStatusCode = XHTTP_ParseCode(pHttp);
        if (pHttp->nStatusCode < XHTTP_CODE_MIN ||
            pHttp->nStatusCode > XHTTP_CODE_MAX)
            return XHTTP_INVALID;
    }
    else if (pHttp->eType == XHTTP_REQUEST)
        pHttp->eMethod = XHTTP_ParseMethod(pHttp);

    if (!XHTTP_ParseUrl(pHttp)) return XHTTP_INVALID;

    xhttp_coding_t coding;
    memset(&coding, 0, sizeof(coding));

    int nStatus = XHTTP_ParseHeaders(pHttp, &coding);
    if (nStatus == XSTDERR) return XHTTP_EALLOC;
    else if (coding.bLengthInvalid) return XHTTP_INVALID;

    xbool_t bValid = XTRUE;
    pHttp->eFraming = XHTTP_GetFraming(pHttp, &coding, &bValid);
    pHttp->nKeepAlive = XHTTP_GetKeepAlive(pHttp);
    if (!bValid) return XHTTP_INVALID;

    /* A length next to a transfer coding is ignored, and so is the connection after such a message
       (RFC 9112, 6.1). A body that ends with the connection leaves nothing to keep it for either. */
    if ((pHttp->eFraming == XHTTP_FRAME_CHUNKED && XHTTP_GetHeader(pHttp, "Content-Length") != NULL) ||
        pHttp->eFraming == XHTTP_FRAME_CLOSE) pHttp->nKeepAlive = XFALSE;

    /* The length of a chunked body is known once it is decoded */
    if (pHttp->eFraming == XHTTP_FRAME_LENGTH) pHttp->nContentLength = coding.nLength;
    else if (pHttp->chunked.nState != XHTTP_CHUNK_DONE) pHttp->nContentLength = XSTDNON;
    return XHTTP_NONE;
}

xhttp_status_t XHTTP_Parse(xhttp_t *pHttp)
{
    XCHECK_NL((pHttp != NULL), XHTTP_INVALID);
    const char *prawData = (const char*)pHttp->rawData.pData;
    size_t nHeaderLength = XHTTP_ParseHeaderLength(prawData);
    if (!nHeaderLength) return XHTTP_INCOMPLETE;

    /* The last byte of the header is its terminator while it is parsed. The buffer
       may be the caller's own, so the byte goes back whatever the outcome. */
    pHttp->rawData.pData[nHeaderLength - 1] = '\0';
    pHttp->nHeaderLength = nHeaderLength;

    xhttp_status_t eError = XHTTP_ParseHeader(pHttp);
    pHttp->rawData.pData[nHeaderLength - 1] = '\n';
    if (eError != XHTTP_NONE) return XHTTP_StatusCb(pHttp, eError);

    xhttp_status_t nStatus = XHTTP_StatusCb(pHttp, XHTTP_PARSED);
    if (nStatus == XHTTP_TERMINATED) return XHTTP_TERMINATED;

    eError = XHTTP_CheckComplete(pHttp);
    if (eError == XHTTP_INVALID || eError == XHTTP_EALLOC) return XHTTP_StatusCb(pHttp, eError);
    else if (eError == XHTTP_COMPLETE) return XHTTP_COMPLETE;

    return nStatus;
}

xhttp_status_t XHTTP_ParseData(xhttp_t *pHttp, uint8_t* pData, size_t nSize)
{
    XCHECK_NL((pHttp != NULL), XHTTP_INVALID);
    int nStatus = XHTTP_InitParser(pHttp, pData, nSize);
    return nStatus > 0 ? XHTTP_Parse(pHttp) : XHTTP_EALLOC;
}

xhttp_status_t XHTTP_ParseBuff(xhttp_t *pHttp, xbyte_buffer_t *pBuffer)
{
    XCHECK_NL((pHttp != NULL && pBuffer != NULL), XHTTP_INVALID);
    XHTTP_Init(pHttp, XHTTP_DUMMY, XSTDNON);
    XByteBuffer_Set(&pHttp->rawData, pBuffer);
    return XHTTP_Parse(pHttp);
}

xhttp_status_t XHTTP_ReadHeader(xhttp_t *pHttp, xsock_t *pSock)
{
    XCHECK_NL((pHttp != NULL && pSock != NULL), XHTTP_INVALID);
    xbyte_buffer_t *pBuffer = (xbyte_buffer_t*)&pHttp->rawData;
    xhttp_status_t eStatus = (xhttp_status_t)XHTTP_INCOMPLETE;
    uint8_t sBuffer[XHTTP_RX_SIZE];

    while (eStatus == XHTTP_INCOMPLETE)
    {
        int nBytes = XSock_Read(pSock, sBuffer, sizeof(sBuffer));
        if (nBytes <= 0) return XHTTP_StatusCb(pHttp, XHTTP_EREAD);

        if (XByteBuffer_Add(pBuffer, sBuffer, nBytes) <= 0)
            return XHTTP_StatusCb(pHttp, XHTTP_EALLOC);

        eStatus = XHTTP_Parse(pHttp);
        if (eStatus < XHTTP_TERMINATED) return eStatus;

        if (pHttp->nHeaderMax &&
            eStatus == XHTTP_INCOMPLETE &&
            pBuffer->nUsed >= pHttp->nHeaderMax)
            return XHTTP_StatusCb(pHttp, XHTTP_BIGHDR);

        if (XSock_IsNB(pSock)) break;
    }

    int nRetVal = XHTTP_Callback(pHttp, XHTTP_READ_HDR, pBuffer->pData, pBuffer->nUsed);
    if (nRetVal == XSTDERR) return XHTTP_TERMINATED;
    else if (nRetVal == XSTDNON)
    {
        pHttp->nComplete = 1;
        return XHTTP_COMPLETE;
    }

    if (eStatus != XHTTP_COMPLETE &&
        eStatus != XHTTP_PARSED) return eStatus;

    /* The chunks that came with the header are decoded before anyone sees them. They were scanned by the
       parse already, which found no end in them and nothing wrong, so this cannot fail or complete the body. */
    if (eStatus == XHTTP_PARSED &&
        pHttp->eFraming == XHTTP_FRAME_CHUNKED &&
        !pHttp->chunked.nPosit) XHTTP_DecodeBuffered(pHttp);

    /* Only the bytes that belong to this message: what follows it in the buffer is the next one's */
    const uint8_t *pBody = XHTTP_GetBody(pHttp);
    size_t nBodySize = XHTTP_GetFramedSize(pHttp);
    if (pBody == NULL || !nBodySize) return eStatus;

    nRetVal = XHTTP_Callback(pHttp, XHTTP_READ_CNT, pBody, nBodySize);
    if (nRetVal == XSTDERR) return XHTTP_TERMINATED;
    else if (nRetVal == XSTDNON)
    {
        pHttp->nComplete = 1;
        return XHTTP_COMPLETE;
    }

    return eStatus;
}

/* Hands body bytes read from the socket to the content callback and keeps them unless it claims them. Returns
   XHTTP_NONE to go on reading, or the status the read ends with. */
static xhttp_status_t XHTTP_TakeBody(xhttp_t *pHttp, const uint8_t *pData, size_t nSize)
{
    int nRetVal = XHTTP_Callback(pHttp, XHTTP_READ_CNT, pData, nSize);
    if (nRetVal == XSTDERR) return XHTTP_TERMINATED;
    else if (nRetVal == XSTDNON)
    {
        pHttp->nComplete = XTRUE;
        return XHTTP_COMPLETE;
    }
    else if (nRetVal == XSTDOK)
    {
        pHttp->nBodyClaimed += nSize;
        return XHTTP_NONE;
    }

    if (XByteBuffer_Add(&pHttp->rawData, pData, nSize) <= 0)
        return XHTTP_StatusCb(pHttp, XHTTP_EALLOC);

    /* The limit holds whether or not the socket blocks */
    if (pHttp->nContentMax && pHttp->rawData.nUsed >= pHttp->nContentMax)
        return XHTTP_StatusCb(pHttp, XHTTP_BIGCNT);

    return XHTTP_NONE;
}

/* A Content-Length body. No read asks for more than the body still owes: the bytes after it are the
   next message's, and they stay in the socket for whoever reads that one. */
static xhttp_status_t XHTTP_ReadLength(xhttp_t *pHttp, xsock_t *pSock)
{
    uint8_t sBuffer[XHTTP_RX_SIZE];
    xbool_t bRead = XFALSE;

    for (;;)
    {
        size_t nHave = XHTTP_GetBodySize(pHttp) + pHttp->nBodyClaimed;
        if (nHave >= pHttp->nContentLength) break;
        else if (bRead && XSock_IsNB(pSock)) return XHTTP_INCOMPLETE;

        size_t nWant = XSTD_MIN(sizeof(sBuffer), pHttp->nContentLength - nHave);
        int nBytes = XSock_Read(pSock, sBuffer, nWant);
        if (nBytes <= 0) return XHTTP_StatusCb(pHttp, XHTTP_EREAD);

        xhttp_status_t eStatus = XHTTP_TakeBody(pHttp, sBuffer, (size_t)nBytes);
        if (eStatus != XHTTP_NONE) return eStatus;
        bRead = XTRUE;
    }

    pHttp->nComplete = XTRUE;
    return XHTTP_COMPLETE;
}

/* A chunked body, decoded as it arrives. The callback is given body bytes only, never the framing around
   them, and whatever was read past the end of the body is kept behind it for the next message. */
static xhttp_status_t XHTTP_ReadChunked(xhttp_t *pHttp, xsock_t *pSock)
{
    xhttp_chunked_t *pChunked = &pHttp->chunked;
    uint8_t sBuffer[XHTTP_RX_SIZE];
    xbool_t bRead = XFALSE;

    /* What is read is kept in rawData, so it has to be the handle's own: a caller's buffer cannot take it */
    if (pHttp->rawData.pData != NULL && !pHttp->rawData.nSize) return XHTTP_StatusCb(pHttp, XHTTP_EALLOC);

    /* A header parsed from a buffer left the chunks that came with it as they were */
    if (!pChunked->nPosit && XHTTP_GetBodySize(pHttp) &&
        XHTTP_DecodeBuffered(pHttp) < 0) return XHTTP_StatusCb(pHttp, XHTTP_INVALID);

    while (pChunked->nState != XHTTP_CHUNK_DONE)
    {
        if (bRead && XSock_IsNB(pSock)) return XHTTP_INCOMPLETE;

        /* The end of the stream before the last chunk is a body cut short */
        int nBytes = XSock_Read(pSock, sBuffer, sizeof(sBuffer));
        if (nBytes <= 0) return XHTTP_StatusCb(pHttp, XHTTP_EREAD);

        size_t nPosit = 0, nRun = 0, nRunSize = 0;
        int nStatus;
        bRead = XTRUE;

        while ((nStatus = XHTTP_ChunkStep(pChunked, sBuffer, (size_t)nBytes, &nPosit, &nRun, &nRunSize)) == XSTDOK)
        {
            xhttp_status_t eStatus = XHTTP_TakeBody(pHttp, &sBuffer[nRun], nRunSize);
            if (eStatus != XHTTP_NONE) return eStatus;
        }

        if (nStatus < 0) return XHTTP_StatusCb(pHttp, XHTTP_INVALID);
        else if (pChunked->nState != XHTTP_CHUNK_DONE) continue;

        pHttp->nContentLength = XHTTP_GetBodySize(pHttp);
        if (nPosit < (size_t)nBytes &&
            XByteBuffer_Add(&pHttp->rawData, &sBuffer[nPosit], (size_t)nBytes - nPosit) <= 0)
            return XHTTP_StatusCb(pHttp, XHTTP_EALLOC);
    }

    pHttp->nComplete = XTRUE;
    return XHTTP_COMPLETE;
}

/* A body that only the end of the connection ends. A read that fails, or that would block, is not that end. */
static xhttp_status_t XHTTP_ReadUntilClose(xhttp_t *pHttp, xsock_t *pSock)
{
    uint8_t sBuffer[XHTTP_RX_SIZE];
    xbool_t bRead = XFALSE;

    while (XSock_IsOpen(pSock))
    {
        if (bRead && XSock_IsNB(pSock)) return XHTTP_INCOMPLETE;

        int nBytes = XSock_Read(pSock, sBuffer, sizeof(sBuffer));
        if (nBytes <= 0) break;

        xhttp_status_t eStatus = XHTTP_TakeBody(pHttp, sBuffer, (size_t)nBytes);
        if (eStatus != XHTTP_NONE) return eStatus;
        bRead = XTRUE;
    }

    if (XSock_Status(pSock) != XSOCK_EOF) return XHTTP_StatusCb(pHttp, XHTTP_EREAD);
    pHttp->nComplete = XTRUE;
    return XHTTP_COMPLETE;
}

/* Over TLS the end of a body like that is the peer's close_notify (RFC 9112, 9.8): a connection that just stops
   could have been cut short by anyone on the path, and its body is not complete. Sockets ignore an EOF without
   close_notify by default, since most protocols find the end of their data themselves, so it is not ignored for
   as long as this body is read. Without SSL_OP_IGNORE_UNEXPECTED_EOF (OpenSSL before 3.0) the two cannot be told
   apart here. */
static xhttp_status_t XHTTP_ReadClose(xhttp_t *pHttp, xsock_t *pSock)
{
#if defined(XSOCK_USE_SSL) && defined(SSL_OP_IGNORE_UNEXPECTED_EOF)
    SSL *pSSL = XSock_IsSSL(pSock) ? XSock_GetSSL(pSock) : NULL;
    if (pSSL != NULL) SSL_clear_options(pSSL, SSL_OP_IGNORE_UNEXPECTED_EOF);

    xhttp_status_t eStatus = XHTTP_ReadUntilClose(pHttp, pSock);

    /* A failed read may have closed the socket and freed its SSL object */
    if (pSSL != NULL && XSock_GetSSL(pSock) == pSSL) SSL_set_options(pSSL, SSL_OP_IGNORE_UNEXPECTED_EOF);
    return eStatus;
#else
    return XHTTP_ReadUntilClose(pHttp, pSock);
#endif
}

xhttp_status_t XHTTP_ReadContent(xhttp_t *pHttp, xsock_t *pSock)
{
    XCHECK_NL((pHttp != NULL && pSock != NULL), XHTTP_INVALID);
    if (pHttp->nComplete) return XHTTP_COMPLETE;
    else if (pHttp->eFraming == XHTTP_FRAME_LENGTH) return XHTTP_ReadLength(pHttp, pSock);
    else if (pHttp->eFraming == XHTTP_FRAME_CHUNKED) return XHTTP_ReadChunked(pHttp, pSock);
    else if (pHttp->eFraming == XHTTP_FRAME_CLOSE) return XHTTP_ReadClose(pHttp, pSock);

    pHttp->nComplete = XTRUE;
    return XHTTP_COMPLETE;
}

xhttp_status_t XHTTP_Receive(xhttp_t *pHttp, xsock_t *pSock)
{
    xhttp_status_t eStatus = XHTTP_ReadHeader(pHttp, pSock);
    if (eStatus != XHTTP_PARSED) return eStatus;
    return XHTTP_ReadContent(pHttp, pSock);
}

static xhttp_status_t XHTTP_WriteRequest(xhttp_t *pHttp, xsock_t *pSock)
{
    xbyte_buffer_t *pBuff = &pHttp->rawData;
    if (!pBuff->nUsed) return XHTTP_StatusCb(pHttp, XHTTP_EWRITE);
    size_t nOffset = 0;

    do
    {
        int nWritten = XSock_Write(pSock, pBuff->pData + nOffset, pBuff->nUsed - nOffset);
        if (nWritten <= 0) return XHTTP_StatusCb(pHttp, XHTTP_EWRITE);
        nOffset += (size_t)nWritten;
    }
    while (nOffset < pBuff->nUsed);

    int nStatus = XHTTP_Callback(pHttp, XHTTP_WRITE, pBuff->pData, pBuff->nUsed);
    return nStatus == XSTDERR ? XHTTP_TERMINATED : XHTTP_COMPLETE;
}

xhttp_status_t XHTTP_Exchange(xhttp_t *pRequest, xhttp_t *pResponse, xsock_t *pSock)
{
    XCHECK_NL((pRequest != NULL && pResponse != NULL && pSock != NULL), XHTTP_INVALID);
    if (XSock_IsNB(pSock)) return XHTTP_StatusCb(pRequest, XHTTP_EFDMODE);
    XHTTP_Init(pResponse, XHTTP_DUMMY, XSTDNON);

    xhttp_status_t eStatus = XHTTP_WriteRequest(pRequest, pSock);
    if (eStatus != XHTTP_COMPLETE) return eStatus;

    XHTTP_SetCallback(
        pResponse,
        pRequest->callback,
        pRequest->pUserCtx,
        pRequest->nCbTypes
    );

    return XHTTP_Receive(pResponse, pSock);
}

xhttp_status_t XHTTP_Connect(xhttp_t *pHttp, xsock_t *pSock, xlink_t *pLink)
{
    /* The socket is made invalid first, the callers close it whatever the outcome */
    XCHECK_NL((pSock != NULL), XHTTP_INVALID);
    XSock_Init(pSock, XSTDNON, XSOCK_INVALID);
    XCHECK_NL((pHttp != NULL && pLink != NULL), XHTTP_INVALID);

    if (!xstrused(pLink->sProtocol)) xstrncpy(pLink->sProtocol, sizeof(pLink->sProtocol), "http");
    if (strncmp(pLink->sProtocol, "http", 4)) return XHTTP_StatusCb(pHttp, XHTTP_EPROTO);

    uint32_t nFlags = XSOCK_CLIENT;
    if (!strncmp(pLink->sProtocol, "https", 5))
    {
        nFlags |= XSOCK_SSL;
        XSock_InitSSL();
    }

    xsock_info_t addrInfo;
    XSock_InitInfo(&addrInfo);

    if (!xstrused(pHttp->sUnixAddr))
    {
        if (pLink->nPort <= 0)
        {
            pLink->nPort = XFlags_IsSSL(nFlags) ? XHTTP_SSL_PORT : XHTTP_DEF_PORT;
            xstrncat(pLink->sHost, sizeof(pLink->sHost), ":%d", pLink->nPort);
        }

        if (XSock_GetAddrInfo(&addrInfo, pLink->sHost) < 0)
            return XHTTP_StatusCb(pHttp, XHTTP_ERESOLVE);

        if (!addrInfo.nPort) addrInfo.nPort = pLink->nPort;
        nFlags |= XSOCK_TCP;
    }
    else
    {
        xstrncpy(addrInfo.sAddr, sizeof(addrInfo.sAddr), pHttp->sUnixAddr);
        nFlags |= XSOCK_UNIX;
    }

    int nStatus = XHTTP_SetAuthBasic(pHttp, pLink->sUser, pLink->sPass);
    if (nStatus < 0) return XHTTP_StatusCb(pHttp, XHTTP_EAUTH);

    if (XHTTP_XCHECK_FLAG(pHttp->nCbTypes, XHTTP_STATUS) && pHttp->callback != NULL)
    {
        char sBuffer[XHTTP_OPTION_MAX];
        xhttp_ctx_t cbCtx;

        if (XFLAGS_CHECK(nFlags, XSOCK_UNIX))
            cbCtx.nLength = xstrncpyf(
                sBuffer, sizeof(sBuffer),
                "Connecting to unix server: %s",
                addrInfo.sAddr
            );
        else
            cbCtx.nLength = xstrncpyf(
                sBuffer, sizeof(sBuffer),
                "Connecting to remote server: %s:%d",
                addrInfo.sAddr, addrInfo.nPort
            );

        cbCtx.pData = sBuffer;
        cbCtx.eCbType = XHTTP_STATUS;
        cbCtx.eStatus = XHTTP_RESOLVED;
        pHttp->callback(pHttp, &cbCtx);
    }

    if (XSock_Open(pSock, nFlags, &addrInfo) == XSOCK_INVALID)
        return XHTTP_StatusCb(pHttp, XHTTP_ECONNECT);

    if (pHttp->nTimeout)
    {
        XSOCKET nFD = XSock_TimeOutR(pSock, (int)pHttp->nTimeout, 0);
        if (nFD == XSOCK_INVALID) return XHTTP_StatusCb(pHttp, XHTTP_ETIMEO);
    }

    return XHTTP_CONNECTED;
}

xhttp_status_t XHTTP_LinkExchange(xhttp_t *pRequest, xhttp_t *pResponse, xlink_t *pLink)
{
    xsock_t sock;
    xhttp_status_t eStatus = XHTTP_Connect(pRequest, &sock, pLink);

    if (eStatus != XHTTP_CONNECTED)
    {
        XSock_Close(&sock);
        return eStatus;
    }

    eStatus = XHTTP_Exchange(pRequest, pResponse, &sock);
    XSock_Close(&sock);
    return eStatus;
}

xhttp_status_t XHTTP_EasyExchange(xhttp_t *pRequest, xhttp_t *pResponse, const char *pLink)
{
    XCHECK_NL((pRequest != NULL && pResponse != NULL), XHTTP_INVALID);
    xlink_t link;
    int nStatus = XLink_Parse(&link, pLink);
    if (nStatus < 0) return XHTTP_StatusCb(pRequest, XHTTP_ELINK);
    return XHTTP_LinkExchange(pRequest, pResponse, &link);
}

xhttp_status_t XHTTP_Perform(xhttp_t *pHttp, xsock_t *pSock, const uint8_t *pBody, size_t nLength)
{
    XCHECK_NL((pHttp != NULL && pSock != NULL), XHTTP_INVALID);
    if (XSock_IsNB(pSock)) return XHTTP_StatusCb(pHttp, XHTTP_EFDMODE);
    xbyte_buffer_t *pBuff = XHTTP_Assemble(pHttp, pBody, nLength);
    if (pBuff == NULL) return XHTTP_StatusCb(pHttp, XHTTP_EASSEMBLE);

    xhttp_status_t eStatus = XHTTP_WriteRequest(pHttp, pSock);
    if (eStatus != XHTTP_COMPLETE) return eStatus;

    XHTTP_Reset(pHttp, XFALSE);
    return XHTTP_Receive(pHttp, pSock);
}

xhttp_status_t XHTTP_LinkPerform(xhttp_t *pHttp, xlink_t *pLink, const uint8_t *pBody, size_t nLength)
{
    xsock_t sock;
    xhttp_status_t eStatus = XHTTP_Connect(pHttp, &sock, pLink);

    if (eStatus != XHTTP_CONNECTED)
    {
        XSock_Close(&sock);
        return eStatus;
    }

    eStatus = XHTTP_Perform(pHttp, &sock, pBody, nLength);
    XSock_Close(&sock);
    return eStatus;
}

xhttp_status_t XHTTP_EasyPerform(xhttp_t *pHttp, const char *pLink, const uint8_t *pBody, size_t nLength)
{
    xlink_t link;
    if (XLink_Parse(&link, pLink) < 0) return XHTTP_ELINK;
    return XHTTP_LinkPerform(pHttp, &link, pBody, nLength);
}

xhttp_status_t XHTTP_SoloPerform(xhttp_t *pHttp, xhttp_method_t eMethod, const char *pLink, const uint8_t *pBody, size_t nLength)
{
    xlink_t link;
    const char *pVer = XUtils_VersionShort();

    if (XLink_Parse(&link, pLink) < 0) return XHTTP_ELINK;
    if (XHTTP_InitRequest(pHttp, eMethod, link.sUri, NULL) < 0) return XHTTP_EINIT;

    int nStatus = XHTTP_AddHeader(pHttp, "Host", "%s", link.sAddr);
    if (nStatus == XSTDERR) return XHTTP_ESETHDR;
    else if (nStatus == XSTDNON) return XHTTP_EEXISTS;

    nStatus = XHTTP_AddHeader(pHttp, "User-Agent", "xutils/%s", pVer);
    if (nStatus == XSTDERR) return XHTTP_ESETHDR;
    else if (nStatus == XSTDNON) return XHTTP_EEXISTS;

    return XHTTP_LinkPerform(pHttp, &link, pBody, nLength);
}
