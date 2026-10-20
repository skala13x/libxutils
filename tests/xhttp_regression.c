/* libxutils: the xhttp tool against a local server that answers as each case needs: downloads complete, cut
 * short and refused, redirects that resolve and redirects that loop, bodies and requests with NUL bytes in them,
 * and the arguments that make the request. The tool is compiled in with its main renamed. */
#define _GNU_SOURCE
#include "test.h"
#include <arpa/inet.h>
#include <stdarg.h>
#include <fcntl.h>
#include <getopt.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define main xhttp_main
#include "../tools/xhttp.c"
#undef main

/* ---------------- the failing calls ---------------- */

/* Only the .part file's descriptor is failed: the sockets and the test's own files go through as they are */
static int g_nPartFD = -1;
static size_t g_nShortWrite = 0;    /* The most a write to the .part file takes, 0 for all of it */
static int g_bFailClose = 0;
static int g_bFailRename = 0;
static int g_nPartWrites = 0;
static int g_nInterruptWrite = 0;   /* The write to the .part file that a signal interrupts, counted from 1 */

int __real_open(const char *pPath, int nFlags, ...);
ssize_t __real_write(int nFD, const void *pData, size_t nSize);
int __real_close(int nFD);
int __real_rename(const char *pOld, const char *pNew);

int __wrap_open(const char *pPath, int nFlags, ...)
{
    mode_t nMode = 0;
    if (nFlags & O_CREAT)
    {
        va_list args;
        va_start(args, nFlags);
        nMode = va_arg(args, mode_t);
        va_end(args);
    }

    int nFD = __real_open(pPath, nFlags, nMode);
    size_t nLength = strlen(pPath);
    if (nFD >= 0 && nLength > 5 && !strcmp(pPath + nLength - 5, ".part")) g_nPartFD = nFD;
    return nFD;
}

ssize_t __wrap_write(int nFD, const void *pData, size_t nSize)
{
    if (nFD == g_nPartFD)
    {
        if (++g_nPartWrites == g_nInterruptWrite)
        {
            errno = EINTR;
            return -1;
        }

        if (g_nShortWrite && nSize > g_nShortWrite) nSize = g_nShortWrite;
    }

    return __real_write(nFD, pData, nSize);
}

/* A failed close has still closed the descriptor */
int __wrap_close(int nFD)
{
    int nStatus = __real_close(nFD);
    if (nFD != g_nPartFD) return nStatus;

    g_nPartFD = -1;
    if (g_bFailClose) { errno = EIO; return -1; }
    return nStatus;
}

int __wrap_rename(const char *pOld, const char *pNew)
{
    if (g_bFailRename) { errno = EXDEV; return -1; }
    return __real_rename(pOld, pNew);
}

static void faults_clear(void)
{
    g_nShortWrite = 0;
    g_bFailClose = g_bFailRename = 0;
    g_nPartWrites = 0;
    g_nInterruptWrite = 0;
}

/* ---------------- the server ---------------- */

static const uint8_t g_binary[] = { 'b', 'i', 'n', 0, 1, 2, 0, 0, 'a', 'r', 'y', 0xff, '\n', 0 };

typedef struct {
    pthread_t thread;
    int nListen;
    uint16_t nPort;
    volatile int bStop;
    int nRequests;
    char sLast[16384];
    size_t nLast;
} xtest_server_t;

static xtest_server_t g_server;

static void server_send(int nFD, const void *pData, size_t nSize)
{
    const uint8_t *pBytes = (const uint8_t*)pData;
    while (nSize > 0)
    {
        ssize_t nSent = send(nFD, pBytes, nSize, MSG_NOSIGNAL);
        if (nSent <= 0) return;
        pBytes += nSent;
        nSize -= (size_t)nSent;
    }
}

static void server_reply(int nFD, const char *pStatus, const char *pHeaders, const void *pBody, size_t nBody)
{
    char sHead[1024];
    int nHead = snprintf(sHead, sizeof(sHead), "HTTP/1.1 %s\r\n%sContent-Length: %zu\r\n\r\n",
        pStatus, pHeaders, nBody);
    server_send(nFD, sHead, (size_t)nHead);
    server_send(nFD, pBody, nBody);
}

