/*!
 *  @file libxutils/src/net/mdtp.c
 *
 *  This source is part of the "libxutils" project
 *  2019-2021  Sandro Kalatozishvili (s.kalatoz@gmail.com)
 *
 * @brief Implementation of Modern Data Transmit Protocol
 * (MDTP) packet parser and assembler functionality
 */

#include "xstd.h"
#include "str.h"
#include "mdtp.h"

const char* XPacket_GetStatusStr(xpacket_status_t eStatus)
{
    switch (eStatus)
    {
        case XPACKET_COMPLETE:
            return "Successfully parsed MDTP packet header and body";
        case XPACKET_PARSED:
            return "Successfully parsed MDTP packet header";
        case XPACKET_ERR_ALLOC:
            return "Failed to allocate memory for MDTP packet";
        case XPACKET_BIGDATA:
            return "Receiving MDTP packet bigger than limit";
        case XPACKET_INCOMPLETE:
            return "Invalid or incomplete MDTP packet";
        case XPACKET_INVALID_ARGS:
            return "Invalid or uninitialized arguments";
        case XPACKET_INVALID:
            return "Invalid MDTP packet header";
        default:
            break;
    }

    return "Unknown status";
}

const char *XPacket_GetTypeStr(xpacket_type_t eType)
{
    switch(eType)
    {
        case XPACKET_TYPE_DUMMY: return "dummy";
        case XPACKET_TYPE_MULTY: return "multy";
        case XPACKET_TYPE_ERROR: return "error";
        case XPACKET_TYPE_LITE: return "lite";
        case XPACKET_TYPE_DATA: return "data";
        case XPACKET_TYPE_PING: return "ping";
        case XPACKET_TYPE_PONG: return "pong";
        case XPACKET_TYPE_INFO: return "info";
        case XPACKET_TYPE_ACK: return "ack";
        case XPACKET_TYPE_CMD: return "cmd";
        case XPACKET_TYPE_EOS: return "eos";
        case XPACKET_TYPE_KA: return "ka";
        default: break;
    }

    return "invalid";
}

xpacket_type_t XPacket_GetType(const char *pType)
{
    if (pType == NULL) return XPACKET_TYPE_INVALID;
    else if (!strncmp(pType, "dummy", 5)) return XPACKET_TYPE_DUMMY;
    else if (!strncmp(pType, "multy", 5)) return XPACKET_TYPE_MULTY;
    else if (!strncmp(pType, "error", 5)) return XPACKET_TYPE_ERROR;
    else if (!strncmp(pType, "lite", 4)) return XPACKET_TYPE_LITE;
    else if (!strncmp(pType, "data", 4)) return XPACKET_TYPE_DATA;
    else if (!strncmp(pType, "ping", 4)) return XPACKET_TYPE_PING;
    else if (!strncmp(pType, "pong", 4)) return XPACKET_TYPE_PONG;
    else if (!strncmp(pType, "info", 4)) return XPACKET_TYPE_INFO;
    else if (!strncmp(pType, "ack", 3)) return XPACKET_TYPE_ACK;
    else if (!strncmp(pType, "cmd", 3)) return XPACKET_TYPE_CMD;
    else if (!strncmp(pType, "eos", 3)) return XPACKET_TYPE_EOS;
    else if (!strncmp(pType, "ka", 2)) return XPACKET_TYPE_KA;
    return XPACKET_TYPE_INVALID;
}

static uint32_t XPacket_ReadU32LE(const uint8_t *pData)
{
    return (uint32_t)pData[0] |
           ((uint32_t)pData[1] << 8) |
           ((uint32_t)pData[2] << 16) |
           ((uint32_t)pData[3] << 24);
}

static void XPacket_WriteU32LE(uint8_t *pData, uint32_t nValue)
{
    pData[0] = (uint8_t)(nValue & 0xff);
    pData[1] = (uint8_t)((nValue >> 8) & 0xff);
    pData[2] = (uint8_t)((nValue >> 16) & 0xff);
    pData[3] = (uint8_t)((nValue >> 24) & 0xff);
}

void XPacket_Clear(xpacket_t *pPacket)
{
    XCHECK_VOID_NL(pPacket);

    if (pPacket->callback != NULL)
    {
        pPacket->callback(pPacket, XPACKET_CB_CLEAR);
        pPacket->callback = NULL;
    }

    if (pPacket->pHeaderObj != NULL)
    {
        XJSON_FreeObject(pPacket->pHeaderObj);
        pPacket->pHeaderObj = NULL;
    }

    pPacket->pUserData = NULL;
    XByteBuffer_Clear(&pPacket->rawData);
}

