/* libxutils: where an HTTP body ends (RFC 9112, 6.3) - with the header, after a length, after the last chunk or with
 * the connection - what a read takes from the socket for it, and what a stream that just stops means for it.
 *
 * Every case runs in one thread over a private socket pair, the TLS ones included, so nothing here waits on a port
 * or on the scheduler: each step writes what the peer would have sent by then, and reads it. */
#include "test.h"
#include "http.h"
#include "sock.h"
#include "xtime.h"
#include <unistd.h>
#include <errno.h>

#include "tls_pair.h"

/* Both ends of a private stream. The reader gives up after two seconds, so a case that is wrong fails
   instead of hanging. */
static int framing_pair(xsock_t *pWriter, xsock_t *pReader)
{
    XSOCKET pair[2] = {XSOCK_INVALID, XSOCK_INVALID};
    if (XSock_CreatePair(pair) != XSTDOK) return XSTDERR;
    XSock_Init(pWriter, XSOCK_TCP_PEER, pair[0]);
    XSock_Init(pReader, XSOCK_TCP_PEER, pair[1]);
    XSock_TimeOutR(pReader, 2, 0);
    return XSTDOK;
}

static int framing_write(xsock_t *pWriter, const void *pData, size_t nSize)
{
    size_t nDone = 0;
    while (nDone < nSize)
    {
        int nBytes = XSock_Write(pWriter, (const uint8_t*)pData + nDone, nSize - nDone);
        if (nBytes <= 0) return XSTDERR;
        nDone += (size_t)nBytes;
    }
    return XSTDOK;
}

static int framing_send(xsock_t *pWriter, const char *pData)
{
    return framing_write(pWriter, pData, strlen(pData));
}

/* Records what the content callback is handed, and answers it the way a case asks */
typedef struct {
    xbyte_buffer_t content;
    int nAnswer;
    int nStopAnswer;
    size_t nStopAfter;
    int nCalls;
} framing_cb_t;

static int framing_callback(xhttp_t *pHttp, xhttp_ctx_t *pCtx)
{
    framing_cb_t *pTest = (framing_cb_t*)pHttp->pUserCtx;
    if (pCtx->eCbType != XHTTP_READ_CNT) return XSTDUSR;

    pTest->nCalls++;
    if (XByteBuffer_Add(&pTest->content, (const uint8_t*)pCtx->pData, pCtx->nLength) <= 0) return XSTDERR;
    if (pTest->nStopAfter && pTest->content.nUsed >= pTest->nStopAfter) return pTest->nStopAnswer;
    return pTest->nAnswer;
}

static void framing_watch(xhttp_t *pHttp, framing_cb_t *pTest, int nAnswer)
{
    memset(pTest, 0, sizeof(*pTest));
    XByteBuffer_Init(&pTest->content, 0, XFALSE);
    pTest->nAnswer = nAnswer;
    XHTTP_SetCallback(pHttp, framing_callback, pTest, XHTTP_READ_CNT);
}

static xbool_t framing_body_is(xhttp_t *pHttp, const char *pBody, size_t nBody)
{
    if (XHTTP_GetBodySize(pHttp) < nBody) return XFALSE;
    return !nBody || !memcmp(XHTTP_GetBody(pHttp), pBody, nBody);
}

/* A message as it arrives, what follows it on the same stream, and what a parse of the two makes of it */
typedef struct {
    const char *pMessage;
    const char *pAfter;
    xhttp_status_t eStatus;
    xhttp_framing_t eFraming;
    size_t nLength;
    const char *pBody;
    xbool_t bKeepAlive;
} framing_case_t;