/* Reads a request, its body by its Content-Length, into the server's last request */
static void server_read(int nFD)
{
    g_server.nLast = 0;
    size_t nBody = 0, nHeader = 0;

    while (g_server.nLast < sizeof(g_server.sLast) - 1)
    {
        ssize_t nRead = recv(nFD, g_server.sLast + g_server.nLast, sizeof(g_server.sLast) - 1 - g_server.nLast, 0);
        if (nRead <= 0) break;
        g_server.nLast += (size_t)nRead;
        g_server.sLast[g_server.nLast] = XSTR_NUL;

        if (!nHeader)
        {
            char *pEnd = strstr(g_server.sLast, "\r\n\r\n");
            if (pEnd == NULL) continue;

            nHeader = (size_t)(pEnd - g_server.sLast) + 4;
            char *pLength = strstr(g_server.sLast, "Content-Length: ");
            if (pLength != NULL && pLength < pEnd) nBody = (size_t)atol(pLength + 16);
        }

        if (g_server.nLast >= nHeader + nBody) break;
    }
}

static void server_answer(int nFD)
{
    server_read(nFD);
    g_server.nRequests++;

    char sPath[256] = "";
    sscanf(g_server.sLast, "%*s %255s", sPath);

    if (!strcmp(sPath, "/file"))
        server_reply(nFD, "200 OK", "", g_binary, sizeof(g_binary));
    else if (!strcmp(sPath, "/empty"))
        server_reply(nFD, "200 OK", "", "", 0);
    else if (!strcmp(sPath, "/chunked"))
    {
        const char *pReply = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
            "5\r\nhello\r\n5\r\nworld\r\n0\r\n\r\n";
        server_send(nFD, pReply, strlen(pReply));
    }
    else if (!strcmp(sPath, "/partial"))
    {
        /* A thousand bytes promised, a hundred sent */
        char sBody[100];
        memset(sBody, 'p', sizeof(sBody));
        const char *pHead = "HTTP/1.1 200 OK\r\nContent-Length: 1000\r\n\r\n";
        server_send(nFD, pHead, strlen(pHead));
        server_send(nFD, sBody, sizeof(sBody));
    }
    else if (!strcmp(sPath, "/missing"))
        server_reply(nFD, "404 Not Found", "", "not found", 9);
    else if (!strcmp(sPath, "/loop"))
        server_reply(nFD, "302 Found", "Location: /loop\r\n", "", 0);
    else if (!strcmp(sPath, "/dir/rel"))
        server_reply(nFD, "302 Found", "Location: file2\r\n", "", 0);
    else if (!strcmp(sPath, "/dir/file2"))
        server_reply(nFD, "200 OK", "", "relative", 8);
    else if (!strcmp(sPath, "/abs"))
    {
        char sHeader[128];
        snprintf(sHeader, sizeof(sHeader), "Location: http://127.0.0.1:%u/file\r\n", g_server.nPort);
        server_reply(nFD, "301 Moved Permanently", sHeader, "", 0);
    }
    else if (!strcmp(sPath, "/echo"))
        server_reply(nFD, "200 OK", "", g_server.sLast, g_server.nLast);
    else
        server_reply(nFD, "500 Internal Server Error", "", "", 0);
}

static void *server_run(void *pArg)
{
    (void)pArg;
    while (!g_server.bStop)
    {
        struct pollfd pfd = { g_server.nListen, POLLIN, 0 };
        if (poll(&pfd, 1, 50) <= 0) continue;

        int nFD = accept(g_server.nListen, NULL, NULL);
        if (nFD < 0) continue;

        server_answer(nFD);
        shutdown(nFD, SHUT_WR);
        close(nFD);
    }

    return NULL;
}

static int server_begin(void)
{
    memset(&g_server, 0, sizeof(g_server));
    g_server.nListen = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(g_server.nListen >= 0, "Create the listening socket");

    int nOn = 1;
    setsockopt(g_server.nListen, SOL_SOCKET, SO_REUSEADDR, &nOn, sizeof(nOn));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t nLength = sizeof(addr);

    CHECK(bind(g_server.nListen, (struct sockaddr*)&addr, sizeof(addr)) == 0 && listen(g_server.nListen, 16) == 0 &&
        getsockname(g_server.nListen, (struct sockaddr*)&addr, &nLength) == 0, "Listen on a port of its own");
    g_server.nPort = ntohs(addr.sin_port);

    CHECK(pthread_create(&g_server.thread, NULL, server_run, NULL) == 0, "Start answering");
    alarm(60);
    return 0;
}

static void server_end(void)
{
    g_server.bStop = 1;
    pthread_join(g_server.thread, NULL);
    close(g_server.nListen);
    alarm(0);
}

