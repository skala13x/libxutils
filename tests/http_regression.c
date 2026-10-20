/* libxutils: HTTP fragmentation, message boundaries, credential capacity and reuse. */
#include "test.h"
#include "http.h"
#include "base64.h"
#include "sock.h"

static int XTest_partial(void)
{
    const char wire[] = "POST /upload HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\n\r\na\0b\r\n";
    const size_t nSize = sizeof(wire) - 1;
    for (size_t nSplit = 0; nSplit < nSize; nSplit++)
    {
        xhttp_t http;
        CHECK(XHTTP_InitParser(&http, (uint8_t*)wire, nSplit) > 0, "Initialize a bounded request prefix");
        xhttp_status_t eStatus = XHTTP_Parse(&http);
        CHECK(eStatus == XHTTP_INCOMPLETE || eStatus == XHTTP_PARSED, "Proper request prefixes must not complete");
        CHECK(XHTTP_AppendData(&http, (uint8_t*)wire + nSplit, nSize - nSplit) > 0, "Append the rest of the request");
        CHECK(XHTTP_Parse(&http) == XHTTP_COMPLETE, "Complete request must parse at every split boundary");
        CHECK(http.eMethod == XHTTP_POST && strcmp(http.sUri, "/upload") == 0, "Fragmentation preserves the start line");
        CHECK(
            XHTTP_GetBodySize(&http) == 5 && memcmp(XHTTP_GetBody(&http), "a\0b\r\n", 5) == 0, "Body bytes must be binary-safe");
        XHTTP_Clear(&http);
    }
    return 0;
}

static int XTest_pipeline(void)
{
    const char wire[] = "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\noneHTTP/1.1 204 No Content\r\n\r\n";
    xhttp_t first, second;
    CHECK(XHTTP_ParseData(&first, (uint8_t*)wire, sizeof(wire) - 1) == XHTTP_COMPLETE, "Parse first pipelined response");
    CHECK(first.nContentLength == 3 && memcmp(XHTTP_GetBody(&first), "one", 3) == 0, "Keep the first response body bounded");
    size_t nExtra = XHTTP_GetExtraSize(&first);
    CHECK(nExtra > 0 && XHTTP_GetPacketSize(&first) + nExtra == sizeof(wire) - 1, "Preserve the next response as extra data");
    CHECK(XHTTP_ParseData(&second, (uint8_t*)XHTTP_GetExtraData(&first), nExtra) == XHTTP_COMPLETE, "Parse the next response");
    CHECK(second.nStatusCode == 204 && XHTTP_GetExtraSize(&second) == 0, "Pipelining must preserve status and boundaries");
    XHTTP_Clear(&second);
    XHTTP_Clear(&first);
    return 0;
}

static int XTest_headers(void)
{
    xhttp_t http;
    CHECK(XHTTP_InitRequest(&http, XHTTP_GET, "/", "1.1") > 0, "Initialize request builder");
    CHECK(XHTTP_AddHeader(&http, "X-Test", "one") > 0, "Insert a named header");
    CHECK(strcmp(XHTTP_GetHeader(&http, "x-TEST"), "one") == 0, "Header lookups are case-insensitive");
    http.nAllowUpdate = XTRUE;
    CHECK(XHTTP_AddHeader(&http, "x-test", "two") > 0, "Update header under alternate casing");
    CHECK(
        http.headerMap.nCount == 1 && strcmp(XHTTP_GetHeader(&http, "X-Test"), "two") == 0, "Update must not duplicate a header");
    char value[4097];
    memset(value, 'x', sizeof(value) - 1);
    value[sizeof(value) - 1] = '\0';
    CHECK(XHTTP_AddHeader(&http, "Authorization", "Bearer %s", value) > 0, "Add credentials larger than the stack format buffer");
    CHECK(strlen(XHTTP_GetHeader(&http, "authorization")) == 7 + strlen(value), "Credentials must never be silently truncated");
    CHECK(XHTTP_AddHeader(&http, "X-Bad\r\nInjected", "x") < 0, "Header names must reject line injection");
    CHECK(XHTTP_AddHeader(&http, "X-Bad", "x\r\nInjected: yes") < 0, "Header values must reject line injection");
    xbyte_buffer_t *pWire = XHTTP_Assemble(&http, NULL, 0);
    CHECK(pWire && strstr((char*)pWire->pData, "\r\nAuthorization: Bearer "), "Assembly preserves the supplied header spelling");
    CHECK(strstr((char*)pWire->pData, "\r\nX-Test: two\r\n"), "Case-insensitive update preserves the first header spelling");
    XHTTP_Clear(&http);
    return 0;
}

static int XTest_basic_auth(void)
{
    char token[64];
    CHECK(XHTTP_GetAuthToken(token, sizeof(token), "Aladdin", "open sesame") == 28,
        "Basic auth must preserve the complete base64 value");
    CHECK(strcmp(token, "QWxhZGRpbjpvcGVuIHNlc2FtZQ==") == 0, "Basic auth must match the published example");
    struct
    {
        char sToken[4];
        char sGuard[8];
    } bounded;
    memset(&bounded, 0x5a, sizeof(bounded));
    CHECK(XHTTP_GetAuthToken(bounded.sToken, sizeof(bounded.sToken), "Aladdin", "open sesame") == 0 && !bounded.sToken[0],
        "Insufficient auth capacity fails without emitting truncated credentials");
    CHECK(memcmp(bounded.sGuard, "ZZZZZZZZ", 8) == 0, "The auth helper must honor caller capacity");
    return 0;
}

static int XTest_long_basic_auth(void)
{
    char password[2049], token[4096];
    memset(password, 'x', sizeof(password) - 1);
    password[sizeof(password) - 1] = 0;
    size_t nLength = XHTTP_GetAuthToken(token, sizeof(token), "user", password);
    CHECK(nLength > sizeof(password), "Encode credentials beyond the old fixed stack limit");
    char *pDecoded = XBase64_Decrypt((const uint8_t*)token, &nLength);
    CHECK(pDecoded && nLength == 2053 && memcmp(pDecoded, "user:", 5) == 0 && strcmp(pDecoded + 5, password) == 0,
        "Long credentials retain every byte");
    free(pDecoded);
    xhttp_t http;
    CHECK(XHTTP_InitRequest(&http, XHTTP_GET, "/", "1.1") > 0, "Initialize auth request");
    CHECK(XHTTP_SetAuthBasic(&http, "user", password) > 0, "Build long Basic auth header");
    const char *pHeader = XHTTP_GetHeader(&http, "Authorization");
    CHECK(pHeader && strncmp(pHeader, "Basic ", 6) == 0 && strcmp(pHeader + 6, token) == 0,
        "Header builder uses complete credentials");
    XHTTP_Clear(&http);
    return 0;
}

static int XTest_reuse(void)
{
    for (int nHard = 0; nHard < 2; nHard++)
    {
        xhttp_t http;
        CHECK(XHTTP_InitRequest(&http, XHTTP_POST, "/first", "1.1") > 0, "Create reusable request");
        CHECK(XHTTP_AddHeader(&http, "X-Old", "old") > 0 && XHTTP_Assemble(&http, (uint8_t*)"abc", 3) != NULL,
            "Assemble the first message");
        XHTTP_Reset(&http, nHard);
        CHECK(XHTTP_GetHeader(&http, "X-Old") == NULL && http.nContentLength == 0 && http.rawData.nUsed == 0,
            "Reset clears headers and body metadata for both allocation modes");
        http.eType = XHTTP_RESPONSE;
        http.nStatusCode = 200;
        CHECK(XHTTP_AddHeader(&http, "X-New", "new") > 0 && XHTTP_Assemble(&http, (uint8_t*)"z", 1) != NULL,
            "Reuse must assemble a fresh message");
        CHECK(XHTTP_GetHeader(&http, "X-Old") == NULL && http.nContentLength == 1,
            "Old state must not contaminate reused messages");
        XHTTP_Clear(&http);
    }
    return 0;
}

static int XTest_empty_body(void)
{
    const char wire[] = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nContent-Type: application/json\r\n\r\n";
    xhttp_t http;
    CHECK(XHTTP_ParseData(&http, (uint8_t*)wire, sizeof(wire) - 1) == XHTTP_COMPLETE,
        "Explicit zero content length completes regardless of content type");
    CHECK(http.nContentLength == 0 && XHTTP_GetBodySize(&http) == 0, "Empty body must not invent data");
    XHTTP_Clear(&http);
    return 0;
}

static int XTest_assemble_empty_body(void)
{
    for (int nResponse = 0; nResponse < 2; nResponse++)
    {
        xhttp_t http;
        if (nResponse) XHTTP_InitResponse(&http, 200, "1.1");
        else XHTTP_InitRequest(&http, XHTTP_POST, "/empty", "1.1");
        CHECK(XHTTP_AddHeader(&http, "Content-Type", "application/octet-stream") > 0, "Set the body type");
        CHECK(XHTTP_AddHeader(&http, "Content-Length", "9") > 0, "An explicit body replaces a stale length");
        CHECK(XHTTP_AddHeader(&http, "Connection", "keep-alive") > 0, "Use a persistent connection");
        xbyte_buffer_t *pWire = XHTTP_Assemble(&http, (const uint8_t*)"", 0);
        CHECK(pWire != NULL, "Assemble an explicitly empty body");
        const char *pLength = XHTTP_GetHeader(&http, "Content-Length");
        CHECK(pLength != NULL && !strcmp(pLength, "0"), "An explicit empty body is length delimited");
        xhttp_t parsed;
        CHECK(XHTTP_ParseData(&parsed, pWire->pData, pWire->nUsed) == XHTTP_COMPLETE, "The empty message completes");
        CHECK(parsed.nContentLength == 0 && XHTTP_GetBodySize(&parsed) == 0, "No body bytes are invented");
        XHTTP_Clear(&parsed);
        XHTTP_Clear(&http);
    }

    xhttp_t stream;
    XHTTP_InitResponse(&stream, 200, "1.1");
    CHECK(XHTTP_AddHeader(&stream, "Content-Type", "application/octet-stream") > 0, "Prepare a streaming response");
    CHECK(XHTTP_Assemble(&stream, NULL, 0) != NULL, "Assemble headers without providing a body");
    CHECK(XHTTP_GetHeader(&stream, "Content-Length") == NULL, "An unspecified streaming body retains its framing");
    XHTTP_Clear(&stream);
    return 0;
}