static int XTest_framing_rules(void)
{
    const framing_case_t cases[] = {
        /* A request without a length or a transfer coding has no body, whatever its type says */
        {"POST /a HTTP/1.1\r\nContent-Type: text/plain\r\n\r\n", "GET /b HTTP/1.1\r\n\r\n",
            XHTTP_COMPLETE, XHTTP_FRAME_NONE, 0, "", XFALSE},
        {"POST /a HTTP/1.1\r\nContent-Length: 3\r\nConnection: keep-alive\r\n\r\nabc", "x",
            XHTTP_COMPLETE, XHTTP_FRAME_LENGTH, 3, "abc", XTRUE},

        /* Some responses never have a body, whatever they announce */
        {"HTTP/1.1 204 No Content\r\nContent-Type: text/plain\r\nConnection: keep-alive\r\n\r\n", "HTTP/1.1 200 OK\r\n\r\n",
            XHTTP_COMPLETE, XHTTP_FRAME_NONE, 0, "", XTRUE},
        {"HTTP/1.1 304 Not Modified\r\nContent-Length: 1234\r\n\r\n", "HTTP/1.1 200 OK\r\n\r\n",
            XHTTP_COMPLETE, XHTTP_FRAME_NONE, 0, "", XFALSE},
        {"HTTP/1.1 100 Continue\r\n\r\n", "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok",
            XHTTP_COMPLETE, XHTTP_FRAME_NONE, 0, "", XFALSE},
        {"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nContent-Length: 9\r\n\r\n", "\x82\x03" "abc",
            XHTTP_COMPLETE, XHTTP_FRAME_NONE, 0, "", XFALSE},

        /* A response without a length has a body that only the end of the connection ends */
        {"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\npartial", "",
            XHTTP_PARSED, XHTTP_FRAME_CLOSE, 0, "partial", XFALSE},
        {"HTTP/1.0 200 OK\r\n\r\n<html>", "", XHTTP_PARSED, XHTTP_FRAME_CLOSE, 0, "<html>", XFALSE},
        {"HTTP/1.1 200 OK\r\nConnection: keep-alive\r\n\r\nbody", "", XHTTP_PARSED, XHTTP_FRAME_CLOSE, 0, "body", XFALSE},
        {"HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\n\r\nzz", "", XHTTP_PARSED, XHTTP_FRAME_CLOSE, 0, "zz", XFALSE},

        /* A request body whose transfer codings do not end in one chunked has no length at all */
        {"POST / HTTP/1.1\r\nTransfer-Encoding: gzip\r\n\r\n", "", XHTTP_INVALID, XHTTP_FRAME_NONE, 0, "", XFALSE},
        {"POST / HTTP/1.1\r\nTransfer-Encoding: chunked, gzip\r\n\r\n", "", XHTTP_INVALID, XHTTP_FRAME_NONE, 0, "", XFALSE},
        {"POST / HTTP/1.1\r\nTransfer-Encoding: chunked, chunked\r\n\r\n0\r\n\r\n", "",
            XHTTP_INVALID, XHTTP_FRAME_NONE, 0, "", XFALSE},
        {"POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n", "",
            XHTTP_INVALID, XHTTP_FRAME_NONE, 0, "", XFALSE},
        {"POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: gzip\r\n\r\n", "",
            XHTTP_INVALID, XHTTP_FRAME_NONE, 0, "", XFALSE},

        /* Chunked applied last and once frames the body, however it is spelled and over however many lines */
        {"POST / HTTP/1.1\r\nTransfer-Encoding: gzip, chunked\r\n\r\n2\r\nzz\r\n0\r\n\r\n", "GET / HTTP/1.1\r\n\r\n",
            XHTTP_COMPLETE, XHTTP_FRAME_CHUNKED, 2, "zz", XFALSE},
        {"POST / HTTP/1.1\r\nTransfer-Encoding: gzip\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nzz\r\n0\r\n\r\n", "",
            XHTTP_COMPLETE, XHTTP_FRAME_CHUNKED, 2, "zz", XFALSE},
        {"POST / HTTP/1.1\r\nTransfer-Encoding: Chunked\r\nConnection: keep-alive\r\n\r\n1\r\nz\r\n0\r\n\r\n", "",
            XHTTP_COMPLETE, XHTTP_FRAME_CHUNKED, 1, "z", XTRUE},
        {"POST / HTTP/1.1\r\nTransfer-Encoding: , chunked \t,\r\n\r\n0\r\n\r\n", "",
            XHTTP_COMPLETE, XHTTP_FRAME_CHUNKED, 0, "", XFALSE},

        /* A length next to it is ignored, and so is the connection after the message */
        {"POST / HTTP/1.1\r\nContent-Length: 100\r\nTransfer-Encoding: chunked\r\nConnection: keep-alive\r\n\r\n"
            "1\r\nz\r\n0\r\n\r\n", "GET / HTTP/1.1\r\n\r\n", XHTTP_COMPLETE, XHTTP_FRAME_CHUNKED, 1, "z", XFALSE},
        {"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Length: 1\r\n\r\n3\r\nabc\r\n0\r\n\r\n", "",
            XHTTP_COMPLETE, XHTTP_FRAME_CHUNKED, 3, "abc", XFALSE},

        /* A chunked body not complete yet is left as it arrived */
        {"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nab", "",
            XHTTP_PARSED, XHTTP_FRAME_CHUNKED, 0, "5\r\nab", XFALSE},

        /* Repeated lengths frame the body only when they agree */
        {"POST / HTTP/1.1\r\nContent-Length: 3\r\nContent-Length: 3\r\n\r\nabc", "",
            XHTTP_COMPLETE, XHTTP_FRAME_LENGTH, 3, "abc", XFALSE},
        {"POST / HTTP/1.1\r\nContent-Length: 3\r\nContent-Length: 4\r\n\r\nabcd", "",
            XHTTP_INVALID, XHTTP_FRAME_NONE, 0, "", XFALSE},
        {"HTTP/1.1 200 OK\r\nContent-Length: 3\r\ncontent-length: 30\r\n\r\nabc", "",
            XHTTP_INVALID, XHTTP_FRAME_NONE, 0, "", XFALSE},

        /* A length that is not one frames nothing: the body after it is not the next message */
        {"POST /test HTTP/1.1\r\nHost: example.com\r\nContent-Length: banana\r\n\r\nhello", "",
            XHTTP_INVALID, XHTTP_FRAME_NONE, 0, "", XFALSE},

        /* A list of the same length is that length, on one line or over several */
        {"POST / HTTP/1.1\r\nContent-Length: 3, 3\r\nContent-Length: 3\r\nConnection: keep-alive\r\n\r\nabc", "x",
            XHTTP_COMPLETE, XHTTP_FRAME_LENGTH, 3, "abc", XTRUE}
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++)
    {
        const framing_case_t *pCase = &cases[i];
        size_t nMessage = strlen(pCase->pMessage);
        size_t nAfter = strlen(pCase->pAfter);
        size_t nBody = strlen(pCase->pBody);

        char sWire[512];
        CHECK(nMessage + nAfter < sizeof(sWire), "The case fits its buffer");
        memcpy(sWire, pCase->pMessage, nMessage);
        memcpy(sWire + nMessage, pCase->pAfter, nAfter);

        xhttp_t http;
        xhttp_status_t eStatus = XHTTP_ParseData(&http, (uint8_t*)sWire, nMessage + nAfter);
        if (eStatus != pCase->eStatus) fprintf(stderr, "Case %zu: status %d\n", i, (int)eStatus);
        CHECK(eStatus == pCase->eStatus, "The body framing decides whether the message is complete");

        if (eStatus != XHTTP_INVALID)
        {
            CHECK(http.eFraming == pCase->eFraming, "Each message is framed by the rule that applies to it");
            CHECK(http.nContentLength == pCase->nLength, "The length is the body's, never one that does not apply");
            CHECK(http.nKeepAlive == pCase->bKeepAlive, "Only a message that can end on its own keeps the connection");
            CHECK(framing_body_is(&http, pCase->pBody, nBody), "The body is what the framing says it is");
            CHECK(XHTTP_GetPacketSize(&http) + XHTTP_GetExtraSize(&http) == http.rawData.nUsed,
                "The message and what follows it account for every byte");
        }

        if (eStatus == XHTTP_COMPLETE)
        {
            CHECK(XHTTP_GetExtraSize(&http) == nAfter, "Exactly the bytes after the message are left over");
            CHECK(!nAfter || !memcmp(XHTTP_GetExtraData(&http), pCase->pAfter, nAfter), "Unchanged, for the next one");
        }
        else if (eStatus == XHTTP_PARSED)
        {
            CHECK(XHTTP_GetExtraSize(&http) == 0 && XHTTP_GetExtraData(&http) == NULL, "Nothing follows an open body");
            CHECK(XHTTP_GetBodySize(&http) == nBody, "All of what arrived is body so far");
        }

        XHTTP_Clear(&http);
    }

    return 0;
}

/* Receives a whole stream through the socket readers with the content callback watching */
static xhttp_status_t framing_receive(xhttp_t *pHttp, framing_cb_t *pTest, const char *pWire, size_t nSize)
{
    xsock_t writer, reader;
    if (framing_pair(&writer, &reader) != XSTDOK) return XHTTP_EINIT;
    framing_write(&writer, pWire, nSize);
    XSock_Close(&writer);

    XHTTP_InitParser(pHttp, NULL, 0);
    framing_watch(pHttp, pTest, XSTDUSR);
    xhttp_status_t eStatus = XHTTP_Receive(pHttp, &reader);
    XSock_Close(&reader);
    return eStatus;
}

static int XTest_framed_callback(void)
{
    /* The content callback is given the bytes of this message and no others: none for a message without a body,
       the length for one that has it, the chunk data and never the framing around it */
    const struct { const char *pWire; xhttp_status_t eStatus; const char *pContent; const char *pAfter; } cases[] = {
        {"HTTP/1.1 204 No Content\r\n\r\nHTTP/1.1 200 OK\r\n\r\n", XHTTP_COMPLETE, "", "HTTP/1.1 200 OK\r\n\r\n"},
        {"HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nabcHTTP/1.1 204 No Content\r\n\r\n", XHTTP_COMPLETE, "abc",
            "HTTP/1.1 204 No Content\r\n\r\n"},
        {"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n0\r\n\r\nHTTP/1.1 204 No Content\r\n\r\n",
            XHTTP_COMPLETE, "abc", "HTTP/1.1 204 No Content\r\n\r\n"},
        {"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n2\r\nd", XHTTP_EREAD, "abcd", ""}
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++)
    {
        xhttp_t http;
        framing_cb_t cb;
        xhttp_status_t eStatus = framing_receive(&http, &cb, cases[i].pWire, strlen(cases[i].pWire));
        size_t nContent = strlen(cases[i].pContent);
        size_t nAfter = strlen(cases[i].pAfter);

        CHECK(eStatus == cases[i].eStatus, "The stream is received as far as it goes");
        CHECK(cb.content.nUsed == nContent && (!nContent || !memcmp(cb.content.pData, cases[i].pContent, nContent)),
            "The callback saw the body of this message alone");
        CHECK(cb.nCalls == (nContent ? 1 : 0), "And nothing at all when there is no body");
        CHECK(XHTTP_GetExtraSize(&http) == nAfter, "What came after the message is kept for the next one");
        CHECK(!nAfter || !memcmp(XHTTP_GetExtraData(&http), cases[i].pAfter, nAfter), "As it arrived");

        XByteBuffer_Clear(&cb.content);
        XHTTP_Clear(&http);
    }

    return 0;
}

static const char g_chunkedHead[] = "POST /c HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n";
static const char g_chunkedBody[] = "4\r\nWiki\r\n5;name=value\r\npedia\r\nE \t; ext=\"q\"\r\n in\r\n\r\nchunks.\r\n"
                                    "0\r\nExpires: never\r\nX-Trailer: 1\r\n\r\n";
static const char g_chunkedData[] = "Wikipedia in\r\n\r\nchunks.";
static const char g_chunkedNext[] = "GET /next HTTP/1.1\r\n\r\n";

static size_t framing_chunked_wire(char *pWire, size_t nSize, xbool_t bNext)
{
    return (size_t)snprintf(pWire, nSize, "%s%s%s", g_chunkedHead, g_chunkedBody, bNext ? g_chunkedNext : "");
}