/* ---------------- the tool ---------------- */

static char g_sDir[128];
static char g_sOut[16384 * 2];
static size_t g_nOut;

/* Runs the tool with these arguments after the program name, as its main does but for the SSL teardown, which
   is for the process to do once. What it writes to standard output is kept in g_sOut. */
static int xhttp_run(const char **pArgs, int *pStatus)
{
    const char *argv[32] = { "xhttp" };
    int argc = 1;
    while (pArgs[argc - 1] != NULL && argc < 31) { argv[argc] = pArgs[argc - 1]; argc++; }
    argv[argc] = NULL;

    char sCapture[256];
    snprintf(sCapture, sizeof(sCapture), "%s/stdout", g_sDir);

    fflush(stdout);
    int nSaved = dup(STDOUT_FILENO);
    int nCapture = open(sCapture, O_RDWR | O_CREAT | O_TRUNC, 0600);
    CHECK(nSaved >= 0 && nCapture >= 0, "Set up the capture of standard output");
    dup2(nCapture, STDOUT_FILENO);

    xhttp_args_t args;
    optind = 0;
    int bParsed = XHTTPApp_ParseArgs(&args, argc, (char**)argv);
    *pStatus = bParsed ? XHTTPApp_Run(&args) : 99;
    XByteBuffer_Clear(&args.content);
    xlog_setfl(XLOG_NONE);

    fflush(stdout);
    dup2(nSaved, STDOUT_FILENO);
    close(nSaved);

    lseek(nCapture, 0, SEEK_SET);
    ssize_t nRead = read(nCapture, g_sOut, sizeof(g_sOut) - 1);
    g_nOut = nRead > 0 ? (size_t)nRead : 0;
    g_sOut[g_nOut] = XSTR_NUL;
    close(nCapture);
    unlink(sCapture);
    return 0;
}

static void link_to(char *pLink, size_t nSize, const char *pPath)
{
    snprintf(pLink, nSize, "http://127.0.0.1:%u%s", g_server.nPort, pPath);
}

static void path_to(char *pPath, size_t nSize, const char *pName)
{
    snprintf(pPath, nSize, "%s/%s", g_sDir, pName);
}

static int file_is(const char *pName, const void *pData, size_t nSize)
{
    char sPath[256];
    path_to(sPath, sizeof(sPath), pName);
    xbyte_buffer_t buffer;
    size_t nLoaded = XPath_LoadBuffer(sPath, &buffer);
    int bSame = nLoaded == nSize && (!nSize || !memcmp(buffer.pData, pData, nSize));
    XByteBuffer_Clear(&buffer);
    return bSame;
}

static int file_exists(const char *pName)
{
    char sPath[256];
    path_to(sPath, sizeof(sPath), pName);
    struct stat st;
    return stat(sPath, &st) == 0;
}

static int case_begin(void)
{
    faults_clear();
    strcpy(g_sDir, "/tmp/xutils-xhttp-XXXXXX");
    CHECK(mkdtemp(g_sDir) != NULL, "Create a directory of its own");
    return server_begin();
}

static void case_end(void)
{
    server_end();
    XDir_Remove(g_sDir);
}

/* ---------------- the cases ---------------- */

static int XTest_download_complete(void)
{
    CHECK(case_begin() == 0, "Start the server");
    char sLink[256], sOut[256];
    int nStatus;

    link_to(sLink, sizeof(sLink), "/file");
    path_to(sOut, sizeof(sOut), "out.bin");
    const char *pFile[] = { "-l", sLink, "-o", sOut, "-t", "10", NULL };
    CHECK(xhttp_run(pFile, &nStatus) == 0 && nStatus == XSTDNON, "A download that is all there succeeds");
    CHECK(file_is("out.bin", g_binary, sizeof(g_binary)), "Into the output, byte for byte");
    CHECK(!file_exists("out.bin.part"), "With no .part file left");

    link_to(sLink, sizeof(sLink), "/chunked");
    path_to(sOut, sizeof(sOut), "chunked.txt");
    CHECK(xhttp_run(pFile, &nStatus) == 0 && nStatus == XSTDNON && file_is("chunked.txt", "helloworld", 10),
        "So does one in chunks");

    link_to(sLink, sizeof(sLink), "/empty");
    path_to(sOut, sizeof(sOut), "empty.txt");
    CHECK(xhttp_run(pFile, &nStatus) == 0 && nStatus == XSTDNON && file_is("empty.txt", "", 0) &&
        !file_exists("empty.txt.part"), "And an empty one makes an empty file");

    /* Written to the file a few bytes at a time, one of them interrupted, it is still all there */
    link_to(sLink, sizeof(sLink), "/file");
    path_to(sOut, sizeof(sOut), "pieces.bin");
    faults_clear();
    g_nShortWrite = 3;
    g_nInterruptWrite = 2;
    CHECK(xhttp_run(pFile, &nStatus) == 0, "Download a file that takes its writes a few bytes at a time");
    int nWrites = g_nPartWrites;
    faults_clear();
    CHECK(nStatus == XSTDNON && nWrites > (int)(sizeof(g_binary) + 2) / 3 && file_is("pieces.bin", g_binary, sizeof(g_binary)),
        "Every byte reaches it, the interrupted write made again");

    case_end();
    return 0;
}

