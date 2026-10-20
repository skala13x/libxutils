/* libxutils: every allocation failure, on every path that can take one.
 *
 * Out-of-memory handling is the part of a library that production never
 * exercises until the one day it does, and by then the failure is a crash
 * rather than an error return. The usual review for it is reading the code,
 * which misses exactly the cases that matter: the third allocation inside a
 * parser, the one that grows a buffer half way through, the one after a
 * pointer has been stored but before its owner has been.
 *
 * So this file drives it mechanically. malloc, calloc, realloc and strdup
 * are wrapped by the linker. Each scenario below is run once to learn how
 * many allocations it makes, then run again once per allocation with that
 * one failing, and the assertions after each run are the same every time:
 *
 *   - the process is still alive and the API reported failure rather than
 *     handing back a half built object,
 *   - everything the scenario allocated before the failure was released.
 *
 * The second point is what makes this worth running under the sanitizer and
 * under valgrind, where a leak on an error path becomes a test failure
 * rather than a number nobody reads.
 */

#include "alloc_inject.h"

#include "array.h"
#include "buf.h"
#include "hash.h"
#include "json.h"
#include "jwt.h"
#include "list.h"
#include "map.h"
#include "pool.h"
#include "str.h"

#include "addr.h"
#include "api.h"
#include "http.h"
#include "mdtp.h"
#include "rtp.h"
#include "sock.h"
#include "ws.h"

#include "base64.h"
#include "crypt.h"
#include "hmac.h"
#include "sha256.h"

#include "xfs.h"
#include "srch.h"
#include "log.h"
#include "cli.h"

#include <unistd.h>

/* ---------------- the scenarios ---------------- */


static int scn_json_parse(void)
{
    const char *pText =
        "{\"name\":\"libxutils\",\"tags\":[\"net\",\"crypt\",\"data\"],"
        "\"nested\":{\"a\":1,\"b\":[true,false,null],\"c\":\"\\u00e9\\u00e8\"},"
        "\"number\":-12.5e3,\"flag\":true}";

    xjson_t json;
    if (XJSON_Parse(&json, NULL, pText, strlen(pText)))
    {
        /* A parse that says it succeeded must have a usable root. */
        if (json.pRootObj == NULL) { XJSON_Destroy(&json); return 1; }

        char *pDumped = XJSON_DumpObj(json.pRootObj, 0, NULL);
        free(pDumped);
    }

    XJSON_Destroy(&json);
    return 0;
}

static int scn_json_write(void)
{
    xjson_obj_t *pRoot = XJSON_NewObject(NULL, NULL, 0);
    if (pRoot == NULL) return 0;

    for (int i = 0; i < 8; i++)
    {
        char sKey[16];
        snprintf(sKey, sizeof(sKey), "key%d", i);

        /* A child that could not be added is still the caller's, which is
         * what the array branch of XJSON_AddObject() documents and what the
         * object branch does too. Writing it as
         *
         *     XJSON_AddObject(pRoot, XJSON_NewInt(...));
         *
         * reads naturally and leaks the child the moment the insert fails,
         * so the handle is kept and released here instead. */
        xjson_obj_t *pChild = XJSON_NewInt(NULL, sKey, i);
        if (pChild == NULL) continue;

        if (XJSON_AddObject(pRoot, pChild) != XJSON_ERR_NONE)
            XJSON_FreeObject(pChild);
    }

    xjson_writer_t writer;
    if (XJSON_InitWriter(&writer, NULL, NULL, 64))
    {
        XJSON_WriteObject(pRoot, &writer);
        XJSON_DestroyWriter(&writer);
    }

    XJSON_FreeObject(pRoot);
    return 0;
}

static int scn_http_parse(void)
{
    const char *pWire =
        "POST /path/to/resource?query=1 HTTP/1.1\r\n"
        "Host: example.invalid\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 13\r\n"
        "X-Extra-Header: some value here\r\n"
        "Authorization: Basic dXNlcjpwYXNz\r\n"
        "\r\n"
        "{\"key\":true}\n";

    xhttp_t http;
    XHTTP_Init(&http, XHTTP_DUMMY, 0);
    xhttp_status_t eStatus = XHTTP_ParseData(&http, (uint8_t*)pWire, strlen(pWire));

    if (eStatus == XHTTP_COMPLETE)
    {
        /* A complete parse must be able to answer for its own fields. */
        const char *pHost = XHTTP_GetHeader(&http, "Host");
        if (pHost != NULL && strcmp(pHost, "example.invalid") != 0)
        {
            XHTTP_Clear(&http);
            return 1;
        }
    }

    XHTTP_Clear(&http);
    return 0;
}

/* The pool and writer of a caller that does not check whether they were set up: whatever allocation fails, using
   them afterwards fails cleanly or works, and never follows what was in their memory before */
static int scn_unchecked_init(void)
{
    xpool_t pool;
    memset(&pool, 0xA5, sizeof(pool));
    XPool_Init(&pool, 256);

    xjson_obj_t *pRoot = XJSON_FromStr(&pool, "{\"a\":[1,2.5,\"x\"],\"b\":{\"c\":null}}");
    xjson_writer_t writer;
    memset(&writer, 0xA5, sizeof(writer));
    XJSON_InitWriter(&writer, &pool, NULL, 4);
    writer.nPretty = 1;
    writer.nTabSize = 2;

    int nBroken = 0;
    if (pRoot != NULL && XJSON_WriteObject(pRoot, &writer) && strstr(writer.pData, "null") == NULL) nBroken = 1;

    XJSON_DestroyWriter(&writer);
    XJSON_FreeObject(pRoot);
    XPool_Destroy(&pool);
    return nBroken;
}

/* A buffer that owned storage borrows other storage, by pointer and from another buffer: what it owned is freed,
   not left behind, and it frees nothing it borrowed */
static int scn_buffer_borrow(void)
{
    static uint8_t sExternal[] = "external";
    xbyte_buffer_t buffer, other;
    XByteBuffer_Init(&buffer, 0, 0);
    XByteBuffer_Init(&other, 0, 0);

    if (XByteBuffer_Add(&buffer, (const uint8_t*)"owned", 5) > 0) XByteBuffer_SetData(&buffer, sExternal, 8);
    if (XByteBuffer_Add(&other, (const uint8_t*)"other", 5) > 0)
    {
        XByteBuffer_Init(&buffer, 0, 0);
        if (XByteBuffer_Add(&buffer, (const uint8_t*)"owned again", 11) > 0) XByteBuffer_Set(&buffer, &other);
    }

    int nBroken = buffer.nSize != 0;
    XByteBuffer_Clear(&buffer);
    XByteBuffer_Clear(&other);
    return nBroken;
}

/* A full ring that is to take an element: whatever allocation fails, it holds either what it held or the new
   element in place of the oldest, never one fewer */
static int scn_ring_evict(void)
{
    xring_buffer_t ring;
    if (XRingBuffer_Init(&ring, 2) != 2) return 0;

    int nBroken = 0;
    if (XRingBuffer_AddData(&ring, (const uint8_t*)"a", 1) == 1 && XRingBuffer_AddData(&ring, (const uint8_t*)"b", 1) == 1)
    {
        int nStatus = XRingBuffer_AddDataAdv(&ring, (const uint8_t*)"cc", 2);
        uint8_t *pData = NULL;
        size_t nSize = 0;

        if (ring.nUsed != 2 || !XRingBuffer_GetData(&ring, &pData, &nSize)) nBroken = 1;
        else if (nStatus > 0 && (nSize != 1 || pData[0] != 'b')) nBroken = 1;
        else if (nStatus <= 0 && (nSize != 1 || pData[0] != 'a')) nBroken = 1;
    }

    XRingBuffer_Destroy(&ring);
    return nBroken;
}