#define FRAMING_VECTOR(body, data) {body, sizeof(body) - 1, data, sizeof(data) - 1}

static int XTest_chunked_parse(void)
{
    char sWire[512];
    size_t nWire = framing_chunked_wire(sWire, sizeof(sWire), XTRUE);
    size_t nData = sizeof(g_chunkedData) - 1;
    size_t nNext = sizeof(g_chunkedNext) - 1;

    /* Decoded apart from the bytes as they arrived, which still hold the message and the next one after it */
    xhttp_t http;
    CHECK(XHTTP_ParseData(&http, (uint8_t*)sWire, nWire) == XHTTP_COMPLETE, "A chunked request completes");
    CHECK(http.eFraming == XHTTP_FRAME_CHUNKED && http.nContentLength == nData, "Its length is that of its chunks");
    CHECK(!memcmp(XHTTP_GetBody(&http), g_chunkedData, nData), "Its body is their data, with no framing in it");
    CHECK(XHTTP_GetBodySize(&http) == nData && XHTTP_GetExtraSize(&http) == nNext, "The next request follows");
    CHECK(!memcmp(XHTTP_GetExtraData(&http), g_chunkedNext, nNext), "Unchanged");
    CHECK(XHTTP_GetPacketSize(&http) == nWire - nNext, "The message ends where it ended on the wire");
    CHECK(http.rawData.nUsed == nWire && !memcmp(http.rawData.pData, sWire, nWire), "Which is left as it was");
    CHECK(http.chunked.nBody == nData && http.chunked.nPosit == sizeof(g_chunkedBody) - 1, "Every chunk was taken");
    CHECK(strcmp(XHTTP_GetHeader(&http, "Host"), "x") == 0, "The header is untouched");

    xhttp_t next;
    CHECK(XHTTP_ParseData(&next, (uint8_t*)XHTTP_GetExtraData(&http), nNext) == XHTTP_COMPLETE &&
        !strcmp(next.sUri, "/next"), "The next request parses from where the first one ended");
    XHTTP_Clear(&next);

    /* A parse of a body already decoded is the same message again, not a second decode */
    CHECK(XHTTP_Parse(&http) == XHTTP_COMPLETE && http.nContentLength == nData, "A second parse completes the same");
    CHECK(!memcmp(XHTTP_GetBody(&http), g_chunkedData, nData), "With the same body");

    xhttp_t copy;
    CHECK(XHTTP_Copy(&copy, &http) == XSTDOK, "A decoded message copies");
    CHECK(copy.eFraming == XHTTP_FRAME_CHUNKED && copy.chunked.nBody == nData && XHTTP_GetExtraSize(&copy) == nNext,
        "With its framing and where it ends");
    CHECK(XHTTP_GetBodySize(&copy) == nData && XHTTP_GetBody(&copy) != XHTTP_GetBody(&http) &&
        !memcmp(XHTTP_GetBody(&copy), g_chunkedData, nData), "And a decoded body of its own");
    XHTTP_Clear(&copy);

    /* A handle reset for the next message forgets the decoded body along with the rest of this one */
    for (int nHard = 0; nHard < 2; nHard++)
    {
        const char sNext[] = "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nnext";
        if (nHard) CHECK(XHTTP_ParseData(&http, (uint8_t*)sWire, nWire) == XHTTP_COMPLETE, "Parse a chunked one again");
        XHTTP_Reset(&http, nHard);
        CHECK(http.eFraming == XHTTP_FRAME_NONE && !http.chunked.nPosit && http.bodyData.pData == NULL, "It is reset");
        CHECK(XHTTP_AppendData(&http, (uint8_t*)sNext, sizeof(sNext) - 1) > 0, "Buffer another message");
        CHECK(XHTTP_Parse(&http) == XHTTP_COMPLETE && XHTTP_GetBodySize(&http) == 4, "It parses with its own body");
        CHECK(framing_body_is(&http, "next", 4), "Not the one before it");
        XHTTP_Clear(&http);
    }

    /* Every way a chunk can be written that is not the plain one */
    const struct { const char *pBody; size_t nBody; const char *pData; size_t nData; } vectors[] = {
        FRAMING_VECTOR("0\r\n\r\n", ""),
        FRAMING_VECTOR("A\r\n0123456789\r\n0\r\n\r\n", "0123456789"),
        FRAMING_VECTOR("f\r\n0123456789abcde\r\n0\r\n\r\n", "0123456789abcde"),
        FRAMING_VECTOR("0005\r\nabcde\r\n0\r\n\r\n", "abcde"),
        FRAMING_VECTOR("5 \r\nabcde\r\n0\r\n\r\n", "abcde"),
        FRAMING_VECTOR("5\t;x\r\nabcde\r\n0\r\n\r\n", "abcde"),
        FRAMING_VECTOR("3;\x80\t=v\r\nabc\r\n0\r\n\r\n", "abc"),
        FRAMING_VECTOR("5;a=\"b c\"\r\nabcde\r\n0;last\r\n\r\n", "abcde"),
        FRAMING_VECTOR("1\r\na\r\n1\r\nb\r\n0\r\nA: 1\r\nB:\t2\r\n\r\n", "ab"),
        FRAMING_VECTOR("5\r\n\r\n\0\r\n\r\n0\r\n\r\n", "\r\n\0\r\n")
    };

    for (size_t i = 0; i < sizeof(vectors) / sizeof(*vectors); i++)
    {
        uint8_t sMessage[256];
        size_t nHead = sizeof(g_chunkedHead) - 1;
        memcpy(sMessage, g_chunkedHead, nHead);
        memcpy(sMessage + nHead, vectors[i].pBody, vectors[i].nBody);

        CHECK(XHTTP_ParseData(&http, sMessage, nHead + vectors[i].nBody) == XHTTP_COMPLETE, "Each form decodes");
        CHECK(http.nContentLength == vectors[i].nData && XHTTP_GetBodySize(&http) == vectors[i].nData,
            "To exactly its data");
        CHECK(!vectors[i].nData || !memcmp(XHTTP_GetBody(&http), vectors[i].pData, vectors[i].nData), "Binary safe");
        XHTTP_Clear(&http);
    }

    /* A caller's buffer is left as it was, so advancing it past the message lands on the next one */
    xbyte_buffer_t buffer;
    CHECK(XByteBuffer_Init(&buffer, 0, XFALSE) >= 0 && XByteBuffer_Add(&buffer, (uint8_t*)sWire, nWire) > 0,
        "Buffer the stream");
    CHECK(XHTTP_ParseBuff(&http, &buffer) == XHTTP_COMPLETE, "A borrowed buffer parses");
    CHECK(buffer.nUsed == nWire && !memcmp(buffer.pData, sWire, nWire), "The caller's buffer is untouched");
    CHECK(http.nContentLength == nData && !memcmp(XHTTP_GetBody(&http), g_chunkedData, nData), "Its body is decoded");
    size_t nPacket = XHTTP_GetPacketSize(&http);
    XHTTP_Clear(&http);
    CHECK(XByteBuffer_Advance(&buffer, nPacket) == (int)nNext, "Past the message");
    CHECK(!memcmp(buffer.pData, g_chunkedNext, nNext), "Is the next request");
    XByteBuffer_Clear(&buffer);

    /* A view that ends exactly where its data does is never written past */
    uint8_t *pExact = (uint8_t*)malloc(nWire);
    CHECK(pExact != NULL, "Allocate an exact view");
    memcpy(pExact, sWire, nWire);
    XByteBuffer_SetData(&buffer, pExact, nWire);
    CHECK(XHTTP_ParseBuff(&http, &buffer) == XHTTP_COMPLETE && XHTTP_GetExtraSize(&http) == nNext,
        "An exact view parses");
    CHECK(!memcmp(pExact, sWire, nWire), "And is left as it was");

    /* The socket readers cannot keep what they read in it */
    xsock_t writer, reader;
    CHECK(framing_pair(&writer, &reader) == XSTDOK, "Create a stream");
    xhttp_t view;
    XHTTP_Init(&view, XHTTP_DUMMY, XSTDNON);
    XByteBuffer_SetData(&view.rawData, pExact, sizeof(g_chunkedHead) + 1);
    CHECK(XHTTP_Parse(&view) == XHTTP_PARSED, "A view of the header and a byte of the body parses");
    CHECK(XHTTP_ReadContent(&view, &reader) == XHTTP_EALLOC, "The content read refuses a buffer it does not own");
    CHECK(!memcmp(pExact, sWire, nWire), "Without touching it");
    XHTTP_Clear(&view);
    XSock_Close(&writer);
    XSock_Close(&reader);

    XHTTP_Clear(&http);
    free(pExact);
    return 0;
}