static int XTest_download_finish(void)
{
    CHECK(case_begin() == 0, "Start the server");
    char sLink[256], sOut[XPATH_MAX];
    int nStatus;

    /* A download whose file failed to close, or to take its name, is not done */
    link_to(sLink, sizeof(sLink), "/file");
    path_to(sOut, sizeof(sOut), "closed.bin");
    const char *pFile[] = { "-l", sLink, "-o", sOut, "-t", "10", NULL };
    g_bFailClose = 1;
    CHECK(xhttp_run(pFile, &nStatus) == 0, "Download a file whose close fails");
    faults_clear();
    CHECK(nStatus == XSTDERR && !file_exists("closed.bin"), "It fails, and has no output");

    path_to(sOut, sizeof(sOut), "renamed.bin");
    g_bFailRename = 1;
    CHECK(xhttp_run(pFile, &nStatus) == 0, "Download a file that cannot take its name");
    faults_clear();
    CHECK(nStatus == XSTDERR && !file_exists("renamed.bin") && file_is("renamed.bin.part", g_binary, sizeof(g_binary)),
        "It fails, and what came stays in its .part file");

    /* An output whose .part name does not fit is not downloaded at all: the name cut to fit was the output's
       own, which -w then let the download truncate before a byte of it had come */
    size_t nUsed = (size_t)snprintf(sOut, sizeof(sOut), "%s", g_sDir);
    while (sizeof(sOut) - 1 - nUsed > 201)
    {
        sOut[nUsed++] = '/';
        memset(sOut + nUsed, 'd', 200);
        nUsed += 200;
        sOut[nUsed] = XSTR_NUL;
        CHECK(mkdir(sOut, 0700) == 0, "Make the next directory down");
    }

    sOut[nUsed++] = '/';
    memset(sOut + nUsed, 'f', sizeof(sOut) - 1 - nUsed);
    sOut[sizeof(sOut) - 1] = XSTR_NUL;
    CHECK(strlen(sOut) == XPATH_MAX - 1, "An output path as long as the tool takes");
    CHECK(XPath_Write(sOut, (const uint8_t*)"old", 3, "cwt") == 3, "With a file there already");

    link_to(sLink, sizeof(sLink), "/partial");
    const char *pLong[] = { "-l", sLink, "-o", sOut, "-w", "-t", "10", NULL };
    CHECK(xhttp_run(pLong, &nStatus) == 0 && nStatus == XSTDERR, "The download is refused");

    xbyte_buffer_t buffer;
    CHECK(XPath_LoadBuffer(sOut, &buffer) == 3 && !memcmp(buffer.pData, "old", 3), "And the file there is untouched");
    XByteBuffer_Clear(&buffer);

    case_end();
    return 0;
}

static int XTest_download_partial(void)
{
    CHECK(case_begin() == 0, "Start the server");
    char sLink[256], sOut[256];
    int nStatus;

    link_to(sLink, sizeof(sLink), "/partial");
    path_to(sOut, sizeof(sOut), "out.bin");
    const char *pArgs[] = { "-l", sLink, "-o", sOut, "-t", "10", NULL };
    CHECK(xhttp_run(pArgs, &nStatus) == 0 && nStatus == XSTDERR, "A download cut short fails");
    CHECK(!file_exists("out.bin"), "It is not given the output's name");

    char sPart[100];
    memset(sPart, 'p', sizeof(sPart));
    CHECK(file_is("out.bin.part", sPart, sizeof(sPart)), "What came of it stays in the .part file");

    /* Printed rather than saved, what came is not printed either */
    const char *pPrint[] = { "-l", sLink, "-t", "10", NULL };
    CHECK(xhttp_run(pPrint, &nStatus) == 0 && nStatus == XSTDERR && g_nOut == 0,
        "A body cut short is not printed as if it were all of it");

    case_end();
    return 0;
}

