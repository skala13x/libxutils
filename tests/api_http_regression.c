/* libxutils: HTTP stream boundaries and exact credential validation through XAPI. */
#include "test.h"
#include "api.h"

typedef struct xtest_http_ {
    xapi_t api;
    xapi_session_t *pSession;
    xsock_t peer;
    char uris[4][64];
    size_t lengths[4];
    uint8_t bodies[4][8];
    int nReads;
    int nClosed;
    int nInvalid;
    int nCounted;
    xbool_t bCountOnly;
} xtest_http_t;

static int XTest_Callback(xapi_ctx_t *pContext, xapi_session_t *pSession)
{
    xtest_http_t *pTest = (xtest_http_t*)pContext->pApi->pUserCtx;
    if (pContext->eCbType == XAPI_CB_REGISTERED) pTest->pSession = pSession;
    else if (pContext->eCbType == XAPI_CB_CLOSED)
    {
        pTest->pSession = NULL;
        pTest->nClosed++;
    }
    else if (pContext->eCbType == XAPI_CB_READ && pTest->bCountOnly)
    {
        xhttp_t *pHTTP = (xhttp_t*)pSession->pPacket;
        if (pHTTP == NULL || strcmp(pHTTP->sUri, "/burst") != 0) pTest->nInvalid++;
        else pTest->nCounted++;
    }
    else if (pContext->eCbType == XAPI_CB_READ)
    {
        xhttp_t *pHTTP = (xhttp_t*)pSession->pPacket;
        if (!pHTTP || pTest->nReads >= 4 || pHTTP->nContentLength > sizeof(pTest->bodies[0]))
        {
            pTest->nInvalid++;
            return XAPI_DISCONNECT;
        }
        int nIndex = pTest->nReads++;
        xstrncpy(pTest->uris[nIndex], sizeof(pTest->uris[nIndex]), pHTTP->sUri);
        pTest->lengths[nIndex] = pHTTP->nContentLength;
        if (pHTTP->nContentLength) memcpy(pTest->bodies[nIndex], XHTTP_GetBody(pHTTP), pHTTP->nContentLength);
    }
    return XAPI_CONTINUE;
}

static int XTest_Open(xtest_http_t *pTest)
{
    XSOCKET pair[2];
    CHECK(XSock_CreatePair(pair) == XSTDOK, "Create a private HTTP stream");
    CHECK(XSock_Init(&pTest->peer, XSOCK_TCP_PEER, pair[1]) != XSOCK_ERROR, "Own the remote descriptor");
    CHECK(XAPI_Init(&pTest->api, XTest_Callback, pTest) == XSTDOK, "Initialize API HTTP receiver");
    xapi_endpoint_t endpoint;
    XAPI_InitEndpoint(&endpoint);
    endpoint.eType = XAPI_HTTP;
    endpoint.eRole = XAPI_PEER;
    endpoint.nFD = pair[0];
    endpoint.nEvents = XPOLLIN;
    CHECK(XAPI_AddEndpoint(&pTest->api, &endpoint) == XSTDOK && pTest->pSession, "Register HTTP receiver");
    CHECK(XSock_NonBlock(&pTest->pSession->sock, XTRUE) != XSOCK_INVALID, "Keep the receiver nonblocking");
    return 0;
}

static void XTest_Close(xtest_http_t *pTest)
{
    XAPI_Destroy(&pTest->api);
    XSock_Close(&pTest->peer);
}

static int XTest_http_partial(void)
{
    xtest_http_t test = {0};
    CHECK(XTest_Open(&test) == 0, "Create a fragmented request fixture");
    const char wire[] = "POST /binary HTTP/1.1\r\nContent-Length: 3\r\nConnection: keep-alive\r\n\r\na\0b";
    for (size_t i = 0; i < sizeof(wire) - 1; i++)
    {
        CHECK(XSock_Write(&test.peer, wire + i, 1) == 1, "Send each header and body byte separately");
        CHECK(XAPI_Service(&test.api, 20) == XEVENTS_SUCCESS, "Service a fragmented HTTP stream");
        CHECK(test.nReads == (i == sizeof(wire) - 2 ? 1 : 0), "Deliver only after the final body byte arrives");
    }
    CHECK(!test.nInvalid && test.nClosed == 0 && test.pSession, "Fragmentation retains a valid live session");
    CHECK(strcmp(test.uris[0], "/binary") == 0 && memcmp(test.bodies[0], "a\0b", 3) == 0, "Retain URI and binary body");
    CHECK(test.pSession->bKeepAlive && test.pSession->rxBuffer.nUsed == 0, "Keep-alive consumes exactly one complete request");
    XTest_Close(&test);
    CHECK(test.nClosed == 1, "HTTP teardown closes its session once");
    return 0;
}