void XPacket_Free(xpacket_t **pPacket)
{
    XCHECK_VOID_NL((pPacket && *pPacket));
    xpacket_t *pMDTPPacket = *pPacket;

    XPacket_Clear(pMDTPPacket);
    if (pMDTPPacket->nAllocated)
    {
        free(pMDTPPacket);
        *pPacket = NULL;
    }
}

void XPacket_ParseHeader(xpacket_header_t *pHeader, xjson_obj_t *pHeaderObj)
{
    XCHECK_VOID_NL((pHeader != NULL));
    const char *pVersion = XJSON_GetString(XJSON_GetObject(pHeaderObj, "version"));
    if (pVersion != NULL && xstrused(pVersion)) xstrncpy(pHeader->sVersion, sizeof(pHeader->sVersion), pVersion);

    const char *pPacketType = XJSON_GetString(XJSON_GetObject(pHeaderObj, "packetType"));
    if (xstrused(pPacketType)) pHeader->eType = XPacket_GetType(pPacketType);

    pHeader->nTimeStamp = XJSON_GetU32(XJSON_GetObject(pHeaderObj, "timeStamp"));
    pHeader->nSessionID = XJSON_GetU32(XJSON_GetObject(pHeaderObj, "sessionId"));
    pHeader->nPacketID = XJSON_GetU32(XJSON_GetObject(pHeaderObj, "packetId"));

    xjson_obj_t *pPayloadObj = XJSON_GetObject(pHeaderObj, "payload");
    if (pPayloadObj != NULL)
    {
        const char *pPayloadType = XJSON_GetString(XJSON_GetObject(pPayloadObj, "payloadType"));
        if (xstrused(pPayloadType)) xstrncpy(pHeader->sPayloadType, sizeof(pHeader->sPayloadType), pPayloadType);

        pHeader->nPayloadSize = XJSON_GetU32(XJSON_GetObject(pPayloadObj, "payloadSize"));
        pHeader->bEncrypted = XJSON_GetBool(XJSON_GetObject(pPayloadObj, "encrypted"));
        pHeader->nSSRCHash = XJSON_GetU32(XJSON_GetObject(pPayloadObj, "ssrcHash"));
    }

    xjson_obj_t *pExtraObj = XJSON_GetObject(pHeaderObj, "extra");
    if (pExtraObj != NULL)
    {
        const char *pTime = XJSON_GetString(XJSON_GetObject(pExtraObj, "time"));
        if (xstrused(pTime)) xstrncpy(pHeader->sTime, sizeof(pHeader->sTime), pTime);

        const char *pTZ = XJSON_GetString(XJSON_GetObject(pExtraObj, "timeZone"));
        if (xstrused(pTZ)) xstrncpy(pHeader->sTZ, sizeof(pHeader->sTZ), pTZ);
    }
}