static int scn_http_chunked_parse(void)
{
    const char *pWire =
        "POST /chunks HTTP/1.1\r\nHost: example.invalid\r\nTransfer-Encoding: gzip\r\nTransfer-Encoding: chunked\r\n"
        "Content-Length: 99\r\n\r\n3;ext=1\r\nabc\r\n4\r\ndefg\r\n0\r\nX-Trailer: t\r\n\r\nGET /next HTTP/1.1\r\n\r\n";

    xhttp_t http;
    xhttp_status_t eStatus = XHTTP_ParseData(&http, (uint8_t*)pWire, strlen(pWire));

    /* A complete parse has the whole decoded body and the next request behind it */
    int nBroken = eStatus == XHTTP_COMPLETE &&
        (http.nContentLength != 7 || memcmp(XHTTP_GetBody(&http), "abcdefg", 7) ||
         XHTTP_GetExtraSize(&http) != 22);

    XHTTP_Clear(&http);
    return nBroken;
}

static int scn_http_chunked_read(void)
{
    XSOCKET pair[2];
    if (XSock_CreatePair(pair) != XSTDOK) return 1;

    xsock_t writer, reader;
    XSock_Init(&writer, XSOCK_TCP_PEER, pair[0]);
    XSock_Init(&reader, XSOCK_TCP_PEER, pair[1]);
    XSock_TimeOutR(&reader, 2, 0);

    /* The header and the start of the body in one read, the rest and the next message in another */
    const char sFirst[] = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nab";
    const char sRest[] = "c\r\n4\r\ndefg\r\n0\r\n\r\nHTTP/1.1 204 No Content\r\n\r\n";
    int nBroken = 0;

    xhttp_t http;
    XHTTP_InitParser(&http, NULL, 0);
    XSock_Write(&writer, sFirst, sizeof(sFirst) - 1);

    if (XHTTP_ReadHeader(&http, &reader) == XHTTP_PARSED)
    {
        XSock_Write(&writer, sRest, sizeof(sRest) - 1);
        if (XHTTP_ReadContent(&http, &reader) == XHTTP_COMPLETE)
            nBroken = http.nContentLength != 7 || memcmp(XHTTP_GetBody(&http), "abcdefg", 7) ||
                XHTTP_GetExtraSize(&http) != 27;
    }

    XHTTP_Clear(&http);
    XSock_Close(&writer);
    XSock_Close(&reader);
    return nBroken;
}

static int scn_http_assemble(void)
{
    xhttp_t http;
    if (XHTTP_InitRequest(&http, XHTTP_POST, "/submit", "1.1") > 0)
    {
        XHTTP_AddHeader(&http, "Host", "example.invalid");
        XHTTP_AddHeader(&http, "Content-Type", "application/octet-stream");
        XHTTP_SetAuthBasic(&http, "user", "password");

        xbyte_buffer_t *pWire = XHTTP_Assemble(&http, (const uint8_t*)"body", 4);
        if (pWire != NULL && pWire->nUsed == 0) { XHTTP_Clear(&http); return 1; }
    }

    XHTTP_Clear(&http);
    return 0;
}

/* A message appended behind what a buffer held is whole, or not there at all */
static int scn_http_assemble_to(void)
{
    xbyte_buffer_t buffer;
    XByteBuffer_Init(&buffer, XSTDNON, XFALSE);
    int nBroken = 0;

    xhttp_t http;
    if (XHTTP_InitResponse(&http, 200, "1.1") > 0 && XByteBuffer_Add(&buffer, (const uint8_t*)"queued", 6) > 0)
    {
        http.nKeepAlive = XTRUE;
        XHTTP_AddHeader(&http, "Content-Type", "text/plain");

        xhttp_status_t eStatus = XHTTP_WriteTo(&http, &buffer, (const uint8_t*)"body", 4);
        if (buffer.nUsed < 6 || memcmp(buffer.pData, "queued", 6)) nBroken = 1;
        else if (eStatus != XHTTP_NONE) nBroken = buffer.nUsed != 6 || buffer.pData[6] != '\0';
        else
        {
            xhttp_t parsed;
            xhttp_status_t eParsed = XHTTP_ParseData(&parsed, buffer.pData + 6, buffer.nUsed - 6);
            if (eParsed == XHTTP_COMPLETE)
                nBroken = XHTTP_GetBodySize(&parsed) != 4 || XHTTP_GetPacketSize(&parsed) != buffer.nUsed - 6;
            else if (eParsed != XHTTP_EALLOC) nBroken = 1;
            XHTTP_Clear(&parsed);
        }
    }

    XHTTP_Clear(&http);
    XByteBuffer_Clear(&buffer);
    return nBroken;
}

static int scn_mdtp_assemble_to(void)
{
    const char *pPayload = "mdtp payload";
    xbyte_buffer_t buffer;
    XByteBuffer_Init(&buffer, XSTDNON, XFALSE);
    int nBroken = 0;

    xpacket_t packet;
    if (XPacket_Init(&packet, (uint8_t*)pPayload, (uint32_t)strlen(pPayload)) == XPACKET_ERR_NONE &&
        XByteBuffer_Add(&buffer, (const uint8_t*)"queued", 6) > 0)
    {
        packet.header.eType = XPACKET_TYPE_DATA;
        xstrncpy(packet.header.sVersion, sizeof(packet.header.sVersion), XPACKET_VERSION_STR);

        xpacket_status_t eStatus = XPacket_WriteTo(&packet, &buffer);
        if (buffer.nUsed < 6 || memcmp(buffer.pData, "queued", 6)) nBroken = 1;
        else if (eStatus != XPACKET_ERR_NONE) nBroken = buffer.nUsed != 6 || buffer.pData[6] != '\0';
        else
        {
            xpacket_t parsed;
            xpacket_status_t eParsed = XPacket_Parse(&parsed, buffer.pData + 6, buffer.nUsed - 6);
            if (eParsed == XPACKET_COMPLETE) nBroken = XPacket_GetSize(&parsed) != buffer.nUsed - 6;
            else if (eParsed != XPACKET_ERR_ALLOC) nBroken = 1;
            XPacket_Clear(&parsed);
        }
    }

    XPacket_Clear(&packet);
    XByteBuffer_Clear(&buffer);
    return nBroken;
}

static int scn_ws_frame(void)
{
    xws_frame_t frame;
    XWebFrame_Init(&frame);

    const char *pPayload = "a websocket payload long enough to need its own allocation";
    if (XWebFrame_Create(&frame, (const uint8_t*)pPayload, strlen(pPayload),
        XWS_TEXT, XTRUE, XTRUE) == XWS_ERR_NONE)
    {
        xws_frame_t parsed;
        if (XWebFrame_ParseData(&parsed, frame.buffer.pData, frame.buffer.nUsed) == XWS_FRAME_COMPLETE)
        {
            const uint8_t *pBack = XWebFrame_GetPayload(&parsed);
            size_t nBack = XWebFrame_GetPayloadLength(&parsed);

            if (pBack != NULL && nBack != strlen(pPayload))
            {
                XWebFrame_Clear(&parsed);
                XWebFrame_Clear(&frame);
                return 1;
            }
        }
        XWebFrame_Clear(&parsed);
    }

    XWebFrame_Clear(&frame);
    return 0;
}