static int XTest_code_strings(void)
{
    /* Every status code the library names has to render, and an unknown
     * one has to fall through rather than reading past its table. */
    const struct { int nCode; const char *pText; } known[] = {
        {100, "Continue"}, {200, "OK"}, {201, "Created"}, {204, "No Content"},
        {301, "Moved Permanently"}, {302, "Found"}, {400, "Bad Request"},
        {401, "Unauthorized"}, {403, "Forbidden"}, {404, "Not Found"},
        {405, "Method Not Allowed"}, {408, "Request Timeout"},
        {500, "Internal Server Error"}, {502, "Bad Gateway"}, {503, "Service Unavailable"}
    };

    for (size_t i = 0; i < sizeof(known) / sizeof(*known); i++)
        CHECK(strcmp(XHTTP_GetCodeStr(known[i].nCode), known[i].pText) == 0,
            "Every known status code renders its own reason phrase");

    CHECK(strncmp(XHTTP_GetCodeStr(999), "Unknown", 7) == 0, "An unknown code falls through");
    CHECK(strncmp(XHTTP_GetCodeStr(0), "Unknown", 7) == 0, "A zero code falls through");
    CHECK(strncmp(XHTTP_GetCodeStr(-1), "Unknown", 7) == 0, "A negative code falls through");

    /* Methods round trip between their name and their enum. */
    const struct { xhttp_method_t eMethod; const char *pName; } methods[] = {
        {XHTTP_PUT, "PUT"}, {XHTTP_GET, "GET"}, {XHTTP_POST, "POST"},
        {XHTTP_DELETE, "DELETE"}, {XHTTP_OPTIONS, "OPTIONS"}
    };

    for (size_t i = 0; i < sizeof(methods) / sizeof(*methods); i++)
    {
        CHECK(strcmp(XHTTP_GetMethodStr(methods[i].eMethod), methods[i].pName) == 0,
            "Every method renders its own name");
        CHECK(XHTTP_GetMethodType(methods[i].pName) == methods[i].eMethod,
            "Every method name parses back to its own value");
    }

    CHECK(strcmp(XHTTP_GetMethodStr(XHTTP_DUMMY), "DUMMY") == 0, "The placeholder method is named");
    CHECK(strcmp(XHTTP_GetMethodStr((xhttp_method_t)99), "UNKNOWN") == 0, "An out of range method is named");
    CHECK(XHTTP_GetMethodType("HEAD") == XHTTP_DUMMY, "An unsupported method is not invented");
    CHECK(XHTTP_GetMethodType("nonsense") == XHTTP_DUMMY, "An unknown method name is rejected");
    CHECK(XHTTP_GetMethodType("") == XHTTP_DUMMY, "An empty method name is rejected");

    /* Every status value has a description. */
    const xhttp_status_t states[] = {
        XHTTP_NONE, XHTTP_INVALID, XHTTP_EINIT, XHTTP_ELINK, XHTTP_EAUTH, XHTTP_EREAD,
        XHTTP_EWRITE, XHTTP_EPROTO, XHTTP_ETIMEO, XHTTP_EALLOC, XHTTP_ESETHDR,
        XHTTP_EFDMODE, XHTTP_EEXISTS, XHTTP_ECONNECT, XHTTP_ERESOLVE, XHTTP_EASSEMBLE,
        XHTTP_TERMINATED, XHTTP_INCOMPLETE, XHTTP_CONNECTED, XHTTP_RESOLVED,
        XHTTP_COMPLETE, XHTTP_BIGCNT, XHTTP_BIGHDR, XHTTP_PARSED
    };

    for (size_t i = 0; i < sizeof(states) / sizeof(*states); i++)
    {
        const char *pText = XHTTP_GetStatusStr(states[i]);
        CHECK(pText != NULL && *pText != '\0', "Every status has a description");
    }
    return 0;
}

static int XTest_content_length(void)
{
    /* A content length is a plain run of digits, or a list of the same one
     * (RFC 9112, 6.3). Anything else is not a length, and the message has no
     * framing at all: it is invalid. Read as 0, as it was, the body went for
     * the next message, which is how a proxy and an origin end up disagreeing
     * on the message boundary. A negative value would have wrapped into a
     * size the peer can never satisfy. */
    const char *pRejected[] = {
        "POST /test HTTP/1.1\r\nHost: example.com\r\nContent-Length: banana\r\n\r\nhello",
        "GET / HTTP/1.1\r\nContent-Length: -5\r\n\r\n",
        "GET / HTTP/1.1\r\nContent-Length: 5x\r\n\r\nabcde",
        "GET / HTTP/1.1\r\nContent-Length: abc\r\n\r\n",
        "GET / HTTP/1.1\r\nContent-Length: +5\r\n\r\nabcde",
        "GET / HTTP/1.1\r\nContent-Length: 0x10\r\n\r\n",
        "GET / HTTP/1.1\r\nContent-Length: \r\n\r\n",
        "GET / HTTP/1.1\r\nContent-Length:\r\n\r\nabcde",
        "GET / HTTP/1.1\r\ncontent-length: \t \r\n\r\n",
        "GET / HTTP/1.1\r\nContent-Length: 5 5\r\n\r\nabcde",
        "GET / HTTP/1.1\r\nContent-Length: 5, 6\r\n\r\nabcdef",
        "GET / HTTP/1.1\r\nContent-Length: 5, x\r\n\r\nabcde",
        "GET / HTTP/1.1\r\nContent-Length: ,\r\n\r\n",
        "GET / HTTP/1.1\r\nContent-Length: ,5\r\n\r\nabcde",
        "GET / HTTP/1.1\r\nContent-Length: , 5\r\n\r\nabcde",
        "GET / HTTP/1.1\r\nContent-Length: 5,\r\n\r\nabcde",
        "GET / HTTP/1.1\r\nContent-Length: 5 ,\r\n\r\nabcde",
        "GET / HTTP/1.1\r\nContent-Length: 5,,5\r\n\r\nabcde",
        "GET / HTTP/1.1\r\nContent-Length: 5, ,5\r\n\r\nabcde",
        "GET / HTTP/1.1\r\nContent-Length: 5,\t,5\r\n\r\nabcde",
        "GET / HTTP/1.1\r\nContent-Length: ,,\r\n\r\n",
        "GET / HTTP/1.1\r\nContent-Length: 5\r\nContent-Length: 5,\r\n\r\nabcde",
        "GET / HTTP/1.1\r\nContent-Length: ,5\r\nContent-Length: 5\r\n\r\nabcde",
        "GET / HTTP/1.1\r\nContent-Length: 5\r\nContent-Length: banana\r\n\r\nabcde",
        "GET / HTTP/1.1\r\nContent-Length: 5\r\nContent-Length:\r\n\r\nabcde",
        "GET / HTTP/1.1\r\nContent-Length: 18446744073709551616\r\n\r\n",
        "GET / HTTP/1.1\r\nContent-Length: 99999999999999999999999\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: banana\r\n\r\nhello",
        "HTTP/1.1 304 Not Modified\r\nContent-Length: banana\r\n\r\n",
        "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\nContent-Length: banana\r\n\r\n0\r\n\r\n"
    };

    for (size_t i = 0; i < sizeof(pRejected) / sizeof(*pRejected); i++)
    {
        xhttp_t http;
        xhttp_status_t eStatus = XHTTP_ParseData(&http, (uint8_t*)pRejected[i], strlen(pRejected[i]));
        if (eStatus != XHTTP_INVALID) fprintf(stderr, "Case %zu: status %d\n", i, (int)eStatus);
        CHECK(eStatus == XHTTP_INVALID, "A message whose length is not one is invalid, whatever else frames it");
        XHTTP_Clear(&http);
    }

    /* A well formed length is taken at face value, whatever the spacing, and
     * so is a list of the same one, on one line or over several. */
    const char *pAccepted[] = {
        "GET / HTTP/1.1\r\nContent-Length: 5\r\n\r\nabcde",
        "GET / HTTP/1.1\r\nContent-Length:5\r\n\r\nabcde",
        "GET / HTTP/1.1\r\nContent-Length:   5\r\n\r\nabcde",
        "GET / HTTP/1.1\r\nCONTENT-LENGTH:\t5 \t\r\n\r\nabcde",
        "GET / HTTP/1.1\r\nContent-Length: 5, 5\r\n\r\nabcde",
        "GET / HTTP/1.1\r\nContent-Length: 5 ,\t5\r\n\r\nabcde",
        "GET / HTTP/1.1\r\nContent-Length: 5,5,5\r\n\r\nabcde",
        "GET / HTTP/1.1\r\nContent-Length: 5\r\nContent-Length: 5, 5\r\n\r\nabcde",
        "GET / HTTP/1.1\r\nContent-Length: 05\r\ncontent-length: 5\r\n\r\nabcde"
    };

    for (size_t i = 0; i < sizeof(pAccepted) / sizeof(*pAccepted); i++)
    {
        xhttp_t http;
        CHECK(XHTTP_ParseData(&http, (uint8_t*)pAccepted[i], strlen(pAccepted[i])) == XHTTP_COMPLETE,
            "A well formed length parses");
        CHECK(http.nContentLength == 5, "The announced length is taken whatever the spacing");
        CHECK(XHTTP_GetBodySize(&http) == 5, "The body is the announced length");
        CHECK(memcmp(XHTTP_GetBody(&http), "abcde", 5) == 0, "The body is the announced bytes");
        XHTTP_Clear(&http);
    }

    /* A zero length announces an empty body, not a missing one. */
    const char empty[] = "POST / HTTP/1.1\r\nContent-Length: 0\r\n\r\n";
    xhttp_t http;
    CHECK(XHTTP_ParseData(&http, (uint8_t*)empty, sizeof(empty) - 1) == XHTTP_COMPLETE,
        "A zero length request completes");
    CHECK(http.nContentLength == 0 && XHTTP_GetBodySize(&http) == 0, "A zero length body is empty");
    XHTTP_Clear(&http);
    return 0;
}