static int XTest_http_pipeline(void)
{
    xtest_http_t test = {0};
    CHECK(XTest_Open(&test) == 0, "Create a pipelined request fixture");
    const char wire[] = "POST /one HTTP/1.1\r\nContent-Length: 3\r\n\r\na\0b"
                        "GET /two HTTP/1.1\r\nContent-Length: 0\r\n\r\n"
                        "POST /three HTTP/1.1\r\nContent-Type: application/json\r\nContent-Length: 5\r\n\r\nab";
    CHECK(XSock_Write(&test.peer, wire, sizeof(wire) - 1) == sizeof(wire) - 1, "Send two requests and part of a third together");
    CHECK(XAPI_Service(&test.api, 20) == XEVENTS_SUCCESS, "Service one coalesced read");
    CHECK(test.nReads == 2 && !test.nInvalid && test.pSession, "Deliver both complete requests without another event");
    CHECK(strcmp(test.uris[0], "/one") == 0 && strcmp(test.uris[1], "/two") == 0, "Pipelining preserves request order");
    CHECK(test.lengths[0] == 3 && test.lengths[1] == 0, "Body lengths must not include the following request");
    CHECK(memcmp(test.bodies[0], "a\0b", 3) == 0 && test.pSession->rxBuffer.nUsed > 0, "Keep only the incomplete tail buffered");
    CHECK(XSock_Write(&test.peer, "cde", 3) == 3, "Finish the final body");
    CHECK(XAPI_Service(&test.api, 20) == XEVENTS_SUCCESS, "Service the remaining body bytes");
    CHECK(test.nReads == 3 && strcmp(test.uris[2], "/three") == 0, "Deliver the third request exactly once");
    CHECK(memcmp(test.bodies[2], "abcde", 5) == 0 && test.pSession->rxBuffer.nUsed == 0,
        "Retain every body byte and drain input");
    XTest_Close(&test);
    return 0;
}

static int XTest_http_pipeline_burst(void)
{
    xtest_http_t test = {0};
    CHECK(XTest_Open(&test) == 0, "Create a pipelined burst fixture");
    test.bCountOnly = XTRUE;

    /* Each pipelined request used to be handled by a recursive call and cut
       off the front of the buffer separately: one stack frame and one move of
       the whole remaining input per request. */
    enum
    {
        BURST_REQUESTS = 20000
    };
    const char request[] = "GET /burst HTTP/1.1\r\nHost: a\r\n\r\n";
    xbyte_buffer_t *pRx = &test.pSession->rxBuffer;
    for (int i = 0; i < BURST_REQUESTS; i++)
        CHECK(XByteBuffer_Add(pRx, (const uint8_t*)request, sizeof(request) - 1) > 0, "Queue one pipelined request");

    CHECK(XAPI_ProcessBuffered(test.pSession) == XAPI_CONTINUE, "A pipelined burst keeps the session");
    CHECK(test.nCounted == BURST_REQUESTS && !test.nInvalid, "Deliver every pipelined request in the burst");
    CHECK(pRx->nUsed == 0, "Consume the whole burst");
    XTest_Close(&test);
    return 0;
}