static int scn_mdtp_packet(void)
{
    const char *pPayload = "mdtp payload";
    int nBroken = 0;

    xpacket_t packet;
    if (XPacket_Init(&packet, (uint8_t*)pPayload, (uint32_t)strlen(pPayload)) == XPACKET_ERR_NONE)
    {
        packet.header.eType = XPACKET_TYPE_DATA;
        xstrncpy(packet.header.sVersion, sizeof(packet.header.sVersion), XPACKET_VERSION_STR);

        /* A packet handed back is a whole one, whatever allocation failed on the way */
        xbyte_buffer_t *pWire = XPacket_Assemble(&packet);
        if (pWire != NULL)
        {
            xpacket_t parsed;
            xpacket_status_t eStatus = XPacket_Parse(&parsed, pWire->pData, pWire->nUsed);
            if (eStatus != XPACKET_COMPLETE && eStatus != XPACKET_ERR_ALLOC) nBroken = 1;
            XPacket_Clear(&parsed);
        }
    }

    XPacket_Clear(&packet);
    return nBroken;
}

static int scn_array_grow(void)
{
    xarray_t *pArray = XArray_New(NULL, 2, XFALSE);
    if (pArray == NULL) return 0;

    for (int i = 0; i < 64; i++)
    {
        char sValue[24];
        snprintf(sValue, sizeof(sValue), "entry-%d", i);
        XArray_AddData(pArray, sValue, strlen(sValue) + 1);
    }

    /* SortBy supplies the comparator; XArray_Sort() needs one of its own. */
    XArray_SortBy(pArray, XARRAY_SORTBY_SIZE);
    XArray_Destroy(pArray);
    return 0;
}

static int scn_map_grow(void)
{
    /* A map whose table could not be made has no size either */
    xmap_t map;
    if (XMap_Init(&map, NULL, 4) != XMAP_OK) return map.nTableSize != 0 || map.pPairs != NULL;

    char sKeys[64][16];
    for (int i = 0; i < 64; i++)
    {
        snprintf(sKeys[i], sizeof(sKeys[i]), "key-%d", i);
        XMap_Put(&map, sKeys[i], sKeys[i]);
    }

    for (int i = 0; i < 64; i++)
    {
        void *pValue = XMap_Get(&map, sKeys[i]);
        /* Anything the map says it stored must be retrievable. */
        if (pValue != NULL && pValue != sKeys[i]) { XMap_Destroy(&map); return 1; }
    }

    XMap_Destroy(&map);
    return 0;
}

static int scn_list_build(void)
{
    xlist_t *pList = XList_New((void*)"head", 0, NULL, NULL);
    if (pList == NULL) return 0;

    for (int i = 0; i < 32; i++) XList_PushNext(pList, (void*)"node", 0);
    XList_Clear(pList);
    return 0;
}

static int scn_list_push(void)
{
    /* Every way of pushing, onto a head that is not on the heap */
    xlist_t head;
    XList_Init(&head, NULL, 0, NULL, NULL);

    for (int i = 0; i < 8; i++)
    {
        XList_PushPrev(&head, (void*)"prev", 0);
        XList_PushNext(&head, (void*)"next", 0);
        XList_PushFront(&head, (void*)"front", 0);
        XList_PushBack(&head, (void*)"back", 0);
    }

    XList_Clear(&head);
    return 0;
}

static int scn_array_ops(void)
{
    /* Every way of storing an item, with the table growing and shrinking as
       it goes. A pushed buffer is not owned here: whether a failed push has
       released it depends on which step failed, which the caller can not see. */
    xarray_t *pArr = XArray_New(NULL, 0, XFALSE);
    if (pArr == NULL) return 0;

    for (uint32_t i = 0; i < 24; i++)
    {
        XArray_PushData(pArr, (void*)"pushed", 0);

        XArray_AddDataKey(pArr, (void*)"keyed", 6, i);
        XArray_InsertData(pArr, 0, (void*)"front", 6);
        XArray_FreeData(XArray_SetData(pArr, 0, (void*)"set", 4));
    }

    /* Whatever is stored can be read back */
    for (size_t i = 0; i < XArray_Used(pArr); i++)
    {
        if (XArray_Get(pArr, i) != NULL && XArray_GetData(pArr, i) == NULL)
        {
            XArray_Destroy(pArr);
            return 1;
        }
    }

    while (XArray_Used(pArr) > 1) XArray_Delete(pArr, 0);
    XArray_Destroy(pArr);
    return 0;
}

static int scn_array_pool(void)
{
    /* The tables are larger than the pool, so each one takes a chunk of its own */
    xarray_t embedded;
    if (XArray_InitPool(&embedded, 64, 32, XFALSE) != NULL)
    {
        for (int i = 0; i < 40; i++) XArray_AddData(&embedded, (void*)"pooled", 7);
        XArray_Clear(&embedded);
        for (int i = 0; i < 8; i++) XArray_AddData(&embedded, (void*)"again", 6);
    }
    XArray_Destroy(&embedded);

    xarray_t *pArr = XArray_NewPool(64, 32, XFALSE);
    if (pArr == NULL) return 0;

    for (int i = 0; i < 40; i++) XArray_InsertData(pArr, 0, (void*)"pooled", 7);
    XArray_Clear(pArr);
    XArray_Destroy(pArr);
    return 0;
}

static int scn_data_buffer(void)
{
    /* A pointer table that grows with every add and shrinks as it is drained */
    xdata_buffer_t buffer;
    if (XDataBuffer_Init(&buffer, 2, XFALSE) < 0) return buffer.pData != NULL;

    static int values[32];
    for (int i = 0; i < 32; i++) XDataBuffer_Add(&buffer, &values[i]);

    /* Whatever was stored is there, in the order it was added: a failed add only leaves its value out */
    for (size_t i = 1; i < buffer.nUsed; i++)
    {
        int *pPrev = (int*)XDataBuffer_Get(&buffer, (unsigned int)i - 1);
        int *pNext = (int*)XDataBuffer_Get(&buffer, (unsigned int)i);
        if (pPrev == NULL || pNext == NULL || pPrev >= pNext) { XDataBuffer_Destroy(&buffer); return 1; }
    }

    while (buffer.nUsed > 1) XDataBuffer_Pop(&buffer, 0);
    XDataBuffer_Destroy(&buffer);
    return 0;
}

static int scn_string_build(void)
{
    xstring_t str;
    XString_Init(&str, 4, 0);

    for (int i = 0; i < 40; i++) XString_Append(&str, "chunk-%d;", i);

    XString_Replace(&str, "chunk", "piece");
    XString_Case(&str, XSTR_UPPER, 0, 0);

    xarray_t *pTokens = XString_SplitStr(&str, ";");
    XArray_Destroy(pTokens);

    /* XString_Copy() returns the copied length, not a status, so comparing
     * it against XSTDOK silently skips the cleanup for every string longer
     * than one character. It initializes the destination whenever the
     * source has data, which is what makes the unconditional clear safe. */
    if (str.pData != NULL)
    {
        xstring_t copy;
        int nCopied = XString_Copy(&copy, &str);
        if (nCopied != XSTDERR && (size_t)nCopied != str.nLength) { XString_Clear(&copy); XString_Clear(&str); return 1; }
        XString_Clear(&copy);
    }

    XString_Clear(&str);
    return 0;
}