static int XTest_chunked_split(void)
{
    /* Split anywhere, the bytes before the end of the body are left exactly as they arrived, so that a parse
       from the start finds the same thing again; the bytes that complete it decode it whole */
    char sWire[512];
    size_t nWire = framing_chunked_wire(sWire, sizeof(sWire), XTRUE);
    size_t nMessage = nWire - (sizeof(g_chunkedNext) - 1);
    size_t nData = sizeof(g_chunkedData) - 1;

    for (size_t nSplit = 0; nSplit < nWire; nSplit++)
    {
        xhttp_t http;
        CHECK(XHTTP_InitParser(&http, (uint8_t*)sWire, nSplit) > 0, "Buffer a prefix");
        xhttp_status_t eStatus = XHTTP_Parse(&http);

        if (nSplit < nMessage)
        {
            CHECK(eStatus == XHTTP_INCOMPLETE || eStatus == XHTTP_PARSED, "A prefix of the body is not a message");
            CHECK(http.rawData.nUsed == nSplit && (!nSplit || !memcmp(http.rawData.pData, sWire, nSplit)),
                "And is left untouched");
        }
        else CHECK(eStatus == XHTTP_COMPLETE, "Once the body ended the message is complete");

        CHECK(XHTTP_AppendData(&http, (uint8_t*)sWire + nSplit, nWire - nSplit) > 0, "Buffer the rest");
        CHECK(XHTTP_Parse(&http) == XHTTP_COMPLETE, "The whole stream completes the message");
        CHECK(http.nContentLength == nData && !memcmp(XHTTP_GetBody(&http), g_chunkedData, nData),
            "With its body decoded once, wherever the split was");
        CHECK(XHTTP_GetExtraSize(&http) == sizeof(g_chunkedNext) - 1, "And the next request behind it");
        XHTTP_Clear(&http);
    }

    return 0;
}

static int XTest_chunked_malformed(void)
{
    /* Whatever two readers could disagree on is refused: a reader that took a lone LF or a short chunk
       for the end of a line would see a different message in the same bytes */
    const char *pRefused[] = {
        "G\r\n0\r\n\r\n",
        "\r\n0\r\n\r\n",
        ";ext\r\n0\r\n\r\n",
        "5\nabcde\r\n0\r\n\r\n",
        "5\rXabcde\r\n0\r\n\r\n",
        "5 x\r\nabcde\r\n0\r\n\r\n",
        "5;a\nb\r\nabcde\r\n0\r\n\r\n",
        "5;a\x01\r\nabcde\r\n0\r\n\r\n",
        "5;a\x7f\r\nabcde\r\n0\r\n\r\n",
        "5\r\nabcdeX\r\n0\r\n\r\n",
        "5\r\nabcdeX\n0\r\n\r\n",
        "5\r\nabcde\n\n0\r\n\r\n",
        "5\r\nabcde\n0\r\n\r\n",
        "5\r\nabcde\rX0\r\n\r\n",
        "-5\r\nabcde\r\n0\r\n\r\n",
        "0x5\r\nabcde\r\n0\r\n\r\n",
        "10000000000000000\r\n",
        "0\r\n\n",
        "0\r\n\rX",
        "0\r\nX: y\nZ: 1\r\n\r\n",
        "0\r\nX: \x01\r\n\r\n",
        "0\r\nX: y\rZ\r\n\r\n"
    };

    char sWire[256];
    size_t nHead = sizeof(g_chunkedHead) - 1;
    memcpy(sWire, g_chunkedHead, nHead);

    for (size_t i = 0; i < sizeof(pRefused) / sizeof(*pRefused); i++)
    {
        size_t nBody = strlen(pRefused[i]);
        memcpy(sWire + nHead, pRefused[i], nBody);

        xhttp_t http;
        CHECK(XHTTP_ParseData(&http, (uint8_t*)sWire, nHead + nBody) == XHTTP_INVALID, "Malformed chunks are refused");
        CHECK(http.rawData.nUsed == nHead + nBody && !memcmp(http.rawData.pData, sWire, nHead + nBody),
            "And not decoded at all");
        XHTTP_Clear(&http);

        xhttp_chunked_t scan;
        XHTTP_InitChunked(&scan);
        CHECK(XHTTP_ScanChunked(&scan, (uint8_t*)pRefused[i], nBody) == XHTTP_INVALID, "The scan refuses them too");
    }

    /* A size line and the trailer section are bounded, however they are padded */
    const size_t nLine = 4096;
    char *pLong = (char*)malloc(nHead + 40000);
    CHECK(pLong != NULL, "Allocate the long lines");

    const char *pFill[] = {"0", ";"};
    for (int nForm = 0; nForm < 3; nForm++)
    {
        size_t nSize = nHead;
        memcpy(pLong, g_chunkedHead, nHead);

        if (nForm < 2)
        {
            /* A size of one after a run of zeros, which overflow nothing, or before a run of extension bytes */
            if (nForm == 1) pLong[nSize++] = '1';
            for (size_t i = 0; i < nLine; i++) pLong[nSize++] = pFill[nForm][0];
            if (nForm == 0) pLong[nSize++] = '1';
            memcpy(pLong + nSize, "\r\nz\r\n0\r\n\r\n", 10);
            nSize += 10;
        }
        else
        {
            memcpy(pLong + nSize, "0\r\nX: ", 6);
            nSize += 6;
            for (size_t i = 0; i < 32 * 1024; i++) pLong[nSize++] = 'y';
            memcpy(pLong + nSize, "\r\n\r\n", 4);
            nSize += 4;
        }

        xhttp_t http;
        CHECK(XHTTP_ParseData(&http, (uint8_t*)pLong, nSize) == XHTTP_INVALID, "An overlong line is refused");
        XHTTP_Clear(&http);

        /* One byte shorter, it is as long as a line can be */
        if (nForm < 2)
        {
            memmove(pLong + nHead + 1, pLong + nHead + 2, nSize - nHead - 2);
            CHECK(XHTTP_ParseData(&http, (uint8_t*)pLong, nSize - 1) == XHTTP_COMPLETE, "A line at the limit is not");
            CHECK(http.nContentLength == 1 && XHTTP_GetBody(&http)[0] == 'z', "And frames its chunk");
            XHTTP_Clear(&http);
        }
    }

    free(pLong);
    return 0;
}

