#include <stdio.h>
#include <string.h>

#include "sock.h"
#include "xtime.h"

#include "test.h"

/*
 * SO_RCVTIMEO / SO_SNDTIMEO take a DWORD of milliseconds on Winsock and a
 * struct timeval on POSIX. Handing Windows the struct makes it read tv_sec as
 * milliseconds, so a 20 second timeout becomes 20 ms and every recv fails
 * immediately - which is exactly how `dgcli login` broke on Windows while
 * working perfectly on Linux.
 *
 * The assertion is on observable behaviour rather than the option encoding,
 * so it is meaningful on whichever platform it runs.
 */
static int test_receive_timeout_is_honoured(void)
{
    XSOCKET pair[2] = {XSOCK_INVALID, XSOCK_INVALID};
    CHECK(XSock_CreatePair(pair) == XSTDOK, "create socket pair");

    xsock_t reader;
    CHECK(XSock_Init(&reader, XSOCK_TCP_PEER, pair[0]) != XSOCK_ERROR, "wrap the read end");

    CHECK(XSock_TimeOutR(&reader, 2, 0) != XSOCK_INVALID, "set a two second receive timeout");

    /* Nothing is ever written, so the read can only end on the timeout */
    uint8_t sBuffer[16];
    uint64_t nStartMs = XTime_GetMs();
    int nRead = XSock_Read(&reader, sBuffer, sizeof(sBuffer));
    uint64_t nElapsedMs = XTime_GetMs() - nStartMs;

    CHECK(nRead <= 0, "a read with no data must not succeed");

    /*
     * Before the fix this returned in about 2 ms on Windows. The window is
     * wide on both sides so the test cannot flake on a loaded machine.
     */
    CHECK(nElapsedMs >= 1500, "a two second timeout must not expire in milliseconds");
    CHECK(nElapsedMs < 10000, "a two second timeout must not block far longer");

    XSock_Close(&reader);
    xclosesock(pair[1]);
    return 0;
}

/* A zero timeout means "no timeout"; sub-millisecond requests must not round
 * down into it and block forever. */
static int test_timeout_arguments(void)
{
    XSOCKET pair[2] = {XSOCK_INVALID, XSOCK_INVALID};
    CHECK(XSock_CreatePair(pair) == XSTDOK, "create socket pair");

    xsock_t sock;
    CHECK(XSock_Init(&sock, XSOCK_TCP_PEER, pair[0]) != XSOCK_ERROR, "wrap the socket");

    CHECK(XSock_TimeOutS(&sock, 1, 0) != XSOCK_INVALID, "set a send timeout");
    CHECK(XSock_TimeOutR(&sock, 0, 500) != XSOCK_INVALID, "set a sub-millisecond receive timeout");

    uint8_t sBuffer[8];
    uint64_t nStartMs = XTime_GetMs();
    XSock_Read(&sock, sBuffer, sizeof(sBuffer));
    uint64_t nElapsedMs = XTime_GetMs() - nStartMs;

    CHECK(nElapsedMs < 5000, "a sub-millisecond timeout must not become no timeout");

    XSock_Close(&sock);
    xclosesock(pair[1]);
    return 0;
}

static int XTest_boundaries(void)
{
    xlog_setfl(XLOG_NONE);

    int nStatus = test_receive_timeout_is_honoured();
    if (nStatus) return nStatus;

    nStatus = test_timeout_arguments();
    if (nStatus) return nStatus;

    puts("sock_timeout_regression: OK");
    return 0;
}

#ifndef _WIN32
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <unistd.h>

#define TIMED_FILL 4

typedef struct {
    int nListen;
    int nFill[TIMED_FILL];
    uint16_t nPort;
} timed_listener_t;

static void timed_listener_close(timed_listener_t *pListener)
{
    for (int i = 0; i < TIMED_FILL; i++) if (pListener->nFill[i] >= 0) close(pListener->nFill[i]);
    if (pListener->nListen >= 0) close(pListener->nListen);
}

/* A non-blocking connect to the port, left to run. Returns the descriptor or -1. */
static int timed_start_connect(uint16_t nPort)
{
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(nPort);

    int nFd = socket(AF_INET, SOCK_STREAM, 0);
    if (nFd < 0) return -1;

    if (fcntl(nFd, F_SETFL, fcntl(nFd, F_GETFL) | O_NONBLOCK) < 0 ||
        (connect(nFd, (struct sockaddr*)&addr, sizeof(addr)) < 0 && errno != EINPROGRESS))
    {
        close(nFd);
        return -1;
    }

    return nFd;
}