static int XTest_start_line(void)
{
    /* A request start line yields the method, the target and the version. */
    const char request[] = "DELETE /a/b/c?x=1&y=2 HTTP/1.1\r\nHost: h\r\n\r\n";
    xhttp_t http;
    CHECK(XHTTP_ParseData(&http, (uint8_t*)request, sizeof(request) - 1) == XHTTP_COMPLETE,
        "A request start line parses");
    CHECK(http.eType == XHTTP_REQUEST, "The message is classified as a request");
    CHECK(http.eMethod == XHTTP_DELETE, "The method is taken from the start line");
    CHECK(strcmp(http.sUri, "/a/b/c?x=1&y=2") == 0, "The whole target including the query is kept");
    CHECK(strcmp(http.sVersion, "1.1") == 0, "The version is taken from the start line");
    CHECK(http.nStatusCode == 0, "A request carries no status code");
    XHTTP_Clear(&http);

    /* A response start line yields the code and the version. */
    const char response[] = "HTTP/1.0 503 Service Unavailable\r\nContent-Length: 0\r\n\r\n";
    CHECK(XHTTP_ParseData(&http, (uint8_t*)response, sizeof(response) - 1) == XHTTP_COMPLETE,
        "A response start line parses");
    CHECK(http.eType == XHTTP_RESPONSE, "The message is classified as a response");
    CHECK(http.nStatusCode == 503, "The status code is taken from the start line");
    CHECK(strcmp(http.sVersion, "1.0") == 0, "The response version is taken from the start line");
    CHECK(XHTTP_IsSuccessCode(&http) == XFALSE, "A server error is not a success code");
    XHTTP_Clear(&http);

    /* Success is the 2xx range and nothing else. */
    const struct { int nCode; xbool_t bSuccess; } codes[] = {
        {200, XTRUE}, {201, XTRUE}, {204, XTRUE}, {299, XTRUE},
        {100, XFALSE}, {199, XFALSE}, {300, XFALSE}, {404, XFALSE}, {500, XFALSE}
    };

    for (size_t i = 0; i < sizeof(codes) / sizeof(*codes); i++)
    {
        char sWire[128];
        snprintf(sWire, sizeof(sWire), "HTTP/1.1 %d X\r\nContent-Length: 0\r\n\r\n", codes[i].nCode);
        CHECK(XHTTP_ParseData(&http, (uint8_t*)sWire, strlen(sWire)) == XHTTP_COMPLETE, "Every response parses");
        CHECK(XHTTP_IsSuccessCode(&http) == codes[i].bSuccess, "Only the 2xx range counts as success");
        XHTTP_Clear(&http);
    }

    /* Anything that is not a start line is rejected outright. A response
     * line with no status code is the important one: the version cut would
     * otherwise run on into the headers, and XHTTP_Assemble() writes the
     * version back out verbatim, so a version holding a CRLF would split
     * the assembled message in two. */
    const char *pInvalid[] = {
        "not http at all\r\n\r\n",
        "\r\n\r\n",
        "GET\r\n\r\n",
        "HTTP/1.1\r\n\r\n",
        "HTTP/1.1\r\nContent-Length: 0\r\n\r\n",
        "HTTP/1.1\r\nX: y\r\n\r\n",
        "HTTP/1.1 \r\n\r\n",
        "HTTP/1.1 99 Too Small\r\n\r\n",
        "HTTP/1.1 600 Too Big\r\n\r\n",
        "HTTP/1.1 abc Not A Code\r\n\r\n"
    };

    for (size_t i = 0; i < sizeof(pInvalid) / sizeof(*pInvalid); i++)
    {
        xhttp_status_t eStatus = XHTTP_ParseData(&http, (uint8_t*)pInvalid[i], strlen(pInvalid[i]));
        CHECK(eStatus != XHTTP_COMPLETE, "A malformed start line does not complete");
        CHECK(strchr(http.sVersion, '\r') == NULL && strchr(http.sVersion, '\n') == NULL,
            "A rejected start line never leaves a line break in the version");
        XHTTP_Clear(&http);
    }

    /* Every code in the valid range is accepted, and the version stays clean. */
    const int accepted[] = {100, 200, 301, 404, 500, 599};
    for (size_t i = 0; i < sizeof(accepted) / sizeof(*accepted); i++)
    {
        char sWire[96];
        snprintf(sWire, sizeof(sWire), "HTTP/1.1 %d R\r\nContent-Length: 0\r\n\r\n", accepted[i]);
        CHECK(XHTTP_ParseData(&http, (uint8_t*)sWire, strlen(sWire)) == XHTTP_COMPLETE,
            "Every code in range completes");
        CHECK(http.nStatusCode == (uint16_t)accepted[i], "The code is taken from the start line");
        CHECK(strcmp(http.sVersion, "1.1") == 0, "The version is just the version");
        XHTTP_Clear(&http);
    }
    return 0;
}

static int XTest_assemble(void)
{
    /* A request built by the library has to parse back as what was built. */
    xhttp_t http;
    CHECK(XHTTP_InitRequest(&http, XHTTP_POST, "/submit", "1.1") > 0, "A request initializes");
    CHECK(XHTTP_AddHeader(&http, "Host", "example.com") > 0, "A header is added");
    CHECK(XHTTP_AddHeader(&http, "X-Count", "%d", 42) > 0, "A formatted header is added");
    CHECK(XHTTP_AddHeader(&http, "Content-Type", "text/plain") > 0, "A content type is added");

    const uint8_t body[] = {'p', 'a', 'y', 0x00, 0xff, 'd'};
    xbyte_buffer_t *pWire = XHTTP_Assemble(&http, body, sizeof(body));
    CHECK(pWire != NULL && pWire->nUsed > 0, "The request assembles");

    xhttp_t parsed;
    CHECK(XHTTP_ParseData(&parsed, pWire->pData, pWire->nUsed) == XHTTP_COMPLETE,
        "The assembled request parses back");
    CHECK(parsed.eMethod == XHTTP_POST, "The method survives the round trip");
    CHECK(strcmp(parsed.sUri, "/submit") == 0, "The target survives the round trip");
    CHECK(strcmp(parsed.sVersion, "1.1") == 0, "The version survives the round trip");
    CHECK(strcmp(XHTTP_GetHeader(&parsed, "Host"), "example.com") == 0, "A header survives the round trip");
    CHECK(strcmp(XHTTP_GetHeader(&parsed, "X-Count"), "42") == 0, "A formatted header survives the round trip");
    CHECK(XHTTP_GetBodySize(&parsed) == sizeof(body), "The body length survives the round trip");
    CHECK(memcmp(XHTTP_GetBody(&parsed), body, sizeof(body)) == 0, "Binary body bytes survive the round trip");

    /* The assembler announces the length it was handed. */
    CHECK(strcmp(XHTTP_GetHeader(&parsed, "Content-Length"), "6") == 0, "The assembler announces the body length");
    XHTTP_Clear(&parsed);
    XHTTP_Clear(&http);

    /* A response assembles the same way. */
    CHECK(XHTTP_InitResponse(&http, 201, "1.1") > 0, "A response initializes");
    CHECK(XHTTP_AddHeader(&http, "Location", "/created/1") > 0, "A location header is added");

    pWire = XHTTP_Assemble(&http, NULL, 0);
    CHECK(pWire != NULL, "A body-less response assembles");

    /* No body handed over leaves the body unspecified, and a response without a length has one that ends with
       the connection (RFC 9112, 6.3): its header parses back, and the parser waits for the end of the stream */
    CHECK(XHTTP_ParseData(&parsed, pWire->pData, pWire->nUsed) == XHTTP_PARSED,
        "The assembled response parses back up to its body");
    CHECK(parsed.eFraming == XHTTP_FRAME_CLOSE && http.eFraming == XHTTP_FRAME_CLOSE, "Both ends see that body framing");
    CHECK(parsed.eType == XHTTP_RESPONSE, "The round trip is still a response");
    CHECK(parsed.nStatusCode == 201, "The status code survives the round trip");
    CHECK(strcmp(XHTTP_GetHeader(&parsed, "Location"), "/created/1") == 0, "The location survives the round trip");
    CHECK(XHTTP_GetBodySize(&parsed) == 0, "A body-less response has no body");
    XHTTP_Clear(&parsed);
    XHTTP_Clear(&http);

    /* An empty body handed over is a length of zero, which completes where the header does */
    CHECK(XHTTP_InitResponse(&http, 201, "1.1") > 0, "A response initializes");
    pWire = XHTTP_Assemble(&http, (const uint8_t*)"", 0);
    CHECK(pWire != NULL && http.eFraming == XHTTP_FRAME_LENGTH, "An empty response assembles with a length");
    CHECK(XHTTP_ParseData(&parsed, pWire->pData, pWire->nUsed) == XHTTP_COMPLETE, "It parses back whole");
    CHECK(parsed.eFraming == XHTTP_FRAME_LENGTH && XHTTP_GetBodySize(&parsed) == 0, "With no body");
    XHTTP_Clear(&parsed);
    XHTTP_Clear(&http);

    /* Every method assembles into a start line the parser recognises. */
    const xhttp_method_t methods[] = {XHTTP_GET, XHTTP_PUT, XHTTP_POST, XHTTP_DELETE, XHTTP_OPTIONS};
    for (size_t i = 0; i < sizeof(methods) / sizeof(*methods); i++)
    {
        CHECK(XHTTP_InitRequest(&http, methods[i], "/x", "1.1") > 0, "Every method initializes");
        pWire = XHTTP_Assemble(&http, NULL, 0);
        CHECK(pWire != NULL, "Every method assembles");
        CHECK(XHTTP_ParseData(&parsed, pWire->pData, pWire->nUsed) == XHTTP_COMPLETE, "Every method parses back");
        CHECK(parsed.eMethod == methods[i], "Every method survives the round trip");
        XHTTP_Clear(&parsed);
        XHTTP_Clear(&http);
    }
    return 0;
}

