/*!
 *  @file libxutils/examples/basic-server.c
 *
 *  This source is part of the "libxutils" project
 *  2015-2026  Sandro Kalatozishvili (s.kalatoz@gmail.com)
 *
 * @brief Example of a simple blocking TCP server that serves one client
 * at a time with a line based protocol. Every line is a command: "echo"
 * sends its text back, "time" sends the time of the server, "stats" what
 * the server has done so far and "quit" closes the connection. A client
 * that stays silent for too long is disconnected, Ctrl+C stops the server.
 *
 * Usage: basic-server [address] [port]
 * Try it with: nc 127.0.0.1 6969
 */

#include "xstd.h"
#include "sock.h"
#include "sig.h"
#include "log.h"
#include "str.h"
#include "xtime.h"

#define XSERVER_ADDR        "127.0.0.1"
#define XSERVER_PORT        6969
#define XSERVER_IDLE_SEC    30

typedef struct {
    uint32_t nClients;
    uint32_t nCommands;
} xserver_stats_t;

typedef struct {
    xserver_stats_t *pStats;
    char sAddr[XSOCK_ADDR_MAX];
    char sInput[XSTR_MIN];
    size_t nUsed;
    xsock_t sock;
    xbool_t bQuit;
    int nIdle;
} xclient_t;

static int g_nInterrupted = 0;

static void signal_callback(int nSig)
{
    (void)nSig;
    g_nInterrupted = 1;
}

static void log_status(const xsock_t *pSock, const char *pWhat)
{
    char sStatus[XSTR_MIN];
    XSock_DescribeStatus(pSock, sStatus, sizeof(sStatus));
    xloge("%s: %s", pWhat, sStatus);
}

/* A read that runs into the timeout fails with EAGAIN, or WSAETIMEDOUT on Windows */
static xbool_t read_timed_out(const xsock_t *pSock)
{
    int nError = XSock_GetSysError(pSock);
#ifdef _WIN32
    return nError == WSAETIMEDOUT;
#else
    return nError == EAGAIN || nError == EWOULDBLOCK;
#endif
}

static void reply(xclient_t *pClient, const char *pFmt, ...)
{
    char sLine[XSTR_MIN];
    int nLength = 0;

    /* One byte is kept for the new line */
    XSTRNCPYFMT(sLine, sizeof(sLine) - 1, pFmt, &nLength);
    if (nLength < 0) return;
    sLine[nLength++] = '\n';

    /* XSock_Send() may send a part of the data, XSock_SendChunk() sends all of it or fails */
    if (XSock_SendChunk(&pClient->sock, sLine, (size_t)nLength) <= 0)
    {
        log_status(&pClient->sock, "Failed to reply");
        pClient->bQuit = XTRUE;
    }
}

static void handle_command(xclient_t *pClient, const char *pLine)
{
    xserver_stats_t *pStats = pClient->pStats;
    if (!xstrused(pLine)) return;

    pStats->nCommands++;
    xlogi("%s: %s", pClient->sAddr, pLine);

    /* xstrncmp() and xstrcmp() return XTRUE when the strings are equal */
    if (xstrncmp(pLine, "echo ", 5))
    {
        reply(pClient, "%s", &pLine[5]);
    }
    else if (xstrcmp(pLine, "time"))
    {
        char sTime[XTIME_MAX];
        XTime_GetStr(sTime, sizeof(sTime), XTIME_STR_ISO8601);
        reply(pClient, "%s", sTime);
    }
    else if (xstrcmp(pLine, "stats"))
    {
        reply(pClient, "Clients: %u, commands: %u", pStats->nClients, pStats->nCommands);
    }
    else if (xstrcmp(pLine, "quit"))
    {
        reply(pClient, "Bye");
        pClient->bQuit = XTRUE;
    }
    else
    {
        reply(pClient, "Unknown command: %s", pLine);
    }
}