/* A listener nobody accepts from, its queue filled up. Linux drops the SYNs it has no room for, so a connect to it is
   never answered and can only end on a timeout. bAnswered says whether this platform answers it anyway. */
static int timed_listener_open(timed_listener_t *pListener, int nBacklog, xbool_t *pAnswered)
{
    pListener->nListen = socket(AF_INET, SOCK_STREAM, 0);
    for (int i = 0; i < TIMED_FILL; i++) pListener->nFill[i] = -1;
    pListener->nPort = 0;
    *pAnswered = XTRUE;
    if (pListener->nListen < 0) return XSTDERR;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    socklen_t nLen = sizeof(addr);
    if (bind(pListener->nListen, (struct sockaddr*)&addr, nLen) < 0 ||
        getsockname(pListener->nListen, (struct sockaddr*)&addr, &nLen) < 0 ||
        listen(pListener->nListen, nBacklog) < 0) return XSTDERR;

    pListener->nPort = ntohs(addr.sin_port);
    if (nBacklog > 0) return XSTDOK;

    for (int i = 0; i < TIMED_FILL; i++) pListener->nFill[i] = timed_start_connect(pListener->nPort);
    xusleep(100000);

    /* One more, waited for: whether it gets through is what this platform does with a full queue */
    int nProbe = timed_start_connect(pListener->nPort);
    if (nProbe < 0) return XSTDERR;

    struct pollfd pollFd;
    pollFd.fd = nProbe;
    pollFd.events = POLLOUT;
    pollFd.revents = 0;
    *pAnswered = poll(&pollFd, 1, 300) != 0;
    close(nProbe);
    return XSTDOK;
}

static int timed_option_ms(int nFd, int nOption)
{
    struct timeval tv;
    socklen_t nLen = sizeof(tv);
    memset(&tv, 0, sizeof(tv));
    if (getsockopt(nFd, SOL_SOCKET, nOption, &tv, &nLen) < 0) return -1;
    return (int)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
}

static void timed_info(xsock_info_t *pInfo, uint16_t nPort)
{
    XSock_InitInfo(pInfo);
    xstrncpy(pInfo->sAddr, sizeof(pInfo->sAddr), "127.0.0.1");
    pInfo->nPort = nPort;
}