static int scn_str_helpers(void)
{
    char *pDup = xstracpy("%s-%d", "value", 42);
    free(pDup);

    xarray_t *pParts = xstrsplit("a,b,c,d,e,f", ",");
    int nComplete = pParts == NULL || pParts->nUsed == 6;
    XArray_Destroy(pParts);
    if (!nComplete) return 1;

    char *pReplaced = xstrrep("the quick brown fox", "quick", "slow");
    free(pReplaced);

    size_t nCloneLen = 0;
    char *pClone = xstracpyn(&nCloneLen, "%s", "0123456789");
    if (pClone != NULL && strlen(pClone) != nCloneLen) { free(pClone); return 1; }
    free(pClone);

    return 0;
}

static int scn_buffer_grow(void)
{
    xbyte_buffer_t buffer;
    XByteBuffer_Init(&buffer, 8, 0);

    for (int i = 0; i < 64; i++)
    {
        uint8_t sChunk[64];
        memset(sChunk, (uint8_t)i, sizeof(sChunk));
        XByteBuffer_Add(&buffer, sChunk, sizeof(sChunk));
    }

    XByteBuffer_Insert(&buffer, 4, (const uint8_t*)"xx", 2);
    XByteBuffer_Delete(&buffer, 0, 16);

    xbyte_buffer_t *pHeap = XByteBuffer_New(32, 0);
    if (pHeap != NULL) XByteBuffer_Free(&pHeap);

    XByteBuffer_Clear(&buffer);
    return 0;
}

static int scn_pool(void)
{
    xpool_t *pPool = XPool_Create(256);
    if (pPool == NULL) return 0;

    for (int i = 0; i < 64; i++) xalloc(pPool, 64);

    XPool_Reset(pPool);
    XPool_Destroy(pPool);
    return 0;
}

static int scn_hash(void)
{
    xhash_t hash;
    XHash_Init(&hash, NULL, NULL);

    for (int i = 0; i < 64; i++) XHash_Insert(&hash, (void*)"value", 6, i);

    XHash_Destroy(&hash);
    return 0;
}

static int scn_hash_reuse(void)
{
    /* A table used again after it was destroyed releases what it holds */
    xhash_t hash;
    XHash_Init(&hash, NULL, NULL);

    for (int i = 0; i < 8; i++) XHash_Insert(&hash, (void*)"value", 6, i);
    XHash_Destroy(&hash);

    for (int i = 0; i < 8; i++) XHash_Insert(&hash, (void*)"again", 6, i);
    XHash_Delete(&hash, 3);
    XHash_Destroy(&hash);
    return 0;
}

static int scn_base64(void)
{
    const uint8_t sInput[] = "a reasonably long input for base64 encoding and back again";
    size_t nLength = sizeof(sInput) - 1;

    char *pEncoded = XBase64_Encrypt(sInput, &nLength);
    if (pEncoded != NULL)
    {
        size_t nBack = nLength;
        char *pDecoded = XBase64_Decrypt((const uint8_t*)pEncoded, &nBack);

        if (pDecoded != NULL && nBack != sizeof(sInput) - 1)
        {
            free(pDecoded);
            free(pEncoded);
            return 1;
        }

        free(pDecoded);
    }

    free(pEncoded);
    return 0;
}

static int scn_crypt(void)
{
    const uint8_t sInput[] = "plaintext for the symmetric round trip";
    size_t nLength = sizeof(sInput) - 1;

    xcrypt_ctx_t ctx;
    XCrypt_Init(&ctx, XTRUE, "crc32", NULL, NULL);

    uint8_t *pCrypted = XCrypt_Multy(&ctx, sInput, &nLength);
    free(pCrypted);

    size_t nHexLength = sizeof(sInput) - 1;
    uint8_t *pHex = XCrypt_HEX(sInput, &nHexLength, " ", 0, XFALSE);
    free(pHex);

    return 0;
}

static int scn_jwt(void)
{
    const uint8_t sSecret[] = "a-shared-secret-value";
    const char *pPayload = "{\"sub\":\"1234567890\",\"name\":\"libxutils\",\"admin\":true}";

    xjwt_t jwt;
    XJWT_Init(&jwt, XJWT_ALG_HS256);

    if (XJWT_AddPayload(&jwt, pPayload, strlen(pPayload), XFALSE) == XSTDOK)
    {
        size_t nTokenLength = 0;
        char *pToken = XJWT_Create(&jwt, sSecret, sizeof(sSecret) - 1, &nTokenLength);

        if (pToken != NULL)
        {
            xjwt_t parsed;
            XJWT_Parse(&parsed, pToken, nTokenLength, sSecret, sizeof(sSecret) - 1);
            XJWT_Destroy(&parsed);
        }

        free(pToken);
    }

    XJWT_Destroy(&jwt);
    return 0;
}

static int scn_link_parse(void)
{
    xlink_t link;
    XLink_Parse(&link, "https://user:password@example.invalid:8443/deep/path/file.json?a=1&b=2#frag");
    XLink_Parse(&link, "unix:///var/run/service.sock:/v1/status");
    return 0;
}

static int scn_sock_create(void)
{
    xsock_t sock;
    if (XSock_Create(&sock, XSOCK_TCP_CLIENT | XSOCK_NB, "127.0.0.1", 9) != XSOCK_INVALID)
        XSock_Close(&sock);
    else
        XSock_Close(&sock);

    xbyte_buffer_t *pBuffer = XByteBuffer_New(128, 0);
    if (pBuffer != NULL) XByteBuffer_Free(&pBuffer);

    return 0;
}

static int scn_rtp(void)
{
    const uint8_t sWire[] =
        "\x80\x60\x00\x2a\x00\x00\x10\x00\x00\x00\x00\x07"
        "\x12\x34\x56\xa1" "\x00\x03" "abc";

    xrtp_packet_t packet;
    memset(&packet, 0, sizeof(packet));
    XRTP_ParsePacket(&packet, (uint8_t*)sWire, sizeof(sWire) - 1);
    return 0;
}


/* ---------------- filesystem, search and logging ---------------- */

/* Every scenario that touches the filesystem works inside this directory,
 * created once and removed at the end, so a failed run leaves nothing. */
static char g_sWorkDir[128];

static int alloc_workdir_begin(void)
{
    snprintf(g_sWorkDir, sizeof(g_sWorkDir), "/tmp/xutils-alloc-XXXXXX");
    return mkdtemp(g_sWorkDir) != NULL ? XSTDOK : XSTDERR;
}

static void alloc_workdir_end(void)
{
    char sPath[256];
    const char *pNames[] = {"a.txt", "b.txt", "copy.txt", "nested/c.txt"};

    for (size_t i = 0; i < sizeof(pNames) / sizeof(*pNames); i++)
    {
        snprintf(sPath, sizeof(sPath), "%s/%s", g_sWorkDir, pNames[i]);
        unlink(sPath);
    }

    snprintf(sPath, sizeof(sPath), "%s/nested", g_sWorkDir);
    rmdir(sPath);
    rmdir(g_sWorkDir);
}

static int scn_file_io(void)
{
    char sPath[256];
    snprintf(sPath, sizeof(sPath), "%s/a.txt", g_sWorkDir);

    xfile_t file;
    if (XFile_Open(&file, sPath, "cwt", NULL) > 0)
    {
        for (int i = 0; i < 16; i++) XFile_Print(&file, "line %d of the fixture\n", i);
        XFile_Close(&file);
    }

    if (XFile_Open(&file, sPath, "r", NULL) > 0)
    {
        size_t nSize = 0;
        uint8_t *pLoaded = XFile_Load(&file, &nSize);
        free(pLoaded);

        XFile_Close(&file);
    }

    xfile_t *pAlloc = XFile_Alloc(sPath, "r", NULL);
    if (pAlloc != NULL) XFile_Free(&pAlloc);

    size_t nPathSize = 0;
    uint8_t *pWhole = XPath_Load(sPath, &nPathSize);
    free(pWhole);

    xbyte_buffer_t buffer;
    if (XPath_LoadBuffer(sPath, &buffer) > 0) XByteBuffer_Clear(&buffer);

    return 0;
}