static int XTest_http_chunked(void)
{
    /* A chunked request sent a byte at a time is delivered once, at its last chunk, with its body decoded. Its
       chunks are scanned as they arrive, from where the last look stopped, so no byte is looked at twice. */
    xtest_http_t test = {0};
    CHECK(XTest_Open(&test) == 0, "Create a chunked request fixture");
    const char sHead[] = "POST /chunks HTTP/1.1\r\nTransfer-Encoding: chunked\r\nConnection: keep-alive\r\n\r\n";
    const char sBody[] = "3\r\na\0b\r\n2;x=y\r\ncd\r\n0\r\nT: 1\r\n\r\n";
    const size_t nHead = sizeof(sHead) - 1, nBody = sizeof(sBody) - 1;

    for (size_t i = 0; i < nHead + nBody; i++)
    {
        const char *pByte = i < nHead ? &sHead[i] : &sBody[i - nHead];
        CHECK(XSock_Write(&test.peer, pByte, 1) == 1, "Send one byte");
        CHECK(XAPI_Service(&test.api, 20) == XEVENTS_SUCCESS, "Service it");
        CHECK(test.nReads == (i == nHead + nBody - 1 ? 1 : 0), "Deliver only at the end of the last chunk");

        if (i + 1 >= nHead && i + 1 < nHead + nBody)
        {
            xapi_session_t *pSession = test.pSession;
            CHECK(pSession->nHeaderWait == SIZE_MAX && pSession->nHeaderScan == nHead, "The body is waited for");
            CHECK(pSession->headerChunks.nPosit == i + 1 - nHead, "Its chunks are scanned as far as they arrived");
        }
    }

    CHECK(!test.nInvalid && test.nClosed == 0 && strcmp(test.uris[0], "/chunks") == 0, "The request is delivered");
    CHECK(test.lengths[0] == 5 && memcmp(test.bodies[0], "a\0bcd", 5) == 0, "With its chunks decoded");
    CHECK(test.pSession->bKeepAlive && test.pSession->rxBuffer.nUsed == 0, "Exactly that request is consumed");
    XTest_Close(&test);
    return 0;
}

static int XTest_http_chunked_pipeline(void)
{
    /* A decoded body gives up the bytes of its framing in the session buffer, and the requests behind it are
       still found where they start */
    xtest_http_t test = {0};
    CHECK(XTest_Open(&test) == 0, "Create a pipelined chunked fixture");
    const char wire[] = "POST /one HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n1\r\na\r\n2\r\nbc\r\n0\r\n\r\n"
                        "GET /two HTTP/1.1\r\nContent-Length: 0\r\n\r\n"
                        "POST /three HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nde\r\n";
    CHECK(XSock_Write(&test.peer, wire, sizeof(wire) - 1) == sizeof(wire) - 1, "Send two requests and part of a third");
    CHECK(XAPI_Service(&test.api, 20) == XEVENTS_SUCCESS, "Service one coalesced read");
    CHECK(test.nReads == 2 && !test.nInvalid, "Both complete requests are delivered");
    CHECK(!strcmp(test.uris[0], "/one") && !strcmp(test.uris[1], "/two"), "In order");
    CHECK(test.lengths[0] == 3 && !memcmp(test.bodies[0], "abc", 3) && test.lengths[1] == 0, "Each with its own body");

    xapi_session_t *pSession = test.pSession;
    const size_t nThird = sizeof("POST /three HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n") - 1;
    CHECK(pSession->rxBuffer.nUsed == nThird + 7 && pSession->nHeaderScan == nThird, "The third waits at the front");
    CHECK(XSock_Write(&test.peer, "1\r\nf\r\n0\r\n\r\n", 11) == 11, "Finish it");
    CHECK(XAPI_Service(&test.api, 20) == XEVENTS_SUCCESS, "Service the rest");
    CHECK(test.nReads == 3 && !strcmp(test.uris[2], "/three"), "The third is delivered");
    CHECK(test.lengths[2] == 3 && !memcmp(test.bodies[2], "def", 3) && pSession->rxBuffer.nUsed == 0, "Decoded, and all");
    XTest_Close(&test);
    return 0;
}

static int XTest_http_chunked_invalid(void)
{
    /* Malformed chunks end the session, whether they arrive with the header or after it, and so does a request
       body whose length cannot be known, or is not a length at all */
    const char *pWires[] = {
        "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\nZZ\r\n",
        "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n",
        "POST / HTTP/1.1\r\nTransfer-Encoding: gzip\r\n\r\nabc",
        "POST / HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\nab",
        "POST /test HTTP/1.1\r\nHost: example.com\r\nContent-Length: banana\r\n\r\nhello"
    };

    for (size_t i = 0; i < sizeof(pWires) / sizeof(*pWires); i++)
    {
        xtest_http_t test = {0};
        CHECK(XTest_Open(&test) == 0, "Create a refused request fixture");
        CHECK(XSock_Write(&test.peer, pWires[i], strlen(pWires[i])) == (int)strlen(pWires[i]), "Send the request");
        CHECK(XAPI_Service(&test.api, 20) == XEVENTS_SUCCESS, "Service it");

        /* The one with a valid header so far is refused at its first malformed chunk */
        if (i == 1)
        {
            CHECK(test.nClosed == 0 && test.pSession->nHeaderScan, "A header without chunks yet waits for them");
            CHECK(XSock_Write(&test.peer, "1\r\nab", 5) == 5, "Send a chunk longer than it says");
            CHECK(XAPI_Service(&test.api, 20) == XEVENTS_SUCCESS, "Service it");
        }

        CHECK(test.nReads == 0 && test.nClosed == 1 && test.pSession == NULL, "The session ends, nothing delivered");
        XTest_Close(&test);
    }

    return 0;
}