/* The same message, built into a handle: a request, a response, and a keep-alive response with a code that
   has no reason phrase of its own */
static int http_init_message(xhttp_t *pHttp, size_t nKind)
{
    if (nKind == 0)
    {
        if (XHTTP_InitRequest(pHttp, XHTTP_POST, "/submit", "1.1") <= 0) return XSTDERR;
        return XHTTP_AddHeader(pHttp, "Host", "example.com") > 0 &&
            XHTTP_AddHeader(pHttp, "X-Count", "%d", 42) > 0 ? XSTDOK : XSTDERR;
    }

    if (XHTTP_InitResponse(pHttp, nKind == 1 ? 201 : 599, "1.1") <= 0) return XSTDERR;
    pHttp->nKeepAlive = nKind == 2;
    return XHTTP_AddHeader(pHttp, "Server", "test") > 0 ? XSTDOK : XSTDERR;
}

static int XTest_assemble_to(void)
{
    /* What is appended is byte for byte what XHTTP_Assemble() builds, behind what the buffer held */
    const uint8_t body[] = {'p', 'a', 'y', 0x00, 0xff, 'd'};
    const struct { const uint8_t *pBody; size_t nLength; } bodies[] = {
        {body, sizeof(body)}, {NULL, 0}, {(const uint8_t*)"", 0}, {NULL, 5}
    };

    for (size_t k = 0; k < 3; k++)
    {
        for (size_t b = 0; b < sizeof(bodies) / sizeof(*bodies); b++)
        {
            xhttp_t assembled, appended;
            CHECK(http_init_message(&assembled, k) == XSTDOK && http_init_message(&appended, k) == XSTDOK,
                "Two handles hold the same message");

            xbyte_buffer_t *pWire = XHTTP_Assemble(&assembled, bodies[b].pBody, bodies[b].nLength);
            CHECK(pWire != NULL && pWire->nUsed > 0, "The message assembles");

            xbyte_buffer_t buffer;
            XByteBuffer_Init(&buffer, 0, XFALSE);
            CHECK(XByteBuffer_Add(&buffer, (const uint8_t*)"head", 4) > 0, "Seed the buffer");

            CHECK(XHTTP_WriteTo(&appended, &buffer, bodies[b].pBody, bodies[b].nLength) == XHTTP_NONE,
                "The message is appended");
            CHECK(buffer.nUsed == 4 + pWire->nUsed && !memcmp(buffer.pData, "head", 4), "Behind what was there");
            CHECK(!memcmp(buffer.pData + 4, pWire->pData, pWire->nUsed) && buffer.pData[buffer.nUsed] == '\0',
                "Byte for byte the assembled message, terminated");

            /* The handle describes the message, the length it was given included, and holds none of it */
            const char *pLength = XHTTP_GetHeader(&appended, "Content-Length");
            CHECK(bodies[b].pBody != NULL ? pLength != NULL && (size_t)atoi(pLength) == bodies[b].nLength : !pLength,
                "A body handed over is announced, and only one");
            CHECK(!appended.rawData.nUsed && !appended.nHeaderLength && !appended.nComplete, "The handle holds nothing");
            CHECK(XHTTP_GetBody(&appended) == NULL && XHTTP_GetHeaderRaw(&appended) == NULL, "And hands nothing out");

            XByteBuffer_Clear(&buffer);
            XHTTP_Clear(&assembled);
            XHTTP_Clear(&appended);
        }
    }

    /* The same handle appends one message after another, and they parse back in order */
    xhttp_t http, parsed;
    xbyte_buffer_t buffer;
    XByteBuffer_Init(&buffer, 0, XFALSE);
    CHECK(http_init_message(&http, 0) == XSTDOK, "A request is built");
    CHECK(XHTTP_WriteTo(&http, &buffer, (const uint8_t*)"first", 5) == XHTTP_NONE &&
        XHTTP_WriteTo(&http, &buffer, (const uint8_t*)"second!", 7) == XHTTP_NONE, "Two messages are appended");

    CHECK(XHTTP_ParseData(&parsed, buffer.pData, buffer.nUsed) == XHTTP_COMPLETE &&
        XHTTP_GetBodySize(&parsed) >= 5 && !memcmp(XHTTP_GetBody(&parsed), "first", 5), "The first parses");
    size_t nFirst = XHTTP_GetPacketSize(&parsed);
    XHTTP_Clear(&parsed);

    CHECK(nFirst < buffer.nUsed && XHTTP_ParseData(&parsed, buffer.pData + nFirst, buffer.nUsed - nFirst) == XHTTP_COMPLETE,
        "The second parses after it");
    CHECK(XHTTP_GetBodySize(&parsed) == 7 && !memcmp(XHTTP_GetBody(&parsed), "second!", 7) &&
        XHTTP_GetPacketSize(&parsed) == buffer.nUsed - nFirst, "With its own body, up to the end");
    XHTTP_Clear(&parsed);

    /* An assembled handle that was appended with another body is assembled again, not handed back as it was */
    CHECK(XHTTP_Assemble(&http, (const uint8_t*)"abc", 3) != NULL, "The handle assembles");
    CHECK(XHTTP_WriteTo(&http, &buffer, (const uint8_t*)"wxyz", 4) == XHTTP_NONE, "And appends another body");
    xbyte_buffer_t *pWire = XHTTP_Assemble(&http, (const uint8_t*)"wxyz", 4);
    CHECK(pWire != NULL && XHTTP_ParseData(&parsed, pWire->pData, pWire->nUsed) == XHTTP_COMPLETE &&
        XHTTP_GetBodySize(&parsed) == 4 && !memcmp(XHTTP_GetBody(&parsed), "wxyz", 4), "The new body is assembled");
    XHTTP_Clear(&parsed);
    XByteBuffer_Clear(&buffer);

    /* Refusals leave the buffer as it was */
    CHECK(XByteBuffer_Add(&buffer, (const uint8_t*)"keep", 4) > 0, "Seed the buffer");
    CHECK(XHTTP_WriteTo(NULL, &buffer, body, sizeof(body)) == XHTTP_INVALID, "A missing handle is refused");
    CHECK(XHTTP_WriteTo(&http, NULL, body, sizeof(body)) == XHTTP_INVALID, "A missing buffer is refused");
    CHECK(XHTTP_WriteTo(&http, &buffer, buffer.pData + 2, buffer.nUsed) == XHTTP_INVALID,
        "A body running past the end of its own buffer is refused");
    CHECK(XHTTP_WriteTo(&http, &buffer, body, (size_t)INT_MAX) == XHTTP_BIGCNT &&
        XHTTP_WriteTo(&http, &buffer, body, SIZE_MAX) == XHTTP_BIGCNT, "A body no buffer can hold is refused unread");
    CHECK(buffer.nUsed == 4 && !memcmp(buffer.pData, "keep", 5) && buffer.nStatus >= 0,
        "Refused messages leave the buffer as it was, and usable");

    uint8_t storage[16] = "borrowed";
    xbyte_buffer_t borrowed;
    XByteBuffer_Init(&borrowed, 0, XFALSE);
    XByteBuffer_SetData(&borrowed, storage, 8);
    CHECK(XHTTP_WriteTo(&http, &borrowed, body, sizeof(body)) == XHTTP_EALLOC, "A borrowed buffer does not grow");
    CHECK(borrowed.pData == storage && borrowed.nUsed == 8 && !memcmp(storage, "borrowed", 9), "Nor is written past");

    /* The buffer's storage past its content is where the message is written, so a body there is refused */
    CHECK(XHTTP_WriteTo(&http, &buffer, buffer.pData + buffer.nUsed, 1) == XHTTP_INVALID,
        "A body in the terminator byte is refused");
    CHECK(XByteBuffer_Reserve(&buffer, 64) > 0 && buffer.nSize >= buffer.nUsed + 64, "Reserve storage past the content");
    CHECK(XHTTP_WriteTo(&http, &buffer, buffer.pData + buffer.nUsed + 16, 8) == XHTTP_INVALID,
        "A body in reserved storage is refused");
    CHECK(buffer.nUsed == 4 && !memcmp(buffer.pData, "keep", 5) && buffer.nStatus >= 0, "The buffer is left as it was");

    /* A message that does not fit behind what the buffer holds is refused before the buffer is asked to grow. The
       buffer only claims to hold that much, and nothing of it is read or written. */
    uint8_t claimed[256] = {0};
    xbyte_buffer_t full;
    XByteBuffer_Init(&full, 0, XFALSE);
    XByteBuffer_SetData(&full, claimed, (size_t)INT_MAX - 100);
    CHECK(XHTTP_WriteTo(&http, &full, claimed, 200) == XHTTP_BIGCNT, "A message past INT_MAX in all is refused");
    CHECK(full.pData == claimed && full.nUsed == (size_t)INT_MAX - 100 && full.nStatus >= 0, "The buffer is left usable");

    /* A body taken from the buffer itself survives the buffer growing */
    for (int i = 0; i < 64; i++) CHECK(XByteBuffer_Add(&buffer, (const uint8_t*)"0123456789abcdef", 16) > 0, "Grow");
    size_t nBefore = buffer.nUsed;
    CHECK(XHTTP_WriteTo(&http, &buffer, buffer.pData, nBefore) == XHTTP_NONE, "Append from itself");
    CHECK(XHTTP_ParseData(&parsed, buffer.pData + nBefore, buffer.nUsed - nBefore) == XHTTP_COMPLETE &&
        XHTTP_GetBodySize(&parsed) == nBefore, "It parses, with the whole old content as its body");
    CHECK(!memcmp(XHTTP_GetBody(&parsed), "keep0123456789abcdef", 20), "Which is the old content");
    XHTTP_Clear(&parsed);
    XByteBuffer_Clear(&buffer);
    XHTTP_Clear(&http);
    return 0;
}