static int scn_path_parse(void)
{
    xpath_t path;
    XPath_Parse(&path, "/one/two/three/file.txt", XTRUE);
    XPath_Parse(&path, "relative/path/file.tar.gz", XFALSE);
    return 0;
}

static int scn_search(void)
{
    char sPath[256];
    snprintf(sPath, sizeof(sPath), "%s/b.txt", g_sWorkDir);

    xfile_t file;
    if (XFile_Open(&file, sPath, "cwt", NULL) > 0)
    {
        XFile_Print(&file, "needle in the haystack\n");
        XFile_Close(&file);
    }

    xsearch_t search;
    XSearch_Init(&search, "*.txt");
    search.bRecursive = XTRUE;

    if (XSearch(&search, g_sWorkDir) >= 0) { /* entries live in the context */ }
    XSearch_Destroy(&search);
    return 0;
}

static int scn_search_empty_stdin(void)
{
    int pipeFds[2];
    if (pipe(pipeFds) < 0) return 1;
    close(pipeFds[1]);

    int nSaved = dup(STDIN_FILENO);
    if (nSaved < 0 || dup2(pipeFds[0], STDIN_FILENO) < 0)
    {
        close(pipeFds[0]);
        if (nSaved >= 0) close(nSaved);
        return 1;
    }

    close(pipeFds[0]);
    clearerr(stdin);

    xsearch_t search;
    XSearch_Init(&search, "*");
    search.bReadStdin = XTRUE;
    search.bMatchOnly = XTRUE;
    xstrncpy(search.sText, sizeof(search.sText), "needle");

    int nStatus = XSearch(&search, NULL);
    size_t nFound = XArray_Used(&search.fileArray);
    XSearch_Destroy(&search);

    int nRestored = dup2(nSaved, STDIN_FILENO);
    close(nSaved);
    clearerr(stdin);
    return nRestored < 0 || nStatus != XSTDNON || nFound != 0;
}

static int scn_logger(void)
{
    char sPath[256];
    snprintf(sPath, sizeof(sPath), "%s/log", g_sWorkDir);

    /* The logger is a process wide singleton, so it is left writing
     * nowhere: what is under test is the formatting path it allocates on,
     * not where the line ends up. */
    xlog_cfg_t cfg;
    XLog_ConfigGet(&cfg);

    xbool_t bFile = cfg.bToFile;
    xbool_t bScreen = cfg.bToScreen;

    cfg.bToFile = XFALSE;
    cfg.bToScreen = XFALSE;
    XLog_ConfigSet(&cfg);

    for (int i = 0; i < 8; i++)
        xlogi("a message with a %s and a %d", "string", i);

    cfg.bToFile = bFile;
    cfg.bToScreen = bScreen;
    XLog_ConfigSet(&cfg);
    return 0;
}

/* ---------------- more of the data structures ---------------- */

static int scn_xstring_ops(void)
{
    xstring_t str;
    if (XString_InitFrom(&str, "%s-%d", "seed", 7) < 0) return 0;

    XString_Insert(&str, 2, "INS", 3);
    XString_Advance(&str, 1);
    XString_Remove(&str, 0, 2);

    xstring_t sub;
    if (XString_SubStr(&str, &sub, 0, 3) >= 0) XString_Clear(&sub);

    xstring_t cut;
    if (XString_CutSub(&str, &cut, "-", "7") >= 0) XString_Clear(&cut);

    XString_Clear(&str);
    return 0;
}

static int scn_array_copy(void)
{
    xarray_t *pArray = XArray_New(NULL, 0, XFALSE);
    if (pArray == NULL) return 0;

    for (int i = 0; i < 24; i++) XArray_AddData(pArray, "payload", 8);

    /* Draining through the accessors is what walks the whole table. */
    for (size_t i = 0; i < XArray_Used(pArray); i++) XArray_GetData(pArray, i);

    while (XArray_Used(pArray) > 0)
    {
        xarray_data_t *pItem = XArray_Remove(pArray, 0);
        XArray_FreeData(pItem);
    }

    XArray_Destroy(pArray);
    return 0;
}

static int scn_json_deep(void)
{
    /* Nesting deep enough that the writer has to grow more than once. */
    xstring_t text;
    XString_Init(&text, 0, 0);

    for (int i = 0; i < 24; i++) XString_Append(&text, "{\"level%d\":", i);
    XString_Append(&text, "null");
    for (int i = 0; i < 24; i++) XString_Append(&text, "}");

    if (text.pData != NULL)
    {
        xjson_t json;
        if (XJSON_Parse(&json, NULL, text.pData, text.nLength))
        {
            char *pDumped = XJSON_DumpObj(json.pRootObj, 1, NULL);
            free(pDumped);
        }
        XJSON_Destroy(&json);
    }

    XString_Clear(&text);
    return 0;
}

static int scn_http_headers(void)
{
    /* Enough headers that the map behind them has to grow. */
    xhttp_t http;
    if (XHTTP_InitRequest(&http, XHTTP_GET, "/many", "1.1") > 0)
    {
        for (int i = 0; i < 40; i++)
        {
            char sName[32], sValue[64];
            snprintf(sName, sizeof(sName), "X-Header-%d", i);
            snprintf(sValue, sizeof(sValue), "value number %d for the header", i);
            XHTTP_AddHeader(&http, sName, sValue);
        }

        for (int i = 0; i < 40; i++)
        {
            char sName[32];
            snprintf(sName, sizeof(sName), "X-Header-%d", i);
            XHTTP_GetHeader(&http, sName);
        }
    }

    XHTTP_Clear(&http);
    return 0;
}

static int scn_ws_fragments(void)
{
    /* A fragmented message, which is where the reassembly buffer grows. */
    xws_frame_t frame;
    XWebFrame_Init(&frame);

    const char *pPart = "a fragment of a larger websocket message";
    if (XWebFrame_Create(&frame, (const uint8_t*)pPart, strlen(pPart),
        XWS_TEXT, XTRUE, XFALSE) == XWS_ERR_NONE)
    {
        XWebFrame_AppendData(&frame, (uint8_t*)pPart, strlen(pPart));
        XWebFrame_Unmask(&frame);
    }

    XWebFrame_Clear(&frame);
    return 0;
}