static int XTest_download_status(void)
{
    CHECK(case_begin() == 0, "Start the server");
    char sLink[256], sOut[256], sPart[256];
    int nStatus;

    /* A .part file an earlier run left is not this run's download */
    path_to(sPart, sizeof(sPart), "out.bin.part");
    CHECK(XPath_Write(sPart, (const uint8_t*)"stale", 5, "cwt") == 5, "A stale .part file");

    link_to(sLink, sizeof(sLink), "/missing");
    path_to(sOut, sizeof(sOut), "out.bin");
    const char *pArgs[] = { "-l", sLink, "-o", sOut, "-t", "10", NULL };
    CHECK(xhttp_run(pArgs, &nStatus) == 0 && nStatus == XSTDERR, "A download the server refuses fails");
    CHECK(!file_exists("out.bin") && file_is("out.bin.part", "stale", 5),
        "And the stale file does not become the output");

    /* Printed, a refusal is shown as it came, as before */
    const char *pPrint[] = { "-l", sLink, "-t", "10", NULL };
    CHECK(xhttp_run(pPrint, &nStatus) == 0 && nStatus == XSTDNON && g_nOut == 9 && !memcmp(g_sOut, "not found", 9),
        "Printed, the body of a refusal is shown");

    case_end();
    return 0;
}

static int XTest_redirects(void)
{
    CHECK(case_begin() == 0, "Start the server");
    char sLink[256], sOut[256];
    int nStatus;

    link_to(sLink, sizeof(sLink), "/loop");
    const char *pLoop[] = { "-l", sLink, "-f", "-t", "10", NULL };
    CHECK(xhttp_run(pLoop, &nStatus) == 0 && nStatus == XSTDERR, "Redirects that loop are given up");
    CHECK(g_server.nRequests == XHTTP_REDIRECT_MAX + 1, "After the request and twenty redirects");

    link_to(sLink, sizeof(sLink), "/dir/rel");
    path_to(sOut, sizeof(sOut), "rel.txt");
    const char *pRelative[] = { "-l", sLink, "-o", sOut, "-t", "10", NULL };
    CHECK(xhttp_run(pRelative, &nStatus) == 0 && nStatus == XSTDNON && file_is("rel.txt", "relative", 8),
        "A relative Location is resolved against the request's path");

    link_to(sLink, sizeof(sLink), "/abs");
    path_to(sOut, sizeof(sOut), "abs.bin");
    CHECK(xhttp_run(pRelative, &nStatus) == 0 && nStatus == XSTDNON && file_is("abs.bin", g_binary, sizeof(g_binary)),
        "An absolute one is followed as it is");

    case_end();

    /* Resolution, scheme kept */
    xlink_t link;
    CHECK(XLink_Parse(&link, "https://example.com/a/b?q=1") >= 0, "Parse the request's link");
    struct { const char *pLocation; const char *pAddress; } cases[] = {
        { "/login", "https://example.com:443/login" },
        { "next", "https://example.com:443/a/next" },
        { "next?x=1", "https://example.com:443/a/next?x=1" },
        { "?page=2", "https://example.com:443/a/b?page=2" },
        { "#top", "https://example.com:443/a/b#top" },
        { "//cdn.example.com/x", "https://cdn.example.com/x" },
        { "http://other.example/x", "http://other.example/x" },
        { "HTTPS://Other.example/y", "HTTPS://Other.example/y" },
        { "web+ext2.v-1:path", "web+ext2.v-1:path" },
        { "a:b/c", "a:b/c" },
        { "./a:b", "https://example.com:443/a/./a:b" },
        { "1http://x", "https://example.com:443/a/1http://x" }
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++)
    {
        char sAddress[XHTTP_URL_MAX];
        XHTTPApp_ResolveLocation(sAddress, sizeof(sAddress), &link, cases[i].pLocation);
        if (strcmp(sAddress, cases[i].pAddress))
        {
            fprintf(stderr, "%s: %s\n", cases[i].pLocation, sAddress);
            CHECK(0, "Each Location resolves to its address");
        }
    }

    CHECK(XLink_Parse(&link, "http://example.com") >= 0, "A link with no path");
    char sAddress[XHTTP_URL_MAX];
    XHTTPApp_ResolveLocation(sAddress, sizeof(sAddress), &link, "next");
    CHECK(!strcmp(sAddress, "http://example.com:80/next"), "A relative path resolves from the root");

    CHECK(XLink_Parse(&link, "example.com/z") >= 0, "A link with no scheme");
    XHTTPApp_ResolveLocation(sAddress, sizeof(sAddress), &link, "/y");
    CHECK(!strcmp(sAddress, "http://example.com/y"), "Is taken as http, as the link itself is");
    return 0;
}