static int XTest_assemble_storage(void)
{
    /* A message that fits is written into the storage the handle already has */
    xhttp_t http, parsed;
    CHECK(XHTTP_InitResponse(&http, 200, "1.1") > 0, "A response initializes");
    const uint8_t *pStorage = http.rawData.pData;
    xbyte_buffer_t *pWire = XHTTP_Assemble(&http, (const uint8_t*)"ok", 2);
    CHECK(pWire != NULL && pStorage != NULL && pWire->pData == pStorage, "Without allocating it again");
    XHTTP_Clear(&http);

    /* A response can carry the body of the request it was parsed into the same handle from. That body was in the
       storage XHTTP_Assemble() released first, and was then copied out of freed memory. */
    const char *pRequest = "POST /echo HTTP/1.1\r\nContent-Length: 11\r\n\r\nhello world";
    CHECK(XHTTP_ParseData(&http, (uint8_t*)pRequest, strlen(pRequest)) == XHTTP_COMPLETE, "A request parses");
    const uint8_t *pBody = XHTTP_GetBody(&http);
    size_t nBody = XHTTP_GetBodySize(&http);
    CHECK(pBody != NULL && nBody == 11, "With its body");

    XHTTP_Reset(&http, XFALSE);
    http.eType = XHTTP_RESPONSE;
    http.nStatusCode = 200;

    pWire = XHTTP_Assemble(&http, pBody, nBody);
    CHECK(pWire != NULL && XHTTP_ParseData(&parsed, pWire->pData, pWire->nUsed) == XHTTP_COMPLETE,
        "The handle assembles a response from it");
    CHECK(parsed.nStatusCode == 200 && XHTTP_GetBodySize(&parsed) == 11 &&
        !memcmp(XHTTP_GetBody(&parsed), "hello world", 11), "That carries the request's body");
    XHTTP_Clear(&parsed);
    XHTTP_Clear(&http);
    return 0;
}

static int XTest_header_lookup(void)
{
    /* Header names are matched without regard to case, and a name that is
     * a prefix of another must not match it. */
    const char wire[] =
        "GET / HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Content-Type: application/json\r\n"
        "X-Long-Header-Name: value\r\n"
        "Empty-Header: \r\n"
        "Content-Length: 0\r\n\r\n";

    xhttp_t http;
    CHECK(XHTTP_ParseData(&http, (uint8_t*)wire, sizeof(wire) - 1) == XHTTP_COMPLETE, "The request parses");

    /* A header with an empty value carries nothing, so it is not stored and
     * not counted; the other four are. */
    CHECK(http.nHeaderCount == 4, "Every header with a value is counted");
    CHECK(XHTTP_GetHeader(&http, "Empty-Header") == NULL, "An empty valued header is not stored");

    CHECK(strcmp(XHTTP_GetHeader(&http, "Host"), "example.com") == 0, "A header is found by its exact name");
    CHECK(strcmp(XHTTP_GetHeader(&http, "host"), "example.com") == 0, "A header is found in lower case");
    CHECK(strcmp(XHTTP_GetHeader(&http, "HOST"), "example.com") == 0, "A header is found in upper case");
    CHECK(strcmp(XHTTP_GetHeader(&http, "HoSt"), "example.com") == 0, "A header is found in mixed case");

    CHECK(strcmp(XHTTP_GetHeader(&http, "X-Long-Header-Name"), "value") == 0, "A long header name is found");
    CHECK(strcmp(XHTTP_GetHeader(&http, "Content-Type"), "application/json") == 0, "A content type is found");

    /* A prefix of a real header name is not that header. */
    CHECK(XHTTP_GetHeader(&http, "Content") == NULL, "A prefix of a header name does not match it");
    CHECK(XHTTP_GetHeader(&http, "X-Long") == NULL, "A prefix of a long header name does not match it");
    CHECK(XHTTP_GetHeader(&http, "Absent") == NULL, "A header that is not there is not found");
    CHECK(XHTTP_GetHeader(&http, "") == NULL, "An empty header name finds nothing");

    /* The raw header block is available as written. */
    char *pRaw = XHTTP_GetHeaderRaw(&http);
    CHECK(pRaw != NULL, "The raw header block is available");
    CHECK(strstr(pRaw, "Host: example.com") != NULL, "The raw block holds the headers as written");
    free(pRaw);

    CHECK(XHTTP_GetHeader(&http, NULL) == NULL, "A missing header name finds nothing");
    XHTTP_Clear(&http);
    return 0;
}

/* Writes a whole message into one end of a socket pair and receives it
 * through the HTTP layer at the other, which is where the size limits and
 * the incremental readers live. */
static xhttp_status_t http_receive(xhttp_t *pHttp, const char *pWire, size_t nSize,
    size_t nHeaderMax, size_t nContentMax)
{
    XSOCKET pair[2] = {XSOCK_INVALID, XSOCK_INVALID};
    if (XSock_CreatePair(pair) != XSTDOK) return XHTTP_EINIT;

    xsock_t writer, reader;
    XSock_Init(&writer, XSOCK_TCP_PEER, pair[0]);
    XSock_Init(&reader, XSOCK_TCP_PEER, pair[1]);
    XSock_TimeOutR(&reader, 2, 0);

    XSock_Write(&writer, pWire, nSize);
    XSock_Close(&writer);

    XHTTP_InitParser(pHttp, NULL, 0);
    pHttp->nHeaderMax = nHeaderMax;
    pHttp->nContentMax = nContentMax;

    xhttp_status_t eStatus = XHTTP_Receive(pHttp, &reader);
    XSock_Close(&reader);
    return eStatus;
}

static int XTest_limits(void)
{
    /* A body larger than the configured maximum is refused while it is
     * still arriving, rather than being buffered to completion first. */
    char sBody[4096];
    memset(sBody, 'b', sizeof(sBody));

    char sWire[8192];
    size_t nUsed = (size_t)snprintf(sWire, sizeof(sWire),
        "POST / HTTP/1.1\r\nContent-Length: %zu\r\n\r\n", sizeof(sBody));
    memcpy(&sWire[nUsed], sBody, sizeof(sBody));
    nUsed += sizeof(sBody);

    xhttp_t http;
    CHECK(http_receive(&http, sWire, nUsed, 0, 64) == XHTTP_BIGCNT,
        "A body past the content maximum is refused");
    XHTTP_Clear(&http);

    /* The same message inside the limit is received whole. */
    CHECK(http_receive(&http, sWire, nUsed, 0, 0) == XHTTP_COMPLETE,
        "The same message with no limit is received");
    CHECK(XHTTP_GetBodySize(&http) == sizeof(sBody), "The whole body arrived");
    CHECK(memcmp(XHTTP_GetBody(&http), sBody, sizeof(sBody)) == 0, "Every body byte arrived unchanged");
    CHECK(http.eMethod == XHTTP_POST, "The received message keeps its method");
    XHTTP_Clear(&http);

    /* A header block past the maximum is refused before the body. */
    char sBigHdr[8192];
    nUsed = (size_t)snprintf(sBigHdr, sizeof(sBigHdr), "GET / HTTP/1.1\r\n");
    while (nUsed < sizeof(sBigHdr) - 64)
        nUsed += (size_t)snprintf(&sBigHdr[nUsed], sizeof(sBigHdr) - nUsed, "X-Pad-%04zu: padding\r\n", nUsed);
    nUsed += (size_t)snprintf(&sBigHdr[nUsed], sizeof(sBigHdr) - nUsed, "\r\n");

    CHECK(http_receive(&http, sBigHdr, nUsed, 256, 0) == XHTTP_BIGHDR,
        "A header block past the maximum is refused");
    XHTTP_Clear(&http);

    /* Inside the limits the same headers are all accepted and counted. */
    CHECK(http_receive(&http, sBigHdr, nUsed, sizeof(sBigHdr) * 2, 0) == XHTTP_COMPLETE,
        "The same headers inside the limit are accepted");
    CHECK(http.nHeaderCount > 100, "Every padding header was counted");
    XHTTP_Clear(&http);

    /* A peer that closes without sending anything is not a message. */
    XSOCKET pair[2] = {XSOCK_INVALID, XSOCK_INVALID};
    CHECK(XSock_CreatePair(pair) == XSTDOK, "A socket pair is created");

    xsock_t writer, reader;
    XSock_Init(&writer, XSOCK_TCP_PEER, pair[0]);
    XSock_Init(&reader, XSOCK_TCP_PEER, pair[1]);
    XSock_TimeOutR(&reader, 2, 0);
    XSock_Close(&writer);

    CHECK(XHTTP_InitParser(&http, NULL, 0) > 0, "The parser initializes");
    CHECK(XHTTP_Receive(&http, &reader) != XHTTP_COMPLETE, "An immediate close is not a message");
    XSock_Close(&reader);
    XHTTP_Clear(&http);
    return 0;
}

/* Records the callback types a parse drives through. */
typedef struct {
    int nStatusCalls;
    int nHeaderCalls;
    int nContentCalls;
    int nErrorCalls;
    xhttp_status_t eLastStatus;
    int nTerminateAfter;
} http_cb_test_t;