xpacket_status_t XPacket_UpdateHeader(xpacket_t *pPacket)
{
    XCHECK(pPacket, XPACKET_INVALID_ARGS);
    xpacket_header_t *pHeader = &pPacket->header;

    if (pPacket->pHeaderObj == NULL)
    {
        pPacket->pHeaderObj = XJSON_NewObject(NULL, NULL, 1);
        if (pPacket->pHeaderObj == NULL) return XPACKET_ERR_ALLOC;
    }

    xjson_obj_t *pHeaderObj = pPacket->pHeaderObj;
    pHeaderObj->nAllowUpdate = 1;

    if (pPacket->callback != NULL) pPacket->callback(pPacket, XPACKET_CB_UPDATE);
    if (pHeader->eType == XPACKET_TYPE_ERROR || pHeader->eType == XPACKET_TYPE_INVALID) return XPACKET_INVALID;
    const char *pPacketType = pHeader->eType == XPACKET_TYPE_LITE ? NULL : XPacket_GetTypeStr(pHeader->eType);

    if ((xstrused(pHeader->sVersion) && XJSON_AddString(pHeaderObj, "version", pHeader->sVersion) != XJSON_ERR_NONE) ||
        (xstrused(pPacketType) && XJSON_AddString(pHeaderObj, "packetType", pPacketType) != XJSON_ERR_NONE) ||
        (pHeader->nSessionID && XJSON_AddU32(pHeaderObj, "sessionId", pHeader->nSessionID) != XJSON_ERR_NONE) ||
        (pHeader->nTimeStamp && XJSON_AddU32(pHeaderObj, "timeStamp", pHeader->nTimeStamp) != XJSON_ERR_NONE) ||
        (pHeader->nPacketID && XJSON_AddU32(pHeaderObj, "packetId", pHeader->nPacketID) != XJSON_ERR_NONE))
    {
        pHeader->eType = XPACKET_TYPE_ERROR;
        return XPACKET_ERR_ALLOC;
    }

    uint8_t nHaveDataType = xstrused(pHeader->sPayloadType);
    uint8_t nHaveTime = xstrused(pHeader->sTime);
    uint8_t nHaveTZ = xstrused(pHeader->sTZ);

    if (nHaveTime || nHaveTZ)
    {
        xjson_obj_t *pExtra = XJSON_GetOrCreateObject(pHeaderObj, "extra", 1);
        if (pExtra == NULL)
        {
            pHeader->eType = XPACKET_TYPE_ERROR;
            return XPACKET_ERR_ALLOC;
        }

        if ((nHaveTime && XJSON_AddString(pExtra, "time", pHeader->sTime) != XJSON_ERR_NONE) ||
            (nHaveTZ && XJSON_AddString(pExtra, "timeZone", pHeader->sTZ) != XJSON_ERR_NONE))
        {
            pHeader->eType = XPACKET_TYPE_ERROR;
            return XPACKET_ERR_ALLOC;
        }
    }

    if (pHeader->nPayloadSize)
    {
        xjson_obj_t *pPayloadObj = XJSON_GetOrCreateObject(pHeaderObj, "payload", 1);
        if (pPayloadObj == NULL)
        {
            pHeader->eType = XPACKET_TYPE_ERROR;
            return XPACKET_ERR_ALLOC;
        }

        if ((nHaveDataType && XJSON_AddString(pPayloadObj, "payloadType", pHeader->sPayloadType) != XJSON_ERR_NONE) ||
            (pHeader->bEncrypted && XJSON_AddBool(pPayloadObj, "encrypted", pHeader->bEncrypted) != XJSON_ERR_NONE) ||
            (XJSON_AddU32(pPayloadObj, "payloadSize", pHeader->nPayloadSize) != XJSON_ERR_NONE))
        {
            pHeader->eType = XPACKET_TYPE_ERROR;
            return XPACKET_ERR_ALLOC;
        }
    }

    return XPACKET_ERR_NONE;
}

xpacket_status_t XPacket_Init(xpacket_t *pPacket, uint8_t *pData, uint32_t nSize)
{
    XCHECK_NL((pPacket != NULL), XPACKET_INVALID_ARGS);
    xpacket_header_t *pHeader = &pPacket->header;
    memset(pHeader, 0, sizeof(xpacket_header_t));

    pHeader->eType = XPACKET_TYPE_LITE;
    pHeader->nPayloadSize = nSize;

    XByteBuffer_Init(&pPacket->rawData, 0, 0);
    pPacket->nHeaderLength = 0;
    pPacket->nPacketSize = 0;
    pPacket->nAllocated = 0;
    pPacket->pPayload = pData;
    pPacket->pUserData = NULL;
    pPacket->callback = NULL;

    pPacket->pHeaderObj = XJSON_NewObject(NULL, NULL, 1);
    XCHECK(pPacket->pHeaderObj, XPACKET_ERR_ALLOC);

    return XPACKET_ERR_NONE;
}

xpacket_t *XPacket_New(uint8_t *pData, uint32_t nSize)
{
    xpacket_t *pPacket = (xpacket_t*)malloc(sizeof(xpacket_t));
    XCHECK(pPacket, NULL);

    xpacket_status_t nStatus = XPacket_Init(pPacket, pData, nSize);
    XCHECK_CALL((nStatus == XPACKET_ERR_NONE), free, pPacket, NULL);

    pPacket->nAllocated = 1;
    return pPacket;
}