static int scn_hmac(void)
{
    const uint8_t sKey[] = "hmac-key";
    const uint8_t sData[] = "the message being authenticated";
    const char *pSHA = "58ef59fbbcb5633430c5f69c5bd561d42f66fa8667fd7de1919e3ea56787195a";
    const char *pMD5 = "ec7054bab643624274e822432fca3163";
    const char *pBase64 = "WO9Z-7y1YzQwxfacW9Vh1C9m-oZn_X3hkZ4-pWeHGVo";

    uint8_t sDigest[XSHA256_DIGEST_SIZE];
    CHECK(XHMAC_SHA256(sDigest, sizeof(sDigest), sData, sizeof(sData) - 1, sKey, sizeof(sKey) - 1) == XSTDOK,
        "The buffer HMAC requires no heap allocation");

    char sHex[XSHA256_LENGTH + 1];
    CHECK(XHMAC_SHA256_HEX(sHex, sizeof(sHex), sData, sizeof(sData) - 1, sKey, sizeof(sKey) - 1) == XSTDOK &&
        !strcmp(sHex, pSHA), "The HMAC key and message produce the independently calculated SHA-256 tag");

    char *pTag = XHMAC_SHA256_NEW(sData, sizeof(sData) - 1, sKey, sizeof(sKey) - 1);
    int nBroken = pTag ? strcmp(pTag, pSHA) != 0 : g_nFailAt < 1;
    free(pTag);
    pTag = XHMAC_MD5_NEW(sData, sizeof(sData) - 1, sKey, sizeof(sKey) - 1);
    nBroken |= pTag ? strcmp(pTag, pMD5) != 0 : g_nFailAt < 1;
    free(pTag);
    size_t nLength = 123;
    pTag = XHMAC_SHA256_B64(sData, sizeof(sData) - 1, sKey, sizeof(sKey) - 1, &nLength);
    nBroken |= pTag ? strcmp(pTag, pBase64) != 0 || nLength != strlen(pBase64) : g_nFailAt < 1;
    free(pTag);

    return nBroken;
}

static int scn_addr_info(void)
{
    xsock_info_t info;
    XSock_AddrInfo(&info, XF_IPV4, "127.0.0.1");

    char sAddr[XSOCK_ADDR_MAX];
    XAddr_GetIFCIP("lo", sAddr, sizeof(sAddr));
    return 0;
}

static int scn_json_scalars(void)
{
    xjson_obj_t *pRoot = XJSON_NewObject(NULL, NULL, XFALSE);
    if (pRoot == NULL) return 0;
    int nBroken = 0;
    if (XJSON_AddU64(pRoot, "u64", UINT64_MAX) == XJSON_ERR_NONE &&
        XJSON_GetU64(XJSON_GetObject(pRoot, "u64")) != UINT64_MAX) nBroken = 1;
    if (XJSON_AddU32(pRoot, "u32", UINT32_MAX) == XJSON_ERR_NONE &&
        XJSON_GetU32(XJSON_GetObject(pRoot, "u32")) != UINT32_MAX) nBroken = 1;
    if (XJSON_AddU16(pRoot, "u16", UINT16_MAX) == XJSON_ERR_NONE &&
        XJSON_GetU16(XJSON_GetObject(pRoot, "u16")) != UINT16_MAX) nBroken = 1;
    if (XJSON_AddInt(pRoot, "int", -17) == XJSON_ERR_NONE &&
        XJSON_GetInt(XJSON_GetObject(pRoot, "int")) != -17) nBroken = 1;
    if (XJSON_AddFloat(pRoot, "float", 1.25) == XJSON_ERR_NONE &&
        XJSON_GetFloat(XJSON_GetObject(pRoot, "float")) != 1.25) nBroken = 1;
    if (XJSON_AddBool(pRoot, "bool", XTRUE) == XJSON_ERR_NONE &&
        !XJSON_GetBool(XJSON_GetObject(pRoot, "bool"))) nBroken = 1;
    XJSON_AddNull(pRoot, "null");
    XJSON_AddStrIfUsed(pRoot, "text", "hello");
    XJSON_AddStrIfUsed(pRoot, "text", "replacement");
    char *pDump = XJSON_DumpObj(pRoot, 2, NULL);
    if (pDump != NULL)
    {
        xjson_t parsed;
        if (XJSON_Parse(&parsed, NULL, pDump, strlen(pDump)) && parsed.pRootObj == NULL) nBroken = 1;
        XJSON_Destroy(&parsed);
    }
    free(pDump);
    xarray_t *pObjects = XJSON_GetObjects(pRoot);
    XArray_Destroy(pObjects);
    XJSON_FreeObject(pRoot);
    return nBroken;
}

static int scn_string_split(void)
{
    const char *pExpected[] = {"one", "two", "three", "four", "five", "six"};
    xarray_t *pTokens = XString_Split("one:two:three:four:five:six", ":");
    int nBroken = 0;
    if (pTokens != NULL)
    {
        if (pTokens->nUsed != 6) nBroken = 1;
        else
        {
            for (size_t i = 0; i < 6; i++)
            {
                xstring_t *pToken = XArray_GetData(pTokens, i);
                if (!pToken || !pToken->pData || strcmp(pToken->pData, pExpected[i])) nBroken = 1;
            }
        }
    }
    XArray_Destroy(pTokens);
    return nBroken;
}

static int scn_string_edit(void)
{
    xstring_t string;
    if (XString_InitFrom(&string, "%s", "alpha beta gamma") < 0) return 0;
    XString_InsertFmt(&string, 5, " %s", "inserted");
    XString_Color(&string, XSTR_CLR_GREEN, 0, 5);
    xstring_t *pCut = XString_CutNew(&string, "beta", "gamma");
    XString_Clear(pCut);
    xstring_t *pCopy = XString_FromStr(&string);
    XString_Clear(pCopy);
    XString_Clear(&string);
    char borrowed[] = {'a', 'b', 'c'};
    XString_Init(&string, 0, XFALSE);
    XString_Set(&string, borrowed, sizeof(borrowed));
    int nStatus = XString_Resize(&string, 32);
    int nBroken = string.nLength != 3 || !string.pData || memcmp(string.pData, borrowed, 3);
    if (nStatus >= 0 && string.pData == borrowed) nBroken = 1;
    XString_Resize(&string, 0);
    XString_Clear(&string);
    return nBroken;
}

static int scn_string_colors(void)
{
    char output[XSTR_STACK + 8];
    int nBroken = 0;
    size_t nSize = xstrnclr(output, sizeof(output), XSTR_CLR_RED, "%s", "red");
    if (nSize && strcmp(output, XSTR_CLR_RED "red" XSTR_FMT_RESET)) nBroken = 1;
    char *pColor = xstrrgb(1, 2, 3);
    free(pColor);
    pColor = xstryuv(100, 128, 128);
    free(pColor);
    xarray_t *pTokens = xstrsplitd("a,b,c", ",");
    if (pTokens && pTokens->nUsed != 5) nBroken = 1;
    XArray_Destroy(pTokens);
    pTokens = xstrsplite("a,,b", ",");
    if (pTokens && pTokens->nUsed != 3) nBroken = 1;
    XArray_Destroy(pTokens);
    xstrregex("first-middle-last", 17, "first*last");
    return nBroken;
}

typedef struct {
    int nRead;
    int nBroken;
} alloc_api_t;

static int alloc_api_cb(xapi_ctx_t *pCtx, xapi_session_t *pSession)
{
    alloc_api_t *pTest = pCtx->pApi->pUserCtx;
    if (pCtx->eCbType != XAPI_CB_READ) return XAPI_CONTINUE;
    const uint8_t *pBody = NULL;
    size_t nSize = 0;
    if (pSession->eType == XAPI_HTTP)
    {
        xhttp_t *pHttp = pSession->pPacket;
        pBody = (const uint8_t*)XHTTP_GetBody(pHttp);
        nSize = pHttp->nContentLength;
    }
    else if (pSession->eType == XAPI_WS)
    {
        xws_frame_t *pFrame = pSession->pPacket;
        pBody = XWebFrame_GetPayload(pFrame);
        nSize = pFrame->nPayloadLength;
    }
    else
    {
        xbyte_buffer_t *pBuffer = pSession->pPacket;
        pBody = pBuffer->pData;
        nSize = pBuffer->nUsed;
    }
    if (nSize != 3 || pBody == NULL || memcmp(pBody, "abc", 3)) pTest->nBroken++;
    pTest->nRead++;
    return XAPI_DISCONNECT;
}