static int http_callback(xhttp_t *pHttp, xhttp_ctx_t *pCtx)
{
    http_cb_test_t *pTest = (http_cb_test_t*)pHttp->pUserCtx;

    if (pCtx->eCbType == XHTTP_STATUS) { pTest->nStatusCalls++; pTest->eLastStatus = pCtx->eStatus; }
    else if (pCtx->eCbType == XHTTP_READ_HDR) pTest->nHeaderCalls++;
    else if (pCtx->eCbType == XHTTP_READ_CNT) pTest->nContentCalls++;
    else if (pCtx->eCbType == XHTTP_ERROR) pTest->nErrorCalls++;

    if (pTest->nTerminateAfter > 0 && pTest->nStatusCalls >= pTest->nTerminateAfter)
        return XSTDERR;

    return XSTDOK;
}

static int XTest_callbacks(void)
{
    /* The parser reports its progress through the callback, and a callback
     * that refuses stops the parse instead of being ignored. */
    const char wire[] = "POST /x HTTP/1.1\r\nContent-Length: 3\r\n\r\nabc";

    http_cb_test_t test;
    memset(&test, 0, sizeof(test));

    xhttp_t http;
    CHECK(XHTTP_InitParser(&http, NULL, 0) > 0, "The parser initializes");
    CHECK(XHTTP_SetCallback(&http, http_callback, &test, XHTTP_STATUS | XHTTP_ERROR) == XSTDOK,
        "The callback is installed");
    CHECK(XHTTP_AppendData(&http, (uint8_t*)wire, sizeof(wire) - 1) > 0, "The message is appended");
    CHECK(XHTTP_Parse(&http) == XHTTP_COMPLETE, "The message parses");
    CHECK(test.nStatusCalls > 0, "The parse reported its progress");
    XHTTP_Clear(&http);

    /* A callback that refuses terminates the parse. */
    memset(&test, 0, sizeof(test));
    test.nTerminateAfter = 1;

    CHECK(XHTTP_InitParser(&http, NULL, 0) > 0, "The parser initializes");
    CHECK(XHTTP_SetCallback(&http, http_callback, &test, XHTTP_STATUS | XHTTP_ERROR) == XSTDOK,
        "The refusing callback is installed");
    CHECK(XHTTP_AppendData(&http, (uint8_t*)wire, sizeof(wire) - 1) > 0, "The message is appended");
    CHECK(XHTTP_Parse(&http) == XHTTP_TERMINATED, "A refusing callback terminates the parse");
    XHTTP_Clear(&http);
    return 0;
}

static int XTest_lifecycle(void)
{
    /* The heap handle owns itself and clears the caller pointer. */
    xhttp_t *pHttp = XHTTP_Alloc(XHTTP_GET, 0);
    CHECK(pHttp != NULL, "A heap handle is allocated");
    CHECK(pHttp->eMethod == XHTTP_GET, "The heap handle keeps its method");
    CHECK(pHttp->nAllocated == XTRUE, "The heap handle is marked as owned");
    XHTTP_Free(&pHttp);
    CHECK(pHttp == NULL, "Freeing clears the caller pointer");

    /* A stack handle is not freed by the same call. */
    xhttp_t http;
    CHECK(XHTTP_Init(&http, XHTTP_PUT, 0) >= 0, "A stack handle initializes");
    CHECK(http.nAllocated == XFALSE, "A stack handle is not marked as owned");
    xhttp_t *pStack = &http;
    XHTTP_Free(&pStack);
    CHECK(pStack == &http, "A non-owned handle pointer is left alone");

    /* A soft reset keeps the configuration; a hard reset clears it. */
    CHECK(XHTTP_Init(&http, XHTTP_POST, 0) >= 0, "The handle initializes");
    CHECK(XHTTP_AddHeader(&http, "X-Kept", "yes") > 0, "A header is added");
    http.nContentMax = 1234;

    XHTTP_Reset(&http, XFALSE);
    CHECK(http.nContentMax == 1234, "A soft reset keeps the configured limits");

    XHTTP_Reset(&http, XTRUE);
    CHECK(http.rawData.nUsed == 0, "A hard reset empties the buffer");

    /* Copying duplicates the message rather than aliasing it. */
    CHECK(XHTTP_Init(&http, XHTTP_GET, 0) >= 0, "The source initializes");
    CHECK(XHTTP_InitRequest(&http, XHTTP_GET, "/source", "1.1") > 0, "The source is a request");
    CHECK(XHTTP_AddHeader(&http, "X-Source", "original") > 0, "The source gets a header");

    xhttp_t copy;
    CHECK(XHTTP_Init(&copy, XHTTP_DUMMY, 0) >= 0, "The destination initializes");
    CHECK(XHTTP_Copy(&copy, &http) == XSTDOK, "The message is copied");
    CHECK(copy.eMethod == XHTTP_GET, "The copy keeps the method");
    CHECK(strcmp(copy.sUri, "/source") == 0, "The copy keeps the target");
    CHECK(strcmp(XHTTP_GetHeader(&copy, "X-Source"), "original") == 0, "The copy keeps the headers");

    /* Clearing the source leaves the copy intact. */
    XHTTP_Clear(&http);
    CHECK(strcmp(XHTTP_GetHeader(&copy, "X-Source"), "original") == 0, "The copy outlives its source");
    XHTTP_Clear(&copy);

    /* Clearing twice is safe. */
    XHTTP_Clear(&copy);
    XHTTP_Free(NULL);
    return 0;
}

static int XTest_unix_and_auth(void)
{
    /* A unix socket target is recorded alongside the ordinary fields. */
    xhttp_t http;
    CHECK(XHTTP_InitRequest(&http, XHTTP_GET, "/status", "1.1") > 0, "The request initializes");
    CHECK(XHTTP_SetUnixAddr(&http, "/var/run/app.sock") > 0, "The unix address is set");
    CHECK(strcmp(http.sUnixAddr, "/var/run/app.sock") == 0, "The unix address is recorded");
    XHTTP_Clear(&http);

    /* The token builder produces the bare base64 of user:password; the
     * scheme name is added by the header setter. */
    char sToken[256];
    CHECK(XHTTP_GetAuthToken(sToken, sizeof(sToken), "user", "pass") > 0, "The auth token is built");
    CHECK(strcmp(sToken, "dXNlcjpwYXNz") == 0, "The token is the base64 of the credentials");

    size_t nDecodedLen = strlen(sToken);
    char *pDecoded = XBase64_Decrypt((const uint8_t*)sToken, &nDecodedLen);
    CHECK(pDecoded != NULL && nDecodedLen == 9, "The token decodes to the credential pair");
    CHECK(memcmp(pDecoded, "user:pass", 9) == 0, "The decoded token is user and password joined by a colon");
    free(pDecoded);

    /* The header setter wraps the same token in the basic scheme. */
    CHECK(XHTTP_InitRequest(&http, XHTTP_GET, "/", "1.1") > 0, "The request initializes");
    CHECK(XHTTP_SetAuthBasic(&http, "user", "pass") > 0, "Basic auth is set on the request");

    const char *pHeader = XHTTP_GetHeader(&http, "Authorization");
    CHECK(pHeader != NULL, "The authorization header is present");
    CHECK(strncmp(pHeader, "Basic ", 6) == 0, "The header announces the basic scheme");
    CHECK(strcmp(&pHeader[6], sToken) == 0, "The header carries the token the builder produced");

    /* The credential survives assembly and reparsing unchanged. */
    xbyte_buffer_t *pWire = XHTTP_Assemble(&http, NULL, 0);
    CHECK(pWire != NULL, "The authenticated request assembles");

    xhttp_t parsed;
    CHECK(XHTTP_ParseData(&parsed, pWire->pData, pWire->nUsed) == XHTTP_COMPLETE,
        "The authenticated request parses back");
    CHECK(strcmp(XHTTP_GetHeader(&parsed, "Authorization"), pHeader) == 0,
        "The authorization header survives the round trip");
    XHTTP_Clear(&parsed);
    XHTTP_Clear(&http);

    /* Empty credentials still produce the base64 of a bare colon. */
    CHECK(XHTTP_GetAuthToken(sToken, sizeof(sToken), "", "") > 0, "An empty credential pair still builds a token");
    CHECK(strcmp(sToken, "Og==") == 0, "An empty credential pair encodes a bare colon");
    CHECK(XHTTP_GetAuthToken(NULL, 16, "u", "p") == 0, "A missing destination builds nothing");
    return 0;
}

static int XTest_many_header_lines(void)
{
    /* A header made of many short lines. Splitting it measured the whole
       header again for every line, so this took minutes instead of
       milliseconds and held up everything else on the event loop. */
    enum
    {
        HEADER_LINES = 150000
    };
    xbyte_buffer_t wire;
    XByteBuffer_Init(&wire, 0, XTRUE);
    CHECK(XByteBuffer_AddFmt(&wire, "GET /many HTTP/1.1\r\n") > 0, "Start the request line");
    for (int i = 0; i < HEADER_LINES; i++)
        CHECK(XByteBuffer_Add(&wire, (const uint8_t*)"a:b\r\n", 5) > 0, "Append one repeated header line");
    CHECK(XByteBuffer_AddFmt(&wire, "X-Last: yes\r\n\r\n") > 0, "Terminate the header");

    xhttp_t http;
    xhttp_status_t eStatus = XHTTP_ParseData(&http, wire.pData, wire.nUsed);
    CHECK(eStatus == XHTTP_COMPLETE, "A header of many lines still parses as one complete request");
    CHECK(strcmp(http.sUri, "/many") == 0, "The request line survives the large header");
    CHECK(http.nHeaderCount == 2, "A repeated header is stored once next to the last distinct one");
    CHECK(strcmp(XHTTP_GetHeader(&http, "a"), "b") == 0 && strcmp(XHTTP_GetHeader(&http, "x-last"), "yes") == 0,
        "Both distinct header values are kept");

    XHTTP_Clear(&http);
    XByteBuffer_Clear(&wire);
    return 0;
}