xpacket_status_t XPacket_Create(xbyte_buffer_t *pBuffer, const char *pHeader, size_t nHdrLen, uint8_t *pData, size_t nSize)
{
    XCHECK((pBuffer != NULL), XPACKET_INVALID_ARGS);
    XCHECK((pHeader != NULL ), XPACKET_INVALID_ARGS);
    XCHECK((nHdrLen > 0), XPACKET_INVALID_ARGS);

    /* The length prefix, like the payload size the header gives, is 32 bits: a longer one would be written cut */
    if (nHdrLen > UINT32_MAX || nSize > UINT32_MAX) return XPACKET_BIGDATA;
    if (pData == NULL) nSize = 0;

    /* A header or a payload in the buffer's content moves if the buffer grows. One in the
       rest of its storage, past nUsed, is where the packet is written, and is refused. */
    size_t nStorage = XSTD_MAX(pBuffer->nSize, pBuffer->nUsed);
    uintptr_t nHdrOffset = (uintptr_t)pHeader - (uintptr_t)pBuffer->pData;
    uintptr_t nDataOffset = (uintptr_t)pData - (uintptr_t)pBuffer->pData;

    xbool_t bHdrAlias = pBuffer->pData != NULL && nHdrOffset < nStorage;
    xbool_t bDataAlias = nSize && pBuffer->pData != NULL && nDataOffset < nStorage;

    if ((bHdrAlias && (nHdrOffset >= pBuffer->nUsed || nHdrLen > pBuffer->nUsed - nHdrOffset)) ||
        (bDataAlias && (nDataOffset >= pBuffer->nUsed || nSize > pBuffer->nUsed - nDataOffset)))
        return XPACKET_INVALID_ARGS;

    /* The byte buffer holds at most INT_MAX bytes and keeps them terminated, one byte past the packet */
    size_t nRoom = pBuffer->nUsed < (size_t)INT_MAX ? (size_t)INT_MAX - pBuffer->nUsed : 0;
    if (nRoom <= XPACKET_INFO_BYTES || nHdrLen >= nRoom - XPACKET_INFO_BYTES ||
        nSize >= nRoom - XPACKET_INFO_BYTES - nHdrLen) return XPACKET_BIGDATA;

    /* Grown once for the whole packet, so a failure leaves the buffer as it was. It used to be cleared, which
       threw away whatever was queued in it before this packet. */
    size_t nPacketSize = XPACKET_INFO_BYTES + nHdrLen + nSize;
    XCHECK((XByteBuffer_Reserve(pBuffer, nPacketSize + 1) > 0), XPACKET_ERR_ALLOC);

    if (bHdrAlias) pHeader = (const char*)pBuffer->pData + nHdrOffset;
    if (bDataAlias) pData = pBuffer->pData + nDataOffset;

    uint8_t *pPacket = pBuffer->pData + pBuffer->nUsed;
    XPacket_WriteU32LE(pPacket, (uint32_t)nHdrLen);
    memcpy(pPacket + XPACKET_INFO_BYTES, pHeader, nHdrLen);
    if (nSize) memcpy(pPacket + XPACKET_INFO_BYTES + nHdrLen, pData, nSize);

    pBuffer->nUsed += nPacketSize;
    pBuffer->pData[pBuffer->nUsed] = '\0';
    return XPACKET_ERR_NONE;
}

/* Writes the packet at the end of pBuffer, and leaves pBuffer as it was on failure */
xpacket_status_t XPacket_Write(xpacket_t *pPacket, xbyte_buffer_t *pBuffer, uint32_t *pHeaderLength)
{
    xpacket_status_t nStatus = XPacket_UpdateHeader(pPacket);
    XCHECK((nStatus == XPACKET_ERR_NONE), nStatus);

    xjson_writer_t jsonWriter;
    XCHECK((XJSON_InitWriter(&jsonWriter, NULL, NULL, XPACKET_HDR_INITIAL) == XJSON_SUCCESS), XPACKET_ERR_ALLOC);

    /* A header that cannot be written is no packet: the empty buffer was handed back as one */
    if (!XJSON_WriteObject(pPacket->pHeaderObj, &jsonWriter))
    {
        XJSON_DestroyWriter(&jsonWriter);
        return XPACKET_INVALID;
    }

    nStatus = XPacket_Create(
        pBuffer,
        jsonWriter.pData,
        jsonWriter.nLength,
        pPacket->pPayload,
        pPacket->header.nPayloadSize
    );

    if (nStatus == XPACKET_ERR_NONE)
    {
        if (pHeaderLength != NULL) *pHeaderLength = (uint32_t)jsonWriter.nLength;
        XPacket_ParseHeader(&pPacket->header, pPacket->pHeaderObj);
    }

    XJSON_DestroyWriter(&jsonWriter);
    return nStatus;
}

xpacket_status_t XPacket_WriteTo(xpacket_t *pPacket, xbyte_buffer_t *pBuffer)
{
    XCHECK((pPacket && pBuffer), XPACKET_INVALID_ARGS);
    return XPacket_Write(pPacket, pBuffer, NULL);
}

xbyte_buffer_t *XPacket_Assemble(xpacket_t *pPacket)
{
    XCHECK((pPacket != NULL), NULL);
    uint32_t nHeaderLength = 0;

    /* A packet that does not assemble leaves nothing behind that looks like one */
    XByteBuffer_Reset(&pPacket->rawData);
    XCHECK_NL((XPacket_Write(pPacket, &pPacket->rawData, &nHeaderLength) == XPACKET_ERR_NONE), NULL);

    pPacket->nHeaderLength = nHeaderLength;
    return &pPacket->rawData;
}