static int XTest_chunked_scan(void)
{
    const uint8_t *pBody = (const uint8_t*)g_chunkedBody;
    size_t nBody = sizeof(g_chunkedBody) - 1;
    xhttp_chunked_t scan;

    /* The scan goes on from where it stopped, so a body that arrives a byte at a time is looked at once */
    XHTTP_InitChunked(&scan);
    for (size_t i = 0; i < nBody; i++)
    {
        CHECK(XHTTP_ScanChunked(&scan, pBody, i) == XHTTP_INCOMPLETE, "No prefix of the body ends it");
        CHECK(scan.nPosit == i, "Every byte so far was taken, and none twice");
    }

    CHECK(XHTTP_ScanChunked(&scan, pBody, nBody) == XHTTP_COMPLETE, "The last byte ends it");
    CHECK(scan.nPosit == nBody && scan.nBody == sizeof(g_chunkedData) - 1 && scan.nLeft == 0,
        "With the whole wire size and the body size it carried");
    CHECK(XHTTP_ScanChunked(&scan, pBody, nBody) == XHTTP_COMPLETE, "An ended body stays ended");

    /* It stops at the end of the body: what follows is not looked at */
    char sMore[512];
    snprintf(sMore, sizeof(sMore), "%s%s", g_chunkedBody, "garbage that is no chunk");
    XHTTP_InitChunked(&scan);
    CHECK(XHTTP_ScanChunked(&scan, (const uint8_t*)sMore, strlen(sMore)) == XHTTP_COMPLETE && scan.nPosit == nBody,
        "The scan ends with the body");

    /* The data of a chunk is skipped, not looked at */
    XHTTP_InitChunked(&scan);
    const char sBig[] = "fffff\r\n0123456789";
    CHECK(XHTTP_ScanChunked(&scan, (const uint8_t*)sBig, sizeof(sBig) - 1) == XHTTP_INCOMPLETE, "A big chunk waits");
    CHECK(scan.nLeft == 0xfffff - 10 && scan.nBody == 10, "For the rest of its data");

    /* Refused once, refused for good */
    XHTTP_InitChunked(&scan);
    CHECK(XHTTP_ScanChunked(&scan, (const uint8_t*)"zz", 2) == XHTTP_INVALID, "Malformed coding is refused");
    CHECK(XHTTP_ScanChunked(&scan, pBody, nBody) == XHTTP_INVALID, "And stays refused");

    /* The guards */
    XHTTP_InitChunked(NULL);
    CHECK(XHTTP_ScanChunked(NULL, pBody, nBody) == XHTTP_INVALID, "No scan state is refused");
    XHTTP_InitChunked(&scan);
    CHECK(XHTTP_ScanChunked(&scan, NULL, 0) == XHTTP_INCOMPLETE, "Nothing yet is incomplete");
    CHECK(XHTTP_ScanChunked(&scan, NULL, 4) == XHTTP_INVALID, "Bytes that are not there are refused");
    CHECK(XHTTP_ScanChunked(&scan, pBody, 4) == XHTTP_INCOMPLETE && scan.nPosit == 4, "Four bytes are taken");
    CHECK(XHTTP_ScanChunked(&scan, pBody, 2) == XHTTP_INVALID, "A body shorter than what was taken is refused");
    return 0;
}

static int XTest_chunked_receive(void)
{
    char sWire[256];
    xsock_t writer, reader;
    framing_cb_t cb;
    xhttp_t http;

    /* Across the header read and the content read, with the next message read along with the end of the body */
    CHECK(framing_pair(&writer, &reader) == XSTDOK, "Create a stream");
    snprintf(sWire, sizeof(sWire), "%s4\r\nWi", g_chunkedHead);
    CHECK(framing_send(&writer, sWire) == XSTDOK, "Send the header and part of the first chunk");
    XHTTP_InitParser(&http, NULL, 0);
    CHECK(XHTTP_ReadHeader(&http, &reader) == XHTTP_PARSED && http.eFraming == XHTTP_FRAME_CHUNKED, "The header is read");
    CHECK(XHTTP_GetBodySize(&http) == 2 && framing_body_is(&http, "Wi", 2), "The chunk data so far is decoded at once");
    CHECK(framing_send(&writer, "ki\r\n5\r\npedia\r\n0\r\nX: y\r\n\r\nHTTP/1.1 204 No Content\r\n\r\n") == XSTDOK,
        "Send the rest, and the next message");
    CHECK(XHTTP_ReadContent(&http, &reader) == XHTTP_COMPLETE, "The body is read to its end");
    CHECK(http.nContentLength == 9 && XHTTP_GetPacketSize(&http) == http.nHeaderLength + 9 &&
        framing_body_is(&http, "Wikipedia", 9), "Decoded");
    CHECK(XHTTP_GetExtraSize(&http) == 27 && !memcmp(XHTTP_GetExtraData(&http), "HTTP/1.1 204", 12),
        "The next message read with it is kept behind it");
    CHECK(XHTTP_ReadContent(&http, &reader) == XHTTP_COMPLETE, "A complete body reads as complete");
    XHTTP_Clear(&http);

    /* A stream that ends before the last chunk is a body cut short */
    snprintf(sWire, sizeof(sWire), "%s5\r\nab", g_chunkedHead);
    CHECK(framing_send(&writer, sWire) == XSTDOK, "Send a chunk cut short");
    XSock_Close(&writer);
    XHTTP_InitParser(&http, NULL, 0);
    CHECK(XHTTP_Receive(&http, &reader) == XHTTP_EREAD, "The end of the stream does not end the body");
    CHECK(framing_body_is(&http, "ab", 2) && !http.nComplete, "What arrived is kept, and not complete");
    XHTTP_Clear(&http);
    XSock_Close(&reader);

    /* Malformed chunks after the header read */
    CHECK(framing_pair(&writer, &reader) == XSTDOK, "Create a stream");
    snprintf(sWire, sizeof(sWire), "%s5\r\nab", g_chunkedHead);
    CHECK(framing_send(&writer, sWire) == XSTDOK, "Send the start of a chunk");
    XHTTP_InitParser(&http, NULL, 0);
    CHECK(XHTTP_ReadHeader(&http, &reader) == XHTTP_PARSED, "The header is read");
    CHECK(framing_send(&writer, "cdeXX") == XSTDOK, "Send the rest without its CRLF");
    CHECK(XHTTP_ReadContent(&http, &reader) == XHTTP_INVALID, "Malformed chunks are refused while they are read");
    XHTTP_Clear(&http);

    /* The content limit holds for decoded bytes */
    snprintf(sWire, sizeof(sWire), "%s", g_chunkedHead);
    CHECK(framing_send(&writer, sWire) == XSTDOK, "Send a header");
    XHTTP_InitParser(&http, NULL, 0);
    http.nContentMax = sizeof(g_chunkedHead) + 16;
    CHECK(XHTTP_ReadHeader(&http, &reader) == XHTTP_PARSED, "The header is read");
    CHECK(framing_send(&writer, "20\r\n0123456789abcdef0123456789abcdef\r\n0\r\n\r\n") == XSTDOK, "Send a large chunk");
    CHECK(XHTTP_ReadContent(&http, &reader) == XHTTP_BIGCNT, "A body past the limit is refused");
    XHTTP_Clear(&http);

    /* The callback can end the body early or stop the read, in the middle of a chunk */
    for (int nStop = 0; nStop < 2; nStop++)
    {
        CHECK(framing_send(&writer, sWire) == XSTDOK, "Send a header");
        XHTTP_InitParser(&http, NULL, 0);
        framing_watch(&http, &cb, XSTDUSR);
        cb.nStopAfter = 3;
        cb.nStopAnswer = nStop ? XSTDERR : XSTDNON;
        CHECK(XHTTP_ReadHeader(&http, &reader) == XHTTP_PARSED, "The header is read");
        CHECK(framing_send(&writer, "5\r\nabcde\r\n0\r\n\r\n") == XSTDOK, "Send the body");
        CHECK(XHTTP_ReadContent(&http, &reader) == (nStop ? XHTTP_TERMINATED : XHTTP_COMPLETE),
            "The callback has the last word");
        CHECK(cb.content.nUsed == 5 && !memcmp(cb.content.pData, "abcde", 5), "On decoded bytes");
        XByteBuffer_Clear(&cb.content);
        XHTTP_Clear(&http);

        /* Drain what the stopped read left */
        char sDrain[64];
        XSock_TimeOutR(&reader, 0, 100000);
        while (XSock_Read(&reader, sDrain, sizeof(sDrain)) > 0);
        XSock_Close(&reader);
        XSock_Close(&writer);
        CHECK(framing_pair(&writer, &reader) == XSTDOK, "Create a stream");
    }

    /* A body parsed from a buffer and then read from the socket: its chunks that were buffered are decoded first,
       whether the rest comes from the socket or was appended in the meantime */
    snprintf(sWire, sizeof(sWire), "%s3\r\nab", g_chunkedHead);
    for (int nForm = 0; nForm < 3; nForm++)
    {
        CHECK(XHTTP_InitParser(&http, (uint8_t*)sWire, strlen(sWire)) > 0, "Buffer the start of a body");
        CHECK(XHTTP_Parse(&http) == XHTTP_PARSED && !http.chunked.nPosit, "It is parsed, its chunks left as they are");

        xhttp_status_t eExpect = XHTTP_COMPLETE;
        if (nForm == 0) CHECK(framing_send(&writer, "c\r\n0\r\n\r\n") == XSTDOK, "The rest is in the socket");
        else if (nForm == 1) CHECK(XHTTP_AppendData(&http, (uint8_t*)"c\r\n0\r\n\r\n", 8) > 0, "Or already buffered");
        else
        {
            CHECK(XHTTP_AppendData(&http, (uint8_t*)"cXX", 3) > 0, "Or buffered and malformed");
            eExpect = XHTTP_INVALID;
        }

        CHECK(XHTTP_ReadContent(&http, &reader) == eExpect, "The content read takes it from there");
        if (eExpect == XHTTP_COMPLETE) CHECK(http.nContentLength == 3 && framing_body_is(&http, "abc", 3), "Decoded");
        XHTTP_Clear(&http);
    }

    /* Without blocking: nothing to read is a read that would block, part of the body is incomplete */
    CHECK(XSock_NonBlock(&reader, XTRUE) != XSOCK_INVALID, "Stop blocking");
    CHECK(framing_send(&writer, sWire) == XSTDOK, "Send the header and part of a chunk");
    XHTTP_InitParser(&http, NULL, 0);
    CHECK(XHTTP_ReadHeader(&http, &reader) == XHTTP_PARSED, "The header is read");
    CHECK(XHTTP_ReadContent(&http, &reader) == XHTTP_EREAD && XSock_Status(&reader) == XSOCK_WANT_READ,
        "Nothing more is a read that would block");
    CHECK(framing_send(&writer, "c\r\n") == XSTDOK && XHTTP_ReadContent(&http, &reader) == XHTTP_INCOMPLETE,
        "The end of a chunk is not the end of the body");
    CHECK(framing_send(&writer, "0\r\n\r\n") == XSTDOK && XHTTP_ReadContent(&http, &reader) == XHTTP_COMPLETE,
        "The last chunk is");
    CHECK(http.nContentLength == 3 && framing_body_is(&http, "abc", 3) && !XHTTP_GetExtraSize(&http), "Decoded");
    XHTTP_Clear(&http);

    XSock_Close(&writer);
    XSock_Close(&reader);
    return 0;
}