static int XTest_header_delimiters(void)
{
    const char wire[] =
        "GET / HTTP/1.1\r\n"
        "X-Inner: a\rb\nc\r\rd\n\ne\r\n"
        "X-Tail: value\r\r\n"
        "X-Last: yes\r\n"
        "Content-Length: 15\r\n\r\n"
        "X-Body: no\r\n\r\nx";

    xhttp_t http;
    CHECK(XHTTP_ParseData(&http, (uint8_t*)wire, sizeof(wire) - 1) == XHTTP_COMPLETE, "Headers end only at a CRLF pair");
    CHECK(http.nHeaderCount == 4, "Only header lines before the body are counted");
    const char *pValue = XHTTP_GetHeader(&http, "X-Inner");
    CHECK(pValue != NULL && !strcmp(pValue, "a\rb\nc\r\rd\n\ne"), "Unpaired line delimiters stay in the value");
    pValue = XHTTP_GetHeader(&http, "X-Tail");
    CHECK(pValue != NULL && !strcmp(pValue, "value\r"), "A carriage return before the delimiter stays in the value");
    pValue = XHTTP_GetHeader(&http, "X-Last");
    CHECK(pValue != NULL && !strcmp(pValue, "yes"), "The final named header is parsed");
    CHECK(XHTTP_GetHeader(&http, "X-Body") == NULL, "Body lines are not parsed as headers");
    CHECK(!memcmp(http.rawData.pData, wire, sizeof(wire)), "Parsing preserves every byte of the message");
    XHTTP_Clear(&http);
    return 0;
}

static int XTest_start_line_colon(void)
{
    /* The start line is not a header whatever it holds: a colon in a request
       target (an absolute URI, a port, a query) or in a reason phrase must not
       make it one. */
    const char request[] =
        "GET http://example.com:8080/a:b?c=d:e HTTP/1.1\r\n"
        "Host: example.com\r\n\r\n";

    xhttp_t http;
    CHECK(XHTTP_ParseData(&http, (uint8_t*)request, sizeof(request) - 1) == XHTTP_COMPLETE,
        "A request whose target holds colons parses");
    CHECK(http.nHeaderCount == 1, "Only the real header is counted");
    CHECK(XHTTP_GetHeader(&http, "get http") == NULL, "The request line is not stored as a header");
    CHECK(strcmp(http.sUri, "http://example.com:8080/a:b?c=d:e") == 0, "The target keeps its colons");
    CHECK(strcmp(XHTTP_GetHeader(&http, "Host"), "example.com") == 0, "The real header is kept");
    XHTTP_Clear(&http);

    /* Its body ends with the connection, so the header is all a parse can complete */
    const char response[] = "HTTP/1.1 200 OK: fine\r\nX-A: 1\r\n\r\n";
    CHECK(XHTTP_ParseData(&http, (uint8_t*)response, sizeof(response) - 1) == XHTTP_PARSED,
        "A response whose reason phrase holds a colon parses");
    CHECK(http.nStatusCode == 200 && http.nHeaderCount == 1, "The status line is not counted as a header");
    CHECK(XHTTP_GetHeader(&http, "http/1.1 200 ok") == NULL, "The status line is not stored as a header");
    XHTTP_Clear(&http);

    /* A start line with nothing but a colon after the method still parses */
    const char bare[] = "GET /:: HTTP/1.1\r\n\r\n";
    CHECK(XHTTP_ParseData(&http, (uint8_t*)bare, sizeof(bare) - 1) == XHTTP_COMPLETE, "A target of colons parses");
    CHECK(http.nHeaderCount == 0 && strcmp(http.sUri, "/::") == 0, "And carries no headers at all");
    XHTTP_Clear(&http);
    return 0;
}

static int XTest_long_header_line(void)
{
    /* A header line is kept whole however long it is. A bearer token or a
       cookie can run to kilobytes, and a clipped value is a different value. */
    const size_t nLengths[] = { 8180, 8189, 8190, 8191, 8192, 8193, 8200, 16384, 30000 };

    for (size_t i = 0; i < sizeof(nLengths) / sizeof(*nLengths); i++)
    {
        size_t nValue = nLengths[i];
        char *pValue = (char*)malloc(nValue + 1);
        CHECK(pValue != NULL, "Allocate the long header value");

        for (size_t j = 0; j < nValue; j++) pValue[j] = (char)('a' + j % 26);
        pValue[nValue] = '\0';

        xbyte_buffer_t wire;
        XByteBuffer_Init(&wire, 0, XTRUE);
        CHECK(XByteBuffer_AddFmt(&wire, "GET / HTTP/1.1\r\nX-Before: 1\r\nCookie: %s\r\nX-After: 2\r\n\r\n", pValue) > 0,
            "Build a request with one very long header line");

        xhttp_t http;
        CHECK(XHTTP_ParseData(&http, wire.pData, wire.nUsed) == XHTTP_COMPLETE, "The request parses");

        const char *pCookie = XHTTP_GetHeader(&http, "Cookie");
        CHECK(pCookie != NULL && strlen(pCookie) == nValue && strcmp(pCookie, pValue) == 0,
            "The long header value is kept whole, byte for byte");
        CHECK(strcmp(XHTTP_GetHeader(&http, "X-Before"), "1") == 0 && strcmp(XHTTP_GetHeader(&http, "X-After"), "2") == 0,
            "The headers around the long line are unaffected");
        CHECK(http.nHeaderCount == 3, "Every header is counted once");

        XHTTP_Clear(&http);
        XByteBuffer_Clear(&wire);
        free(pValue);
    }

    return 0;
}

static int XTest_header_whitespace(void)
{
    /* Optional whitespace around a field value is not part of it (RFC 7230,
       3.2.4). What is inside the value stays as it was sent. */
    const char wire[] =
        "GET / HTTP/1.1\r\n"
        "X-Tab:\tvalue\r\n"
        "X-Trail: value \t \r\n"
        "X-Both: \t  both\t\r\n"
        "X-Only: \t \r\n"
        "X-Inner: a b\tc\r\n"
        "X-Colons::: v\r\n"
        "x-first: one\r\n"
        "X-FIRST: two\r\n"
        "X-Late:\r\n"
        "X-Late: kept\r\n"
        "no-colon-at-all\r\n"
        ": nameless\r\n\r\n";

    xhttp_t http;
    CHECK(XHTTP_ParseData(&http, (uint8_t*)wire, sizeof(wire) - 1) == XHTTP_COMPLETE, "The request parses");
    CHECK(strcmp(XHTTP_GetHeader(&http, "X-Tab"), "value") == 0, "A leading tab is not part of the value");
    CHECK(strcmp(XHTTP_GetHeader(&http, "X-Trail"), "value") == 0, "Trailing spaces and tabs are not part of it");
    CHECK(strcmp(XHTTP_GetHeader(&http, "X-Both"), "both") == 0, "Whitespace on both sides is dropped");
    CHECK(XHTTP_GetHeader(&http, "X-Only") == NULL, "A value of only whitespace is no value");
    CHECK(strcmp(XHTTP_GetHeader(&http, "X-Inner"), "a b\tc") == 0, "Whitespace inside the value is kept");
    CHECK(strcmp(XHTTP_GetHeader(&http, "X-Colons"), "v") == 0, "Extra colons after the name are skipped");
    CHECK(strcmp(XHTTP_GetHeader(&http, "X-First"), "one") == 0, "The first of repeated names wins");
    CHECK(strcmp(XHTTP_GetHeader(&http, "X-Late"), "kept") == 0, "An empty first occurrence does not claim the name");
    CHECK(XHTTP_GetHeader(&http, "no-colon-at-all") == NULL, "A line without a colon is no header");
    CHECK(http.nHeaderCount == 7, "Exactly the headers with a name and a value are stored");

    /* Stored names are lower case, whatever the spelling on the wire */
    CHECK(XMap_Get(&http.headerMap, "x-tab") != NULL && XMap_Get(&http.headerMap, "X-Tab") == NULL,
        "Header names are stored in lower case");
    XHTTP_Clear(&http);
    return 0;
}

static int XTest_parse_restores_buffer(void)
{
    /* The parser works on the caller's bytes. Whatever the outcome they are
       left as they were, so the same buffer can be parsed again or passed on. */
    const char *pWires[] = {
        "GARBAGE WITHOUT A VERSION\r\n\r\n",
        "HTTP/1.1 999 Out Of Range\r\n\r\n",
        "HTTP/1.1 42 Too Low\r\n\r\n",
        "PATCH / HTTP/1.1\r\nHost: x\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: x\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nbody",
        "GET / HTTP/1.1\r\nContent-Length: 10\r\n\r\npart",
        "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n4\r\nbody\r\n0\r\n\r\nGET / HTTP/1.1\r\n\r\n",
        "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n4\r\nbo"
    };

    for (size_t i = 0; i < sizeof(pWires) / sizeof(*pWires); i++)
    {
        size_t nLength = strlen(pWires[i]);
        uint8_t *pCopy = (uint8_t*)malloc(nLength + 1);
        CHECK(pCopy != NULL, "Allocate the caller owned wire");
        memcpy(pCopy, pWires[i], nLength + 1);

        for (int nPass = 0; nPass < 2; nPass++)
        {
            xhttp_t http;
            XHTTP_Init(&http, XHTTP_DUMMY, XSTDNON);
            XByteBuffer_SetData(&http.rawData, pCopy, nLength);
            xhttp_status_t eStatus = XHTTP_Parse(&http);

            CHECK(memcmp(pCopy, pWires[i], nLength + 1) == 0, "The caller's bytes are unchanged after any outcome");
            CHECK(i < 4 ? eStatus == XHTTP_INVALID : eStatus != XHTTP_INVALID, "Only the malformed messages are invalid");
            XHTTP_Clear(&http);
        }

        free(pCopy);
    }

    return 0;
}