xpacket_status_t XPacket_Parse(xpacket_t *pPacket, const uint8_t *pData, size_t nSize)
{
    XCHECK((pPacket != NULL), XPACKET_INVALID_ARGS);
    xpacket_header_t *pHdr = &pPacket->header;

    memset(pHdr, 0, sizeof(xpacket_header_t));
    XByteBuffer_Init(&pPacket->rawData, XSTDNON, XFALSE);

    pHdr->eType = XPACKET_TYPE_INVALID;
    pPacket->pHeaderObj = NULL;
    pPacket->pPayload = NULL;
    pPacket->pUserData = NULL;
    pPacket->callback = NULL;
    pPacket->nHeaderLength = 0;
    pPacket->nPacketSize = 0;
    pPacket->nAllocated = 0;

    XCHECK((pData != NULL), XPACKET_INVALID_ARGS);
    XCHECK((nSize > 0), XPACKET_INVALID_ARGS);

    if (nSize < XPACKET_INFO_BYTES)
    {
        pHdr->eType = XPACKET_TYPE_INCOMPLETE;
        return XPACKET_INCOMPLETE;
    }

    /* A packet always has a header, as XPacket_Create() makes it. Four zero bytes used to be a complete packet
       of size 0: no header, no payload, and nothing to say how far a reader of the stream had come, so the one in
       api.c went past whatever came after them. */
    pPacket->nHeaderLength = XPacket_ReadU32LE(pData);
    if (!pPacket->nHeaderLength) return XPACKET_INVALID;

    if ((nSize - XPACKET_INFO_BYTES) < pPacket->nHeaderLength)
    {
        pHdr->eType = XPACKET_TYPE_INCOMPLETE;
        return XPACKET_INCOMPLETE;
    }

    xstrncpy(pHdr->sVersion, sizeof(pHdr->sVersion), XPACKET_VERSION_STR);
    const char *pHeader = &((char*)pData)[XPACKET_INFO_BYTES];

    /* A header that could not be parsed for want of memory is not a malformed one */
    xjson_t json;
    if (!XJSON_Parse(&json, NULL, pHeader, pPacket->nHeaderLength))
    {
        xpacket_status_t eStatus = json.nError == XJSON_ERR_ALLOC ? XPACKET_ERR_ALLOC : XPACKET_INVALID;
        XJSON_Destroy(&json);
        return eStatus;
    }

    pPacket->pHeaderObj = json.pRootObj;
    XPacket_ParseHeader(pHdr, pPacket->pHeaderObj);

    size_t nPayloadOffset = XPACKET_INFO_BYTES + (size_t)pPacket->nHeaderLength;
    size_t nPacketSize = nPayloadOffset + (size_t)pHdr->nPayloadSize;

    if (nSize < nPacketSize || nPacketSize > UINT32_MAX)
    {
        pHdr->eType = XPACKET_TYPE_INCOMPLETE;
        XJSON_FreeObject(pPacket->pHeaderObj);
        pPacket->pHeaderObj = NULL;
        return XPACKET_INCOMPLETE;
    }

    pPacket->nPacketSize = (uint32_t)nPacketSize;
    if (pHdr->nPayloadSize) pPacket->pPayload = (uint8_t*)&pData[nPayloadOffset];
    if (pPacket->callback != NULL) pPacket->callback(pPacket, XPACKET_CB_PARSED);

    return XPACKET_COMPLETE;
}

const uint8_t *XPacket_GetHeader(xpacket_t *pPacket)
{
    if (pPacket == NULL || !pPacket->nHeaderLength) return NULL;
    size_t nRequired = XPACKET_INFO_BYTES + pPacket->nHeaderLength;
    if (pPacket->rawData.nUsed < nRequired) return NULL;
    return &pPacket->rawData.pData[XPACKET_INFO_BYTES];
}

const uint8_t *XPacket_GetPayload(xpacket_t *pPacket)
{
    if (pPacket == NULL || !pPacket->header.nPayloadSize) return NULL;
    if (pPacket->rawData.pData == NULL) return pPacket->pPayload;

    size_t nPayloadOffset = XPACKET_INFO_BYTES + (size_t)pPacket->nHeaderLength;
    size_t nActualSize = nPayloadOffset + (size_t)pPacket->header.nPayloadSize;
    if (pPacket->rawData.nUsed < nActualSize) return NULL;

    return &pPacket->rawData.pData[nPayloadOffset];
}

size_t XPacket_GetSize(xpacket_t *pPacket)
{
    if (pPacket == NULL) return XSTDNON;
    return pPacket->nPacketSize;
}