/* TCP is a stream: one read can bring a part of a line or several lines at once */
static void handle_input(xclient_t *pClient)
{
    char *pLine = pClient->sInput;
    char *pEnd = memchr(pLine, '\n', pClient->nUsed);

    while (pEnd != NULL && !pClient->bQuit)
    {
        /* Terminals end the lines with "\r\n" */
        *pEnd = XSTR_NUL;
        if (pEnd > pLine && pEnd[-1] == '\r') pEnd[-1] = XSTR_NUL;

        handle_command(pClient, pLine);
        pLine = pEnd + 1;

        size_t nLeft = pClient->nUsed - (size_t)(pLine - pClient->sInput);
        pEnd = memchr(pLine, '\n', nLeft);
    }

    /* The beginning of the next line waits for the rest of it */
    size_t nLeft = pClient->nUsed - (size_t)(pLine - pClient->sInput);
    memmove(pClient->sInput, pLine, nLeft);
    pClient->nUsed = nLeft;

    if (pClient->nUsed == sizeof(pClient->sInput))
    {
        reply(pClient, "The line is too long, at most %zu characters", sizeof(pClient->sInput) - 1);
        pClient->nUsed = 0;
    }
}

static void serve_client(xclient_t *pClient)
{
    reply(pClient, "Hello %s, the commands are: echo <text>, time, stats, quit", pClient->sAddr);

    while (!pClient->bQuit && !g_nInterrupted)
    {
        char *pFree = &pClient->sInput[pClient->nUsed];
        size_t nFree = sizeof(pClient->sInput) - pClient->nUsed;

        int nRead = XSock_Read(&pClient->sock, pFree, nFree);
        if (nRead > 0)
        {
            pClient->nUsed += (size_t)nRead;
            pClient->nIdle = 0;
            handle_input(pClient);
            continue;
        }

        /* The read gives up every second, so the loop can see Ctrl+C */
        if (nRead < 0 && read_timed_out(&pClient->sock))
        {
            if (++pClient->nIdle < XSERVER_IDLE_SEC) continue;
            reply(pClient, "Silent for %d seconds, bye", XSERVER_IDLE_SEC);
            break;
        }

        /* 0 is the end of the stream: the client has closed the connection */
        if (nRead < 0) log_status(&pClient->sock, "Failed to read");
        break;
    }
}

int main(int argc, char *argv[])
{
    xlog_defaults();
    xlog_timing(XLOG_TIME);
    xlog_enable(XLOG_INFO);

    const char *pAddr = argc > 1 ? argv[1] : XSERVER_ADDR;
    int nPort = argc > 2 ? atoi(argv[2]) : XSERVER_PORT;

    if (nPort <= 0 || nPort > UINT16_MAX)
    {
        xlog("Usage: %s [address] [port]", argv[0]);
        return XSTDERR;
    }

    int nSignals[2] = { SIGTERM, SIGINT };
    XSig_Register(nSignals, 2, signal_callback);

    /* XSOCK_REUSEADDR lets the server listen again right after it was stopped */
    xsock_t listener;
    if (XSock_Create(&listener, XSOCK_TCP_SERVER | XSOCK_REUSEADDR, pAddr, (uint16_t)nPort) == XSOCK_INVALID)
    {
        log_status(&listener, "Failed to listen");
        return XSTDERR;
    }

    /* Accept gives up every second too, a status of XSOCK_WANT_READ tells that nobody came */
    XSock_TimeOutR(&listener, 1, 0);
    xlogi("Listening on %s:%d", pAddr, nPort);

    xserver_stats_t stats = { 0, 0 };
    int nStatus = 0;

    while (!g_nInterrupted)
    {
        xclient_t client;
        if (XSock_Accept(&listener, &client.sock) == XSOCK_INVALID)
        {
            if (listener.eStatus == XSOCK_WANT_READ) continue;
            log_status(&listener, "Failed to accept");
            nStatus = XSTDERR;
            break;
        }

        /* The client inherits nothing from the listener: own timeout, and a failed
           read leaves it open, so a timeout does not end the connection */
        client.sock.nFlags |= XSOCK_KEEPOPEN;
        XSock_TimeOutR(&client.sock, 1, 0);

        char sIP[XSOCK_ADDR_MAX];
        XSock_IPAddr(&client.sock, sIP, sizeof(sIP));
        xstrncpyf(client.sAddr, sizeof(client.sAddr), "%s:%u", sIP, ntohs(client.sock.sockAddr.inAddr.sin_port));

        client.pStats = &stats;
        client.bQuit = XFALSE;
        client.nUsed = 0;
        client.nIdle = 0;

        stats.nClients++;
        xlogi("%s connected", client.sAddr);

        serve_client(&client);
        XSock_Close(&client.sock);
        xlogi("%s disconnected", client.sAddr);
    }

    XSock_Close(&listener);
    xlogi("Stopped after %u clients and %u commands", stats.nClients, stats.nCommands);
    return nStatus;
}