static int XTest_print_body(void)
{
    CHECK(case_begin() == 0, "Start the server");
    char sLink[256];
    int nStatus;

    link_to(sLink, sizeof(sLink), "/file");
    const char *pArgs[] = { "-l", sLink, "-t", "10", NULL };
    CHECK(xhttp_run(pArgs, &nStatus) == 0 && nStatus == XSTDNON, "A body is printed");
    CHECK(g_nOut == sizeof(g_binary) && !memcmp(g_sOut, g_binary, sizeof(g_binary)),
        "As it came, every byte after a NUL included, with nothing after it off a terminal");

    case_end();
    return 0;
}

static int XTest_request_args(void)
{
    CHECK(case_begin() == 0, "Start the server");
    char sLink[256];
    int nStatus;

    link_to(sLink, sizeof(sLink), "/echo");
    const char *pHeaders[] = { "-l", sLink, "-t", "10", "-x", "X-Time: 12:30:45; X-Url : http://a/b?c=d ", NULL };
    CHECK(xhttp_run(pHeaders, &nStatus) == 0 && nStatus == XSTDNON, "A request with custom headers");
    CHECK(strstr(g_sOut, "\r\nX-Time: 12:30:45\r\n") != NULL, "A value keeps its colons");
    CHECK(strstr(g_sOut, "\r\nX-Url: http://a/b?c=d\r\n") != NULL, "And loses only the spaces around it");

    const char *pBad[] = { "NoColon", "X-Empty:", ": value", "X-A: 1; broken", NULL };
    for (size_t i = 0; pBad[i] != NULL; i++)
    {
        const char *pArgs[] = { "-l", sLink, "-t", "10", "-x", pBad[i], NULL };
        int nRequests = g_server.nRequests;
        CHECK(xhttp_run(pArgs, &nStatus) == 0 && nStatus == XSTDERR && g_server.nRequests == nRequests,
            "A header with no name or no value is refused, and nothing is sent");
    }

    /* Methods: one the library does not know is refused, a known one is sent with its body */
    const char *pUnknown[] = { "-l", sLink, "-m", "FOO", NULL };
    CHECK(xhttp_run(pUnknown, &nStatus) == 0 && nStatus == 99, "An unknown method is refused with the arguments");

    char sContent[256];
    path_to(sContent, sizeof(sContent), "body.bin");
    CHECK(XPath_Write(sContent, g_binary, sizeof(g_binary), "cwt") == (int)sizeof(g_binary), "A binary body");

    const char *pPost[] = { "-l", sLink, "-m", "POST", "-c", sContent, "-t", "10", NULL };
    CHECK(xhttp_run(pPost, &nStatus) == 0 && nStatus == XSTDNON && !strncmp(g_sOut, "POST /echo ", 11),
        "A known one is sent");
    CHECK(g_nOut > sizeof(g_binary) && !memcmp(g_sOut + g_nOut - sizeof(g_binary), g_binary, sizeof(g_binary)),
        "With its body");

    /* Verbose, the request shown is all of it. The answer is empty, so the body can only be in what was shown. */
    link_to(sLink, sizeof(sLink), "/empty");
    const char *pVerbose[] = { "-l", sLink, "-m", "POST", "-c", sContent, "-t", "10", "-v", NULL };
    CHECK(xhttp_run(pVerbose, &nStatus) == 0 && nStatus == XSTDNON, "A verbose request");
    CHECK(memmem(g_sOut, g_nOut, "\r\n\r\nbin\0\1\2\0\0ary\xff\n", 17) != NULL,
        "Shows its body whole, past the NUL bytes in it");

    case_end();
    return 0;
}

XTEST_MAIN(
    XTEST_CASE(download_complete),
    XTEST_CASE(download_finish),
    XTEST_CASE(download_partial),
    XTEST_CASE(download_status),
    XTEST_CASE(redirects),
    XTEST_CASE(print_body),
    XTEST_CASE(request_args)
)