static int scn_api_input(xapi_type_t eType, const char *pWire, size_t nSize)
{
    alloc_api_t test = {0};
    xapi_t api;
    XAPI_Init(&api, alloc_api_cb, &test);
    XAPI_SetRxSize(&api, 1024);
    XSOCKET pair[2];
    if (XSock_CreatePair(pair) != XSTDOK) return 1;
    if (send(pair[1], pWire, nSize, XMSG_NOSIGNAL) != (ssize_t)nSize)
    {
        close(pair[0]);
        close(pair[1]);
        return 1;
    }
    xapi_endpoint_t endpoint;
    XAPI_InitEndpoint(&endpoint);
    endpoint.eType = eType;
    endpoint.eRole = XAPI_PEER;
    endpoint.nEvents = XPOLLIN;
    endpoint.nFD = pair[0];
    int nStatus = XAPI_AddEvent(&api, &endpoint);
    if (nStatus == XSTDOK)
    {
        api.events.nEventMax = 16;
        for (int i = 0; i < 8 && api.events.nEventCount && !test.nRead; i++)
            if (XAPI_Service(&api, 0) != XEVENTS_SUCCESS) break;
    }
    XAPI_Destroy(&api);
    close(pair[1]);
    if (g_nFailAt < 0 && test.nRead != 1) test.nBroken++;
    return test.nBroken || test.nRead > 1;
}

static int scn_api_http(void)
{
    const char wire[] = "POST /test HTTP/1.1\r\nHost: localhost\r\nContent-Length: 3\r\n\r\nabc";
    return scn_api_input(XAPI_HTTP, wire, sizeof(wire) - 1);
}

static int scn_api_http_chunked(void)
{
    const char wire[] = "POST /test HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n"
        "1\r\na\r\n2\r\nbc\r\n0\r\n\r\n";
    return scn_api_input(XAPI_HTTP, wire, sizeof(wire) - 1);
}

static int scn_api_ws(void)
{
    const char wire[] = "GET /test HTTP/1.1\r\nHost: localhost\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n"
        "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n"
        "\x82\x83\x01\x02\x03\x04\x60\x60\x60";
    return scn_api_input(XAPI_WS, wire, sizeof(wire) - 1);
}

static int scn_api_stream(void)
{
    return scn_api_input(XAPI_SOCK, "abc", 3);
}

static int scn_cli_lines(void)
{
    xcli_win_t window;
    XCLIWin_Init(&window, XTRUE);
    int nBroken = 0;
    for (int i = 0; i < 40; i++)
    {
        size_t nBefore = window.lines.nUsed;
        int nStatus = XCLIWin_AddLineFmt(&window, "line %d", i);
        if (nStatus < 0)
        {
            if (window.lines.nUsed > nBefore) nBroken = 1;
            break;
        }
        if (nStatus > 0)
        {
            char sExpected[32];
            snprintf(sExpected, sizeof(sExpected), "line %d", i);
            const char *pLine = XArray_GetData(&window.lines, nBefore);
            if (pLine == NULL || strcmp(pLine, sExpected)) nBroken = 1;
        }
    }
    XCLIWin_Destroy(&window);
    return nBroken;
}

static int scn_cli_frame(void)
{
    xcli_win_t window;
    XCLIWin_Init(&window, XTRUE);
    int nBroken = 0;
    const char *pLines[] = {"one", "a much longer second line", "\033[31mred\033[0m", ""};
    for (size_t i = 0; i < sizeof(pLines) / sizeof(*pLines); i++)
        if (XCLIWin_AddLine(&window, (char*)pLines[i], strlen(pLines[i]) + 1) < 0) break;
    xbyte_buffer_t frame;
    int nStatus = XCLIWin_GetFrame(&window, &frame);
    if (nStatus < 0 && (frame.pData != NULL || frame.nUsed || window.lines.nUsed)) nBroken = 1;
    if (nStatus > 0 && window.lines.nUsed && !frame.nUsed) nBroken = 1;
    XByteBuffer_Clear(&frame);
    XCLIWin_Destroy(&window);
    return nBroken;
}

/* A strict document that lost a member to a failed allocation is never written out: every add below reports
 * what it could not store, so whatever the writer does turn into text has to hold all of it, and a lost member
 * has to make the write fail. A caller that ignored the add would otherwise send or save it without the member. */
static int scn_json_incomplete(void)
{
    xjson_obj_t *pRoot = XJSON_NewObject(NULL, NULL, 0);
    if (pRoot == NULL) return 0;
    XJSON_SetStrict(pRoot, XTRUE);

    int bLost = 0;
    bLost |= XJSON_AddString(pRoot, "type", "auth") == XJSON_ERR_ALLOC;
    bLost |= XJSON_AddU32(pRoot, "version", 1) == XJSON_ERR_ALLOC;
    bLost |= XJSON_AddStrIfUsed(pRoot, "status", "ok") == XJSON_ERR_ALLOC;
    bLost |= XJSON_AddBool(pRoot, "flag", 1) == XJSON_ERR_ALLOC;
    bLost |= XJSON_AddNull(pRoot, "none") == XJSON_ERR_ALLOC;
    bLost |= XJSON_AddInt(pRoot, "delta", -7) == XJSON_ERR_ALLOC;
    bLost |= XJSON_AddU64(pRoot, "big", 18446744073709551615ULL) == XJSON_ERR_ALLOC;
    bLost |= XJSON_AddU16(pRoot, "port", 443) == XJSON_ERR_ALLOC;
    bLost |= XJSON_AddFloat(pRoot, "ratio", 0.5) == XJSON_ERR_ALLOC;

    /* A nested object and an array, each of which can lose its own members or not exist at all */
    xjson_obj_t *pNested = XJSON_GetOrCreateObject(pRoot, "nested", 0);
    xjson_obj_t *pList = XJSON_GetOrCreateArray(pRoot, "list", 0);
    bLost |= pNested == NULL || pList == NULL;
    if (pNested != NULL) bLost |= XJSON_AddString(pNested, "inner", "value") == XJSON_ERR_ALLOC;

    /* Three items, so the array grows. An item that could not even be created never reached the array, so the
       array cannot know about it: that loss is the caller's to see, and the expected text leaves the list out */
    int bAllItems = 1;
    for (int i = 5; i <= 7; i++)
    {
        xjson_obj_t *pItem = XJSON_NewInt(NULL, NULL, i);
        if (pItem == NULL) bAllItems = 0;
        else if (pList == NULL || XJSON_AddObject(pList, pItem) != XJSON_ERR_NONE)
        {
            bLost = 1;
            XJSON_FreeObject(pItem);
        }
    }

    size_t nLength = 0;
    char *pDump = XJSON_DumpObj(pRoot, 0, &nLength);
    int nBroken = 0;

    if (pDump != NULL && bLost) nBroken = 1;
    else if (pDump != NULL)
    {
        const char *pMembers[] = { "\"type\":\"auth\"", "\"version\":1", "\"status\":\"ok\"", "\"flag\":true",
            "\"none\":null", "\"delta\":-7", "\"big\":18446744073709551615", "\"port\":443", "\"ratio\":",
            "\"inner\":\"value\"", bAllItems ? "\"list\":[5,6,7]" : "\"list\":[" };
        for (size_t i = 0; i < sizeof(pMembers) / sizeof(pMembers[0]); i++)
            if (strstr(pDump, pMembers[i]) == NULL) nBroken = 1;
    }

    if (nBroken) fprintf(stderr, "json-incomplete: written%s: %s\n", bLost ? " after losing a member" : "", pDump);
    free(pDump);
    XJSON_FreeObject(pRoot);
    return nBroken;
}