static int XTest_timed_connect_posix(void)
{
    alarm(60);

    timed_listener_t listener;
    xbool_t bAnswered = XTRUE;
    xsock_info_t info;
    xsock_t sock;

    /* Opened, the socket is the blocking one it was asked for, with both timeouts already in place */
    CHECK(timed_listener_open(&listener, 8, &bAnswered) == XSTDOK, "A listener is set up");
    timed_info(&info, listener.nPort);
    CHECK(XSock_OpenTimed(&sock, XSOCK_TCP_CLIENT, &info, 1) != XSOCK_INVALID, "A timed open connects");
    CHECK(!XSock_IsNB(&sock), "It is left blocking");
    int nRecvMs = timed_option_ms(sock.nFD, SO_RCVTIMEO);
    int nSendMs = timed_option_ms(sock.nFD, SO_SNDTIMEO);
    CHECK(nRecvMs >= 900 && nRecvMs <= 1100, "Its receive timeout is the one asked for");
    CHECK(nSendMs >= 900 && nSendMs <= 1100, "So is its send timeout");
    XSock_Close(&sock);

    /* No timeout, and a socket that does not block, open exactly as XSock_Open() does */
    CHECK(XSock_OpenTimed(&sock, XSOCK_TCP_CLIENT, &info, 0) != XSOCK_INVALID, "An open with no timeout connects");
    CHECK(timed_option_ms(sock.nFD, SO_RCVTIMEO) == 0 && timed_option_ms(sock.nFD, SO_SNDTIMEO) == 0,
        "And sets no timeout");
    XSock_Close(&sock);

    CHECK(XSock_OpenTimed(&sock, XSOCK_TCP_CLIENT | XSOCK_NB, &info, 1) != XSOCK_INVALID, "A non-blocking open connects");
    CHECK(XSock_IsNB(&sock) && timed_option_ms(sock.nFD, SO_RCVTIMEO) == 0, "And keeps to XSock_Open()");
    XSock_Close(&sock);
    timed_listener_close(&listener);

    /* A refusal is still reported at once, with its own error */
    CHECK(timed_listener_open(&listener, 8, &bAnswered) == XSTDOK, "Another listener is set up");
    timed_info(&info, listener.nPort);
    timed_listener_close(&listener);
    uint64_t nStartMs = XTime_GetMonoMs();
    CHECK(XSock_OpenTimed(&sock, XSOCK_TCP_CLIENT, &info, 5) == XSOCK_INVALID, "A closed port is not connected to");
    CHECK(XTime_GetMonoMs() - nStartMs < 4000, "Without waiting the timeout out");
    CHECK(XSock_Status(&sock) == XSOCK_ERR_CONNECT && XSock_GetSysError(&sock) == ECONNREFUSED,
        "With the refusal as its error");

    /* A connect nobody answers ends on the timeout. Without one it waited out the kernel's SYN retries. */
    CHECK(timed_listener_open(&listener, 0, &bAnswered) == XSTDOK, "A full listener is set up");
    if (bAnswered)
    {
        timed_listener_close(&listener);
        printf("A full accept queue is answered here, skipping\n");
        return 77;
    }

    timed_info(&info, listener.nPort);
    nStartMs = XTime_GetMonoMs();
    CHECK(XSock_OpenTimed(&sock, XSOCK_TCP_CLIENT, &info, 1) == XSOCK_INVALID, "An unanswered connect fails");
    uint64_t nElapsedMs = XTime_GetMonoMs() - nStartMs;
    CHECK(nElapsedMs >= 900, "Once its timeout ran out");
    CHECK(nElapsedMs < 10000, "And not much later");
    CHECK(XSock_Status(&sock) == XSOCK_ERR_CONNECT && XSock_GetSysError(&sock) == ETIMEDOUT,
        "As a connect that timed out");
    CHECK(XSock_GetFD(&sock) == XSOCK_INVALID, "Leaving nothing open");

    timed_listener_close(&listener);
    alarm(0);
    return 0;
}

#ifdef XSOCK_USE_SSL
/* A server that accepts and then never says a word of TLS. The receive timeout used to be set only once the open had
   returned, so the handshake waited for the server's hello for good. */
static int XTest_timed_handshake_posix(void)
{
    alarm(60);
    XSock_InitSSL();

    timed_listener_t listener;
    xbool_t bAnswered = XTRUE;
    CHECK(timed_listener_open(&listener, 8, &bAnswered) == XSTDOK, "A silent listener is set up");

    xsock_info_t info;
    timed_info(&info, listener.nPort);

    xsock_t sock;
    uint64_t nStartMs = XTime_GetMonoMs();
    CHECK(XSock_OpenTimed(&sock, XSOCK_TCP_CLIENT | XSOCK_SSL, &info, 1) == XSOCK_INVALID, "A silent handshake fails");
    uint64_t nElapsedMs = XTime_GetMonoMs() - nStartMs;
    CHECK(nElapsedMs >= 900, "Once its timeout ran out");
    CHECK(nElapsedMs < 10000, "And not much later");
    CHECK(XSock_Status(&sock) == XSOCK_ERR_SSLCNT, "As a failed handshake");
    CHECK(XSock_GetSysError(&sock) == EAGAIN || XSock_GetSysError(&sock) == EWOULDBLOCK,
        "That says the socket timed out, not nothing");
    CHECK(XSock_GetFD(&sock) == XSOCK_INVALID, "Leaving nothing open");

    timed_listener_close(&listener);
    alarm(0);
    return 0;
}
#endif
#endif

static int XTest_timed_connect(void)
{
    xlog_setfl(XLOG_NONE);
#ifndef _WIN32
    return XTest_timed_connect_posix();
#else
    printf("The full accept queue this needs is a POSIX one, skipping\n");
    return 77;
#endif
}

static int XTest_timed_handshake(void)
{
    xlog_setfl(XLOG_NONE);
#if !defined(_WIN32) && defined(XSOCK_USE_SSL)
    return XTest_timed_handshake_posix();
#else
    printf("No TLS or no POSIX sockets, skipping\n");
    return 77;
#endif
}

XTEST_MAIN(XTEST_CASE(boundaries), XTEST_CASE(timed_connect), XTEST_CASE(timed_handshake))