static int XTest_api_guards(void)
{
    xhttp_t http, copy;
    char sToken[64];

    CHECK(XHTTP_SetUnixAddr(NULL, "/a") == (size_t)XSTDERR && XHTTP_SetUnixAddr(&http, "") == (size_t)XSTDERR,
        "A unix address needs a handle and a path");
    CHECK(XHTTP_Copy(NULL, &http) == XSTDERR && XHTTP_Copy(&copy, NULL) == XSTDERR, "A copy needs both handles");
    XHTTP_Free(NULL);

    CHECK(!XHTTP_GetAuthToken(sToken, 0, "u", "p") && !XHTTP_GetAuthToken(sToken, 8, NULL, "p") &&
        !XHTTP_GetAuthToken(sToken, 8, "u", NULL), "A token needs room and both credentials");
    CHECK(!XHTTP_GetAuthToken(sToken, 4, "user", "pass") && sToken[0] == '\0', "A token that does not fit is none");

    CHECK(XHTTP_InitRequest(&http, XHTTP_GET, "/", "1.1") > 0, "The request initializes");
    CHECK(XHTTP_SetAuthBasic(&http, "user", "") == XSTDNON && XHTTP_GetHeader(&http, "Authorization") == NULL,
        "Basic auth needs both credentials");

    /* Setting a header again to what it holds is no update; to anything else it is, and needs permission */
    CHECK(XHTTP_AddHeader(&http, "X-Kept", "one") == 1, "A header is added");
    CHECK(XHTTP_AddHeader(&http, "X-Kept", "one") == 1, "The same value again changes nothing");
    CHECK(XHTTP_AddHeader(&http, "X-Kept", "two") == XSTDEXC, "Another value is refused without permission");
    CHECK(!strcmp(XHTTP_GetHeader(&http, "X-Kept"), "one"), "And the header keeps its value");

    /* Keep-alive asks for a connection header unless one is set */
    http.nKeepAlive = XTRUE;
    CHECK(XHTTP_Assemble(&http, NULL, 0) != NULL, "The request assembles");
    CHECK(!strcmp(XHTTP_GetHeader(&http, "Connection"), "keep-alive"), "A kept connection is announced");
    CHECK(XHTTP_Assemble(&http, NULL, 0) == &http.rawData, "Assembling it again gives what is already there");

    char *pRaw = XHTTP_GetHeaderRaw(&http);
    CHECK(pRaw != NULL && strstr(pRaw, "X-Kept: one\r\n") != NULL, "The raw header is what was assembled");
    free(pRaw);
    XHTTP_Clear(&http);

    CHECK(XHTTP_GetHeaderRaw(NULL) == NULL && XHTTP_GetHeaderRaw(&http) == NULL, "No header has no raw form");
    CHECK(!XHTTP_GetExtraSize(NULL) && !XHTTP_GetPacketSize(NULL), "No handle has no extra data or size");

    /* A header and its body, with nothing after it */
    const char *pWire = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
    CHECK(XHTTP_ParseData(&http, (uint8_t*)pWire, strlen(pWire)) == XHTTP_COMPLETE, "A response parses");
    CHECK(XHTTP_GetExtraData(&http) == NULL && XHTTP_GetPacketSize(&http) == strlen(pWire), "Nothing follows it");
    XHTTP_Clear(&http);

    /* A length too big for the type is no length, and the message has no framing */
    pWire = "POST / HTTP/1.1\r\nContent-Length: 99999999999999999999999\r\n\r\n";
    CHECK(XHTTP_ParseData(&http, (uint8_t*)pWire, strlen(pWire)) == XHTTP_INVALID, "An oversized length is invalid");
    XHTTP_Clear(&http);

    /* The biggest one there is can be held */
    char sLargest[96];
    snprintf(sLargest, sizeof(sLargest), "POST / HTTP/1.1\r\nContent-Length: %zu\r\n\r\nab", (size_t)SIZE_MAX);
    CHECK(XHTTP_ParseData(&http, (uint8_t*)sLargest, strlen(sLargest)) == XHTTP_PARSED &&
        http.nContentLength == SIZE_MAX, "The largest length a size holds is a length");
    XHTTP_Clear(&http);

    /* Spaces around the target are not part of it */
    pWire = "GET    /spaced/path    HTTP/1.1\r\nHost: x\r\n\r\n";
    CHECK(XHTTP_ParseData(&http, (uint8_t*)pWire, strlen(pWire)) == XHTTP_COMPLETE, "A spaced request line parses");
    CHECK(!strcmp(http.sUri, "/spaced/path"), "The target is what is between the spaces");
    XHTTP_Clear(&http);

    pWire = "HTTP/1.1\r\n\r\n";
    CHECK(XHTTP_ParseData(&http, (uint8_t*)pWire, strlen(pWire)) == XHTTP_INVALID, "A status line without a code is invalid");
    XHTTP_Clear(&http);
    return 0;
}

static int XTest_null_args(void)
{
    /* No request, response, socket or link: each call reports it instead of faulting. The status
       callback is reached on some of these paths, and it is given no handle either. */
    xhttp_t http;
    xsock_t sock;
    xlink_t link;
    xbyte_buffer_t buffer;
    uint8_t sData[] = "GET / HTTP/1.1\r\n\r\n";

    CHECK(XHTTP_GetMethodType(NULL) == XHTTP_DUMMY && XHTTP_IsSuccessCode(NULL) == XFALSE, "No method or code");
    CHECK(XHTTP_Init(NULL, XHTTP_GET, 0) == XSTDERR && XHTTP_InitParser(NULL, sData, sizeof(sData)) == XSTDERR, "Nothing is set up");
    CHECK(XHTTP_InitRequest(NULL, XHTTP_GET, "/", NULL) == XSTDERR && XHTTP_InitResponse(NULL, 200, NULL) == XSTDERR,
        "Neither as a request nor as a response");
    CHECK(XHTTP_SetAuthBasic(NULL, "user", "pass") == XSTDERR && XHTTP_AppendData(NULL, sData, 4) == XSTDERR, "Nor added to");
    XHTTP_Reset(NULL, XTRUE);
    XHTTP_Clear(NULL);
    CHECK(XHTTP_Assemble(NULL, NULL, 0) == NULL, "Nothing is assembled");
    CHECK(XHTTP_Parse(NULL) == XHTTP_INVALID && XHTTP_ParseData(NULL, sData, sizeof(sData)) == XHTTP_INVALID, "Or parsed");
    CHECK(XHTTP_ParseBuff(NULL, &buffer) == XHTTP_INVALID && XHTTP_ParseBuff(&http, NULL) == XHTTP_INVALID, "From a buffer either");
    CHECK(XHTTP_ReadHeader(NULL, &sock) == XHTTP_INVALID && XHTTP_ReadHeader(&http, NULL) == XHTTP_INVALID, "Nothing is read");
    CHECK(XHTTP_ReadContent(NULL, &sock) == XHTTP_INVALID && XHTTP_ReadContent(&http, NULL) == XHTTP_INVALID, "Header or content");
    CHECK(XHTTP_Receive(NULL, &sock) == XHTTP_INVALID, "Nor received");
    CHECK(XHTTP_Exchange(NULL, &http, &sock) == XHTTP_INVALID && XHTTP_Exchange(&http, NULL, &sock) == XHTTP_INVALID,
        "Nothing is exchanged");
    CHECK(XHTTP_Exchange(&http, &http, NULL) == XHTTP_INVALID, "Without a socket");
    CHECK(XHTTP_Perform(NULL, &sock, NULL, 0) == XHTTP_INVALID && XHTTP_Perform(&http, NULL, NULL, 0) == XHTTP_INVALID,
        "Nor performed");

    /* A connection made from nothing leaves the socket closed, so a caller can close it as always */
    CHECK(XHTTP_Connect(&http, NULL, &link) == XHTTP_INVALID, "No socket is connected");
    CHECK(XHTTP_Connect(NULL, &sock, &link) == XHTTP_INVALID && sock.nFD == XSOCK_INVALID, "No request connects");
    CHECK(XHTTP_Connect(&http, &sock, NULL) == XHTTP_INVALID && sock.nFD == XSOCK_INVALID, "Nor to no link");
    CHECK(XHTTP_LinkExchange(NULL, &http, &link) == XHTTP_INVALID && XHTTP_LinkPerform(NULL, &link, NULL, 0) == XHTTP_INVALID,
        "Nor through a link");
    CHECK(XHTTP_EasyExchange(NULL, &http, "http://localhost/") == XHTTP_INVALID, "Nor by its address");
    CHECK(XHTTP_EasyExchange(&http, NULL, "http://localhost/") == XHTTP_INVALID, "To no response");
    CHECK(XHTTP_EasyPerform(NULL, "http://localhost/", NULL, 0) == XHTTP_INVALID, "Nor performed by its address");
    return 0;
}

XTEST_MAIN(
    XTEST_CASE(null_args),
    XTEST_CASE(api_guards),
    XTEST_CASE(partial),
    XTEST_CASE(pipeline),
    XTEST_CASE(headers),
    XTEST_CASE(basic_auth),
    XTEST_CASE(long_basic_auth),
    XTEST_CASE(reuse),
    XTEST_CASE(empty_body),
    XTEST_CASE(assemble_empty_body),
    XTEST_CASE(code_strings),
    XTEST_CASE(content_length),
    XTEST_CASE(start_line),
    XTEST_CASE(assemble),
    XTEST_CASE(assemble_to),
    XTEST_CASE(assemble_storage),
    XTEST_CASE(header_lookup),
    XTEST_CASE(limits),
    XTEST_CASE(callbacks),
    XTEST_CASE(lifecycle),
    XTEST_CASE(unix_and_auth),
    XTEST_CASE(many_header_lines),
    XTEST_CASE(header_delimiters),
    XTEST_CASE(start_line_colon),
    XTEST_CASE(long_header_line),
    XTEST_CASE(header_whitespace),
    XTEST_CASE(parse_restores_buffer)
)