static const alloc_case_t g_scenarios[] = {
    {"json-parse",     scn_json_parse},
    {"json-write",     scn_json_write},
    {"json-incomplete", scn_json_incomplete},
    {"http-parse",     scn_http_parse},
    {"http-assemble",  scn_http_assemble},
    {"http-assemble-to", scn_http_assemble_to},
    {"unchecked-init", scn_unchecked_init},
    {"ring-evict",     scn_ring_evict},
    {"buffer-borrow",  scn_buffer_borrow},
    {"http-chunked-parse", scn_http_chunked_parse},
    {"http-chunked-read",  scn_http_chunked_read},
    {"ws-frame",       scn_ws_frame},
    {"mdtp-packet",    scn_mdtp_packet},
    {"mdtp-assemble-to", scn_mdtp_assemble_to},
    {"array-grow",     scn_array_grow},
    {"map-grow",       scn_map_grow},
    {"list-build",     scn_list_build},
    {"list-push",      scn_list_push},
    {"array-ops",      scn_array_ops},
    {"array-pool",     scn_array_pool},
    {"data-buffer",    scn_data_buffer},
    {"string-build",   scn_string_build},
    {"str-helpers",    scn_str_helpers},
    {"buffer-grow",    scn_buffer_grow},
    {"pool",           scn_pool},
    {"hash",           scn_hash},
    {"hash-reuse",     scn_hash_reuse},
    {"base64",         scn_base64},
    {"crypt",          scn_crypt},
    {"jwt",            scn_jwt},
    {"link-parse",     scn_link_parse},
    {"sock-create",    scn_sock_create},
    {"rtp",            scn_rtp},
    {"file-io",        scn_file_io},
    {"path-parse",     scn_path_parse},
    {"search",         scn_search},
    {"search-stdin",   scn_search_empty_stdin},
    {"logger",         scn_logger},
    {"xstring-ops",    scn_xstring_ops},
    {"array-copy",     scn_array_copy},
    {"json-deep",      scn_json_deep},
    {"http-headers",   scn_http_headers},
    {"ws-fragments",   scn_ws_fragments},
    {"hmac",           scn_hmac},
    {"addr-info",      scn_addr_info},
    {"cli-lines",      scn_cli_lines},
    {"cli-frame",      scn_cli_frame},
    {"api-http",       scn_api_http},
    {"api-http-chunked", scn_api_http_chunked},
    {"api-ws",         scn_api_ws},
    {"api-stream",     scn_api_stream},
    {"json-scalars",   scn_json_scalars},
    {"string-split",   scn_string_split},
    {"string-edit",    scn_string_edit},
    {"string-colors",  scn_string_colors}
};

static int XTest_sweep(void)
{
    /* The filesystem scenarios need somewhere to work. */
    if (alloc_workdir_begin() != XSTDOK)
    {
        printf("No temporary directory, skipping\n");
        return 77;
    }

    /* Every scenario, every allocation point. A failure names the scenario
     * and the allocation index, which is enough to reproduce it by hand:
     *
     *   XALLOC_ONLY=mdtp-packet XALLOC_AT=18 ./alloc_fail_regression sweep
     *
     * runs that one point on its own, which is what to put under a debugger
     * when the sweep reports a crash rather than a leak. */
    size_t nCount = sizeof(g_scenarios) / sizeof(*g_scenarios);
    const char *pOnly = getenv("XALLOC_ONLY");
    const char *pAt = getenv("XALLOC_AT");
    int nFailed = 0;

    if (pOnly != NULL && pAt != NULL)
    {
        for (size_t i = 0; i < nCount; i++)
        {
            if (strcmp(g_scenarios[i].pName, pOnly) != 0) continue;

            xalloc_arm(atol(pAt));
            int nBroken = g_scenarios[i].run();
            long nLeaked = g_nLive;
            xalloc_disarm();

            printf("%s at %s: broken=%d leaked=%ld\n", pOnly, pAt, nBroken, nLeaked);
            alloc_workdir_end();

            CHECK(nBroken == 0, "The single point handed back no broken object");
            CHECK(nLeaked == 0, "The single point leaked nothing");
            return 0;
        }

        printf("No scenario named %s\n", pOnly);
        alloc_workdir_end();
        return 1;
    }

    for (size_t i = 0; i < nCount; i++)
    {
        if (pOnly != NULL && strcmp(g_scenarios[i].pName, pOnly) != 0) continue;
        if (alloc_sweep(&g_scenarios[i]) != 0) nFailed++;
    }

    alloc_workdir_end();

    CHECK(nFailed == 0, "Every scenario survives a failure at every allocation it makes");
    return 0;
}

static int XTest_accounting(void)
{
    /* The harness itself has to be trustworthy, or a clean sweep proves
     * nothing. These check that the counter sees allocations, that the
     * live count returns to zero, and that injection actually injects. */
    /* volatile so the malloc/free pair cannot be elided as dead: the point
     * is that the call reaches the wrapper, not what the memory holds. */
    xalloc_arm(-1);
    void * volatile pBlock = malloc(64);
    long nAfterAlloc = g_nLive;
    long nCounted = g_nCount;
    free((void*)pBlock);
    long nAfterFree = g_nLive;
    xalloc_disarm();

    CHECK(nCounted >= 1, "The counter sees an allocation");
    CHECK(nAfterAlloc == 1, "An allocation is counted as live");
    CHECK(nAfterFree == 0, "Freeing it brings the live count back to zero");

    /* Injection at the first allocation makes it fail. */
    xalloc_arm(1);
    void * volatile pFailed = malloc(64);
    xalloc_disarm();
    CHECK(pFailed == NULL, "The injected allocation fails");

    /* And only that one: the next succeeds. */
    xalloc_arm(1);
    void * volatile pFirst = malloc(64);
    void * volatile pSecond = malloc(64);
    long nLive = g_nLive;
    xalloc_disarm();

    CHECK(pFirst == NULL, "The first allocation fails");
    CHECK(pSecond != NULL, "The one after it succeeds");
    CHECK(nLive == 1, "Only the successful one is live");
    free((void*)pSecond);

    /* Nothing is counted while the harness is disarmed. */
    long nBefore = g_nCount;
    void * volatile pQuiet = malloc(64);
    free((void*)pQuiet);
    CHECK(g_nCount == nBefore, "Allocations outside a run are not counted");

    /* realloc is tracked through its three shapes. */
    xalloc_arm(-1);
    void * volatile pGrow = realloc(NULL, 16);
    long nAfterGrowAlloc = g_nLive;
    pGrow = realloc((void*)pGrow, 128);
    long nAfterGrow = g_nLive;
    free((void*)pGrow);
    long nAfterGrowFree = g_nLive;
    xalloc_disarm();

    CHECK(nAfterGrowAlloc == 1, "A realloc from nothing is an allocation");
    CHECK(nAfterGrow == 1, "A realloc that grows is not a second one");
    CHECK(nAfterGrowFree == 0, "Freeing the result brings it back to zero");
    return 0;
}

XTEST_MAIN(
    XTEST_CASE(accounting),
    XTEST_CASE(sweep)
)