static int XTest_chunked_stream(void)
{
    /* A body of many chunks of every size, with CR and LF all through its data, read in many pieces. Claimed by
       the callback it reaches the callback whole and is not kept; left to the handle, it is kept whole. */
    enum { STREAM_BODY = 20000 };
    uint8_t *pData = (uint8_t*)malloc(STREAM_BODY);
    CHECK(pData != NULL, "Allocate the body");
    for (size_t i = 0; i < STREAM_BODY; i++) pData[i] = (uint8_t)(i * 7 + 3);

    xbyte_buffer_t wire;
    XByteBuffer_Init(&wire, 0, XTRUE);
    CHECK(XByteBuffer_AddFmt(&wire, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n") > 0, "Build the header");
    for (size_t nDone = 0, i = 0; nDone < STREAM_BODY; i++)
    {
        size_t nChunk = XSTD_MIN((i * 37) % 997 + 1, STREAM_BODY - nDone);
        CHECK(XByteBuffer_AddFmt(&wire, "%zx%s\r\n", nChunk, i % 3 ? "" : ";n=v") > 0, "Build a size line");
        CHECK(XByteBuffer_Add(&wire, pData + nDone, nChunk) > 0 && XByteBuffer_AddFmt(&wire, "\r\n") > 0, "And its data");
        nDone += nChunk;
    }
    CHECK(XByteBuffer_AddFmt(&wire, "0\r\n\r\n") > 0, "Build the last chunk");

    for (int nClaim = 0; nClaim < 2; nClaim++)
    {
        xsock_t writer, reader;
        CHECK(framing_pair(&writer, &reader) == XSTDOK, "Create a stream");
        CHECK(framing_write(&writer, wire.pData, wire.nUsed) == XSTDOK, "Send the whole response");
        XSock_Close(&writer);

        xhttp_t http;
        framing_cb_t cb;
        XHTTP_InitParser(&http, NULL, 0);
        framing_watch(&http, &cb, nClaim ? XSTDOK : XSTDUSR);
        CHECK(XHTTP_Receive(&http, &reader) == XHTTP_COMPLETE, "The response is received");
        CHECK(cb.content.nUsed == STREAM_BODY && !memcmp(cb.content.pData, pData, STREAM_BODY),
            "The callback saw every body byte in order, and no framing");

        if (nClaim)
        {
            CHECK(XHTTP_GetBodySize(&http) < XHTTP_RX_SIZE, "A claimed body is not kept");
            CHECK(http.nBodyClaimed + XHTTP_GetBodySize(&http) == STREAM_BODY, "But it is all counted");
        }
        else
        {
            CHECK(http.nContentLength == STREAM_BODY && XHTTP_GetBodySize(&http) == STREAM_BODY, "The body is kept");
            CHECK(!memcmp(XHTTP_GetBody(&http), pData, STREAM_BODY), "Decoded");
        }

        CHECK(cb.nCalls > 1, "In many pieces");
        XByteBuffer_Clear(&cb.content);
        XHTTP_Clear(&http);
        XSock_Close(&reader);
    }

    XByteBuffer_Clear(&wire);
    free(pData);
    return 0;
}

static int XTest_length_invalid(void)
{
    /* Read from a socket, a message whose length is not one is refused once its header is in, and its content
       callback never sees the bytes after it: a reader that took them for a whole message of nothing would hand
       them on as the next one */
    const char *pWires[] = {
        "POST /test HTTP/1.1\r\nHost: example.com\r\nContent-Length: banana\r\n\r\nhello",
        "HTTP/1.1 200 OK\r\nContent-Length: banana\r\n\r\nhello",
        "HTTP/1.1 200 OK\r\nContent-Length: 18446744073709551616\r\n\r\nhello",
        "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 5x\r\n\r\nhello"
    };

    for (size_t i = 0; i < sizeof(pWires) / sizeof(*pWires); i++)
    {
        xhttp_t http;
        framing_cb_t cb;
        xhttp_status_t eStatus = framing_receive(&http, &cb, pWires[i], strlen(pWires[i]));

        CHECK(eStatus == XHTTP_INVALID, "The message is invalid, not a complete one with no body");
        CHECK(cb.nCalls == 0 && cb.content.nUsed == 0, "And the callback is given none of what follows it");

        XByteBuffer_Clear(&cb.content);
        XHTTP_Clear(&http);
    }

    return 0;
}

static int XTest_stop_with_header(void)
{
    /* A content callback can stop at the body bytes that came in with the header: the message is complete there,
       and nothing more is read. The writer is gone, so a reader that went on would find the end of the stream
       short of the length, and fail. */
    xsock_t writer, reader;
    CHECK(framing_pair(&writer, &reader) == XSTDOK, "Create a stream");
    CHECK(framing_send(&writer, "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nhello") == XSTDOK,
        "Send a header and half of its body");
    XSock_Close(&writer);

    xhttp_t http;
    framing_cb_t cb;
    XHTTP_InitParser(&http, NULL, 0);
    framing_watch(&http, &cb, XSTDNON);

    CHECK(XHTTP_Receive(&http, &reader) == XHTTP_COMPLETE && http.nComplete, "Stopping at once completes the message");
    CHECK(cb.nCalls == 1 && cb.content.nUsed == 5 && !memcmp(cb.content.pData, "hello", 5),
        "The callback had the bytes that came with the header, once");

    XByteBuffer_Clear(&cb.content);
    XHTTP_Clear(&http);
    XSock_Close(&reader);
    return 0;
}

static int XTest_length_bounded(void)
{
    /* A Content-Length body is read to its length and no further: the next message stays in the socket for
       whoever reads it, rather than vanishing into this message's buffer */
    xsock_t writer, reader;
    CHECK(framing_pair(&writer, &reader) == XSTDOK, "Create a stream");
    CHECK(framing_send(&writer, "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n") == XSTDOK, "Send a header");

    xhttp_t first;
    XHTTP_InitParser(&first, NULL, 0);
    CHECK(XHTTP_ReadHeader(&first, &reader) == XHTTP_PARSED, "The header is read");
    CHECK(framing_send(&writer, "helloHTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nbye") == XSTDOK,
        "Send the body and the next response together");
    CHECK(XHTTP_ReadContent(&first, &reader) == XHTTP_COMPLETE, "The body is read");
    CHECK(XHTTP_GetBodySize(&first) == 5 && framing_body_is(&first, "hello", 5), "Exactly the body");
    CHECK(XHTTP_GetExtraSize(&first) == 0, "Nothing past it was taken from the socket");

    xhttp_t second;
    XHTTP_InitParser(&second, NULL, 0);
    CHECK(XHTTP_Receive(&second, &reader) == XHTTP_COMPLETE && framing_body_is(&second, "bye", 3),
        "The next response is read whole from the socket");
    XHTTP_Clear(&second);
    XHTTP_Clear(&first);

    /* When the next message came with the header, it is kept behind the body instead */
    CHECK(framing_send(&writer, "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhelloHTTP/1.1 204 No Content\r\n\r\n") == XSTDOK,
        "Send two responses together");
    framing_cb_t cb;
    XHTTP_InitParser(&first, NULL, 0);
    framing_watch(&first, &cb, XSTDUSR);
    CHECK(XHTTP_Receive(&first, &reader) == XHTTP_COMPLETE, "The first response is received");
    CHECK(cb.content.nUsed == 5 && !memcmp(cb.content.pData, "hello", 5), "Its callback saw its body alone");
    CHECK(XHTTP_GetExtraSize(&first) == 27 && !memcmp(XHTTP_GetExtraData(&first), "HTTP/1.1 204", 12),
        "The next one is kept behind it");
    XByteBuffer_Clear(&cb.content);
    XHTTP_Clear(&first);

    XSock_Close(&writer);
    XSock_Close(&reader);
    return 0;
}

static int XTest_length_claimed(void)
{
    /* Bytes the callback claims are counted toward the length across reads that do not block: a later read
       asks only for what is still owed, and stops where the body does */
    xsock_t writer, reader;
    CHECK(framing_pair(&writer, &reader) == XSTDOK, "Create a stream");
    CHECK(XSock_NonBlock(&reader, XTRUE) != XSOCK_INVALID, "Stop blocking");
    CHECK(framing_send(&writer, "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n") == XSTDOK, "Send a header");

    xhttp_t http;
    framing_cb_t cb;
    XHTTP_InitParser(&http, NULL, 0);
    framing_watch(&http, &cb, XSTDOK);
    CHECK(XHTTP_ReadHeader(&http, &reader) == XHTTP_PARSED, "The header is read");
    CHECK(XHTTP_ReadContent(&http, &reader) == XHTTP_EREAD && XSock_Status(&reader) == XSOCK_WANT_READ,
        "Nothing more is a read that would block");

    CHECK(framing_send(&writer, "he") == XSTDOK, "Send part of the body");
    CHECK(XHTTP_ReadContent(&http, &reader) == XHTTP_INCOMPLETE && http.nBodyClaimed == 2, "Part is claimed");
    CHECK(framing_send(&writer, "lloHTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nbye") == XSTDOK,
        "Send the rest and the next response");
    CHECK(XHTTP_ReadContent(&http, &reader) == XHTTP_COMPLETE && http.nBodyClaimed == 5, "All of it is claimed");
    CHECK(cb.content.nUsed == 5 && !memcmp(cb.content.pData, "hello", 5) && !XHTTP_GetBodySize(&http),
        "The callback had it all, and the handle none");
    CHECK(XHTTP_ReadContent(&http, &reader) == XHTTP_COMPLETE, "A complete body reads as complete");
    XByteBuffer_Clear(&cb.content);
    XHTTP_Clear(&http);

    CHECK(XSock_NonBlock(&reader, XFALSE) != XSOCK_INVALID, "Block again");
    XHTTP_InitParser(&http, NULL, 0);
    CHECK(XHTTP_Receive(&http, &reader) == XHTTP_COMPLETE && framing_body_is(&http, "bye", 3),
        "The next response was left in the socket");
    XHTTP_Clear(&http);

    XSock_Close(&writer);
    XSock_Close(&reader);
    return 0;
}

static int XTest_close_eof(void)
{
    const char sHeader[] = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\n";
    xsock_t writer, reader;
    framing_cb_t cb;
    xhttp_t http;

    /* The end of the stream ends the body */
    CHECK(framing_pair(&writer, &reader) == XSTDOK, "Create a stream");
    CHECK(framing_send(&writer, sHeader) == XSTDOK && framing_send(&writer, "bo") == XSTDOK, "Send the start");
    XHTTP_InitParser(&http, NULL, 0);
    CHECK(XHTTP_ReadHeader(&http, &reader) == XHTTP_PARSED && http.eFraming == XHTTP_FRAME_CLOSE, "The header is read");
    CHECK(!http.nKeepAlive, "A body like that leaves no connection to keep");
    CHECK(framing_send(&writer, "dy") == XSTDOK, "Send the rest");
    XSock_Close(&writer);
    CHECK(XHTTP_ReadContent(&http, &reader) == XHTTP_COMPLETE && XHTTP_GetBodySize(&http) == 4 &&
        framing_body_is(&http, "body", 4), "The end of the stream completes it");
    XHTTP_Clear(&http);
    XSock_Close(&reader);

    /* A read that fails before the end, here because it timed out, does not: it used to complete an empty body */
    CHECK(framing_pair(&writer, &reader) == XSTDOK, "Create a stream");
    XSock_TimeOutR(&reader, 0, 200000);
    CHECK(framing_send(&writer, sHeader) == XSTDOK, "Send only the header");
    XHTTP_InitParser(&http, NULL, 0);
    CHECK(XHTTP_Receive(&http, &reader) == XHTTP_EREAD && !http.nComplete, "A timed out read is no end of the body");
    XHTTP_Clear(&http);
    XSock_Close(&writer);
    XSock_Close(&reader);

    /* Nor does a read that would block, before any of the body or after part of it */
    CHECK(framing_pair(&writer, &reader) == XSTDOK, "Create a stream");
    CHECK(XSock_NonBlock(&reader, XTRUE) != XSOCK_INVALID, "Stop blocking");
    CHECK(framing_send(&writer, sHeader) == XSTDOK, "Send only the header");
    XHTTP_InitParser(&http, NULL, 0);
    framing_watch(&http, &cb, XSTDOK);
    CHECK(XHTTP_ReadHeader(&http, &reader) == XHTTP_PARSED, "The header is read");
    CHECK(XHTTP_ReadContent(&http, &reader) == XHTTP_EREAD && XSock_Status(&reader) == XSOCK_WANT_READ,
        "No body yet is a read that would block, not an empty body");
    CHECK(framing_send(&writer, "abc") == XSTDOK && XHTTP_ReadContent(&http, &reader) == XHTTP_INCOMPLETE,
        "Part of it is incomplete");
    XSock_Close(&writer);
    CHECK(XHTTP_ReadContent(&http, &reader) == XHTTP_COMPLETE && http.nComplete, "The end of the stream completes it");
    CHECK(cb.content.nUsed == 3 && http.nBodyClaimed == 3 && !XHTTP_GetBodySize(&http), "The callback had it all");
    XByteBuffer_Clear(&cb.content);
    XHTTP_Clear(&http);
    XSock_Close(&reader);

    /* The content limit holds without blocking too: it used to be checked only on a socket that blocks */
    CHECK(framing_pair(&writer, &reader) == XSTDOK, "Create a stream");
    CHECK(XSock_NonBlock(&reader, XTRUE) != XSOCK_INVALID, "Stop blocking");
    CHECK(framing_send(&writer, sHeader) == XSTDOK, "Send the header");
    XHTTP_InitParser(&http, NULL, 0);
    http.nContentMax = sizeof(sHeader) + 8;
    CHECK(XHTTP_ReadHeader(&http, &reader) == XHTTP_PARSED, "The header is read");
    CHECK(framing_send(&writer, "0123456789abcdef") == XSTDOK, "Send a body past the limit");
    CHECK(XHTTP_ReadContent(&http, &reader) == XHTTP_BIGCNT, "It is refused");
    XHTTP_Clear(&http);
    XSock_Close(&writer);
    XSock_Close(&reader);

    /* An end of the stream the caller already read ends the body; a socket closed for any other reason does not */
    for (int nEnd = 0; nEnd < 2; nEnd++)
    {
        CHECK(framing_pair(&writer, &reader) == XSTDOK, "Create a stream");
        CHECK(framing_send(&writer, sHeader) == XSTDOK, "Send the header");
        XHTTP_InitParser(&http, NULL, 0);
        CHECK(XHTTP_ReadHeader(&http, &reader) == XHTTP_PARSED, "The header is read");

        if (nEnd)
        {
            char sByte;
            XSock_Close(&writer);
            CHECK(XSock_Read(&reader, &sByte, 1) == 0 && XSock_Status(&reader) == XSOCK_EOF, "The end is read");
        }
        else XSock_Close(&reader);

        CHECK(!XSock_IsOpen(&reader), "The socket is closed either way");
        CHECK(XHTTP_ReadContent(&http, &reader) == (nEnd ? XHTTP_COMPLETE : XHTTP_EREAD), "Only the end ends the body");
        XHTTP_Clear(&http);
        XSock_Close(&writer);
        XSock_Close(&reader);
    }

    return 0;
}

static int XTest_tls_close_notify(void)
{
#ifdef XSOCK_USE_SSL
    /* Over TLS the peer's close_notify is the end of a body that ends with the connection */
    tls_pair_t tls;
    if (tls_pair_open(&tls) != XSTDOK) return tls_pair_skip(&tls);

    CHECK(tls_pair_answer(&tls, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\nall of it",
        TLS_PAIR_NOTIFY) == XSTDOK, "The server answers and closes the session");
    xhttp_t http;
    XHTTP_InitParser(&http, NULL, 0);
    CHECK(XHTTP_Receive(&http, &tls.client) == XHTTP_COMPLETE, "The close_notify ends the body");
    CHECK(XHTTP_GetBodySize(&http) == 9 && framing_body_is(&http, "all of it", 9), "All of it");
    XHTTP_Clear(&http);
    tls_pair_close(&tls);
    return 0;
#else
    printf("Built without TLS, skipping\n");
    return 77;
#endif
}

static int XTest_tls_truncated(void)
{
#if defined(XSOCK_USE_SSL) && defined(SSL_OP_IGNORE_UNEXPECTED_EOF)
    /* An end of the transport without close_notify could be anyone on the path cutting the body short: such
       a body is not complete (RFC 9112, 9.8), though the bytes that arrived are kept */
    tls_pair_t tls;
    if (tls_pair_open(&tls) != XSTDOK) return tls_pair_skip(&tls);

    SSL *pSSL = XSock_GetSSL(&tls.client);
    CHECK(SSL_get_options(pSSL) & SSL_OP_IGNORE_UNEXPECTED_EOF, "Sockets ignore such an end by default");
    CHECK(tls_pair_answer(&tls, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\nall of i",
        TLS_PAIR_CUT) == XSTDOK, "The transport ends in the middle of the body");

    xhttp_t http;
    XHTTP_InitParser(&http, NULL, 0);
    CHECK(XHTTP_Receive(&http, &tls.client) == XHTTP_EREAD && !http.nComplete, "A cut stream does not end the body");
    CHECK(XHTTP_GetBodySize(&http) == 8 && framing_body_is(&http, "all of i", 8), "What arrived is kept");
    XHTTP_Clear(&http);
    tls_pair_close(&tls);
    return 0;
#else
    printf("No way to tell a cut TLS stream from a closed one here, skipping\n");
    return 77;
#endif
}

static int XTest_tls_length(void)
{
#ifdef XSOCK_USE_SSL
    /* A body that ends by its length or its last chunk does not care how the stream ends after it */
    const char *pAnswers[] = {
        "HTTP/1.1 200 OK\r\nContent-Length: 9\r\n\r\nall of it",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n9\r\nall of it\r\n0\r\n\r\n"
    };

    for (size_t i = 0; i < sizeof(pAnswers) / sizeof(*pAnswers); i++)
    {
        tls_pair_t tls;
        if (tls_pair_open(&tls) != XSTDOK) return tls_pair_skip(&tls);
        CHECK(tls_pair_answer(&tls, pAnswers[i], TLS_PAIR_CUT) == XSTDOK, "The server answers and stops");

        xhttp_t http;
        XHTTP_InitParser(&http, NULL, 0);
        CHECK(XHTTP_Receive(&http, &tls.client) == XHTTP_COMPLETE, "The framed body completes");
        CHECK(http.nContentLength == 9 && framing_body_is(&http, "all of it", 9), "Whole");
        XHTTP_Clear(&http);
        tls_pair_close(&tls);
    }

    return 0;
#else
    printf("Built without TLS, skipping\n");
    return 77;
#endif
}

static int XTest_tls_option(void)
{
#if defined(XSOCK_USE_SSL) && defined(SSL_OP_IGNORE_UNEXPECTED_EOF)
    /* A body read that stops before the end leaves the socket as it found it */
    tls_pair_t tls;
    if (tls_pair_open(&tls) != XSTDOK) return tls_pair_skip(&tls);

    CHECK(tls_pair_answer(&tls, "HTTP/1.1 200 OK\r\n\r\npart", TLS_PAIR_OPEN) == XSTDOK, "The server answers part");
    xhttp_t http;
    XHTTP_InitParser(&http, NULL, 0);
    CHECK(XHTTP_ReadHeader(&http, &tls.client) == XHTTP_PARSED, "The header is read");

    CHECK(XSock_NonBlock(&tls.client, XTRUE) != XSOCK_INVALID, "Stop blocking");
    CHECK(XHTTP_ReadContent(&http, &tls.client) == XHTTP_EREAD && XSock_Status(&tls.client) == XSOCK_WANT_READ,
        "The read stops where nothing more has arrived");
    CHECK(SSL_get_options(XSock_GetSSL(&tls.client)) & SSL_OP_IGNORE_UNEXPECTED_EOF, "The socket is as it was");

    CHECK(tls_pair_answer(&tls, "rest", TLS_PAIR_NOTIFY) == XSTDOK, "The server sends the rest and closes");
    CHECK(XSock_NonBlock(&tls.client, XFALSE) != XSOCK_INVALID, "Block again");
    CHECK(XHTTP_ReadContent(&http, &tls.client) == XHTTP_COMPLETE && framing_body_is(&http, "partrest", 8),
        "The body completes at the close_notify");
    XHTTP_Clear(&http);
    tls_pair_close(&tls);
    return 0;
#else
    printf("No way to tell a cut TLS stream from a closed one here, skipping\n");
    return 77;
#endif
}

XTEST_MAIN(
    XTEST_CASE(framing_rules),
    XTEST_CASE(framed_callback),
    XTEST_CASE(chunked_parse),
    XTEST_CASE(chunked_split),
    XTEST_CASE(chunked_malformed),
    XTEST_CASE(chunked_scan),
    XTEST_CASE(chunked_receive),
    XTEST_CASE(chunked_stream),
    XTEST_CASE(length_invalid),
    XTEST_CASE(stop_with_header),
    XTEST_CASE(length_bounded),
    XTEST_CASE(length_claimed),
    XTEST_CASE(close_eof),
    XTEST_CASE(tls_close_notify),
    XTEST_CASE(tls_truncated),
    XTEST_CASE(tls_length),
    XTEST_CASE(tls_option)
)