static int XTest_Authorize(const char *pHeader, const char *pValue, xbool_t bBasic, xbool_t bAllowed)
{
    xtest_http_t test = {0};
    CHECK(XTest_Open(&test) == 0, "Create an authorization fixture");
    xhttp_t request;
    CHECK(XHTTP_InitRequest(&request, XHTTP_GET, "/", "1.1") > 0, "Build the authorization request");
    if (pValue) CHECK(XHTTP_AddHeader(&request, pHeader, "%s", pValue) > 0, "Attach the supplied credential");
    test.pSession->pPacket = &request;
    int nStatus = XAPI_AuthorizeHTTP(test.pSession, bBasic ? "dXNlcjpwYXNz" : NULL, bBasic ? NULL : "test-key");
    test.pSession->pPacket = NULL;
    xbool_t bAccepted = nStatus == XSTDOK && test.pSession->txBuffer.nUsed == 0;
    int nCode = 0;
    if (test.pSession->txBuffer.nUsed)
    {
        xhttp_t response;
        XHTTP_ParseData(&response, test.pSession->txBuffer.pData, test.pSession->txBuffer.nUsed);
        nCode = response.nStatusCode;
        XHTTP_Clear(&response);
    }
    XHTTP_Clear(&request);
    XTest_Close(&test);
    CHECK(bAccepted == bAllowed, "Only complete, correctly framed credentials authorize the request");
    CHECK(bAllowed || nCode == 401, "Rejected credentials produce an HTTP 401 response");
    return 0;
}

static int XTest_auth_key(void)
{
    CHECK(XTest_Authorize("X-API-KEY", "test-key", XFALSE, XTRUE) == 0, "The exact key authorizes");
    const char *values[] = {NULL, "other-key", "test-ke", "test-key-extra", "test-key "};
    for (size_t i = 0; i < sizeof(values) / sizeof(*values); i++)
        CHECK(XTest_Authorize("X-API-KEY", values[i], XFALSE, XFALSE) == 0, "Missing, changed and suffixed keys are rejected");
    return 0;
}

static int XTest_auth_basic(void)
{
    CHECK(XTest_Authorize("Authorization", "Basic dXNlcjpwYXNz", XTRUE, XTRUE) == 0, "The exact Basic token authorizes");
    const char *values[] = {NULL, "Basic other", "Basic dXNlcjpwYXNz-extra", "xBasic dXNlcjpwYXNz", "BasicXdXNlcjpwYXNz"};
    for (size_t i = 0; i < sizeof(values) / sizeof(*values); i++)
        CHECK(XTest_Authorize("Authorization", values[i], XTRUE, XFALSE) == 0, "Malformed and suffixed Basic credentials fail");
    CHECK(XTest_Authorize("Authorization", "basic dXNlcjpwYXNz", XTRUE, XTRUE) == 0, "The scheme ignores letter case");
    CHECK(XTest_Authorize("Authorization", "Basic   dXNlcjpwYXNz", XTRUE, XTRUE) == 0,
        "Scheme spacing preserves the complete token");
    return 0;
}

static int XTest_auth_short(void)
{
    const char *values[] = {"Basic", "Basic ", "B", "Basic x"};
    for (size_t i = 0; i < sizeof(values) / sizeof(*values); i++)
        CHECK(XTest_Authorize("Authorization", values[i], XTRUE, XFALSE) == 0, "Short Basic headers reject without overreading");
    return 0;
}

XTEST_MAIN(
    XTEST_CASE(http_partial),
    XTEST_CASE(http_pipeline),
    XTEST_CASE(http_pipeline_burst),
    XTEST_CASE(http_chunked),
    XTEST_CASE(http_chunked_pipeline),
    XTEST_CASE(http_chunked_invalid),
    XTEST_CASE(auth_key),
    XTEST_CASE(auth_basic),
    XTEST_CASE(auth_short)
)
