/*!
 *  @file libxutils/src/sys/exec.c
 *
 *  This source is part of the "libxutils" project
 *  2015-2026  Sandro Kalatozishvili (s.kalatoz@gmail.com)
 *
 * @brief Child process execution with standard streams an event loop can poll.
 */

/* pipe2() */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "exec.h"
#include "xtime.h"
#include "sync.h"

#ifndef _WIN32
#include <poll.h>
extern char **environ;
#endif

#ifdef __linux__
#include <sys/prctl.h>
#endif

#ifndef SIGKILL
#define SIGKILL                 9
#endif

#define XEXEC_STREAMS           3
#define XEXEC_IO_CHUNK          65536
#define XEXEC_FD_CLOSE_MAX      65536
#define XEXEC_WAIT_STEP_USEC    2000
#define XEXEC_RUN_STEP_MS       20
#define XEXEC_RUN_GRACE_MS      250
#define XEXEC_RUN_KILL_WAIT_MS  5000

void XExec_Init(xexec_t *pExec)
{
    XCHECK_VOID_NL((pExec != NULL));
    memset(pExec, 0, sizeof(*pExec));

    pExec->nStdIn = XSOCK_INVALID;
    pExec->nStdOut = XSOCK_INVALID;
    pExec->nStdErr = XSOCK_INVALID;
    pExec->nPid = XSTDERR;

#ifdef _WIN32
    for (int i = 0; i < XEXEC_STREAMS; i++)
    {
        pExec->hPumps[i] = NULL;
        pExec->nBridges[i] = XSOCK_INVALID;
    }

    pExec->hProcess = NULL;
    pExec->hJob = NULL;
#endif
}

static void XExec_CloseSock(XSOCKET *pSock)
{
    if (*pSock == XSOCK_INVALID) return;
#ifdef _WIN32
    closesocket(*pSock);
#else
    close(*pSock);
#endif
    *pSock = XSOCK_INVALID;
}

void XExec_CloseStdIn(xexec_t *pExec)
{
    XCHECK_VOID_NL((pExec != NULL));
    XExec_CloseSock(&pExec->nStdIn);
}

#ifndef _WIN32

/* What the child installs as its descriptors 0, 1 and 2, how far it closes the rest, and who started it. */
typedef struct XExecChild {
    int nFds[XEXEC_STREAMS];
    int nMaxFd;
    pid_t nParent;
} xexec_child_t;

/* A coverage build (XUTILS_TEST_COVERAGE) writes out what the child ran before it leaves: _exit() skips the
   handler that would, and otherwise none of the child's own lines would count. Nothing else defines it. */
#ifdef XEXEC_GCOV_DUMP
void __gcov_dump(void);
#define XEXEC_CHILD_EXIT(code) do { __gcov_dump(); _exit(code); } while (XFALSE)
#else
#define XEXEC_CHILD_EXIT(code) _exit(code)
#endif

/* Child side only: one write and _exit, the reason going to the child's own stderr. */
#define XEXEC_CHILD_FAIL(msg, code)                                         \
    do {                                                                    \
        ssize_t nXExecDone = write(STDERR_FILENO, msg, sizeof(msg) - 1);    \
        (void)nXExecDone;                                                   \
        XEXEC_CHILD_EXIT(code);                                             \
    }                                                                       \
    while (XFALSE)

static XSTATUS XExec_Pipe(int nFds[2])
{
#ifdef __linux__
    if (pipe2(nFds, O_CLOEXEC) == 0) return XSTDOK;
    if (errno != ENOSYS) return XSTDERR;
#endif

    if (pipe(nFds) != 0) return XSTDERR;
    (void)fcntl(nFds[0], F_SETFD, FD_CLOEXEC);
    (void)fcntl(nFds[1], F_SETFD, FD_CLOEXEC);
    return XSTDOK;
}

static XSTATUS XExec_SetNonBlock(int nFd)
{
    int nFlags = fcntl(nFd, F_GETFL);
    if (nFlags < 0) return XSTDERR;
    return fcntl(nFd, F_SETFL, nFlags | O_NONBLOCK) == 0 ? XSTDOK : XSTDERR;
}

static void XExec_CloseFd(int *pFd)
{
    if (*pFd < 0) return;
    close(*pFd);
    *pFd = XSTDERR;
}

/* Everything from 3 up: the caller's sockets and files are not the child's business, and a descriptor
   without close-on-exec - one opened by a library, or before the caller set the flag - would otherwise
   reach the program. close_range does it in one call where the kernel has it (Linux 5.9); an older kernel
   answers ENOSYS and gets the loop. */
static void XExec_CloseFrom3(int nMaxFd)
{
#if defined(__linux__) && defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34))
    if (close_range(3U, ~0U, 0) == 0) return;
#elif defined(__linux__) && defined(SYS_close_range)
    if (syscall(SYS_close_range, 3U, ~0U, 0U) == 0) return;
#endif

    for (int nFd = 3; nFd < nMaxFd; nFd++) close(nFd);
}

/* Never returns: every way out is an exec or an _exit. */
static void XExec_Child(const xexec_opts_t *pOpts, xexec_child_t *pChild, char *const *ppArgv, char *const *ppEnv)
{
    /* A source that is already 0, 1 or 2 would be overwritten by the dup2 of an earlier stream
       before its own turn came, so every such source is moved out of the way first. */
    for (int i = 0; i < XEXEC_STREAMS; i++)
    {
        int nOld = pChild->nFds[i];
        if (nOld >= XEXEC_STREAMS) continue;

        int nNew = fcntl(nOld, F_DUPFD, XEXEC_STREAMS);
        if (nNew < 0) XEXEC_CHILD_EXIT(XEXEC_EXIT_NOTFOUND);

        for (int j = 0; j < XEXEC_STREAMS; j++)
            if (pChild->nFds[j] == nOld) pChild->nFds[j] = nNew;
    }

    for (int i = 0; i < XEXEC_STREAMS; i++)
        if (dup2(pChild->nFds[i], i) < 0) XEXEC_CHILD_EXIT(XEXEC_EXIT_NOTFOUND);

    if ((pOpts->nFlags & XEXEC_GROUP) && setsid() < 0)
        XEXEC_CHILD_FAIL("cannot start a new process group\n", XEXEC_EXIT_NOTFOUND);

    /* An ignored signal stays ignored across exec, and so does a blocked one: a caller that ignores
       SIGPIPE would hand a child a pipeline that no longer stops at a closed reader. Everything is still
       blocked from the fork; a signal that arrived meanwhile is taken, by default, once unblocked here. */
    struct sigaction defAction;
    memset(&defAction, 0, sizeof(defAction));
    defAction.sa_handler = SIG_DFL;
    sigemptyset(&defAction.sa_mask);

    for (int nSig = 1; nSig < NSIG; nSig++)
    {
        if (nSig == SIGKILL || nSig == SIGSTOP) continue;
        (void)sigaction(nSig, &defAction, NULL);
    }

    sigset_t emptySet;
    sigemptyset(&emptySet);
    (void)sigprocmask(SIG_SETMASK, &emptySet, NULL);

    /* Order matters: supplementary groups, then gid, then uid - after the uid nothing else can change. */
    if (pOpts->bSetIdentity)
    {
#ifdef __APPLE__
        int nGroupCount = (int)pOpts->nGroups;
#else
        size_t nGroupCount = pOpts->nGroups;
#endif

        if (setgroups(nGroupCount, pOpts->pGroups) != 0 || setgid(pOpts->nGid) != 0 || setuid(pOpts->nUid) != 0)
            XEXEC_CHILD_FAIL("cannot switch to the requested user\n", XEXEC_EXIT_NOTFOUND);
    }

#ifdef __linux__
    /* After the identity change, which clears it. A caller that is already gone leaves nobody to stop the
       program, so it does not start. */
    if ((pOpts->nFlags & XEXEC_DIE_WITH_PARENT) &&
        (prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 || getppid() != pChild->nParent))
        XEXEC_CHILD_FAIL("cannot tie the program to its parent\n", XEXEC_EXIT_NOTFOUND);
#endif

    /* After the identity change, so the directory is entered with the permissions of the account that runs */
    if (xstrused(pOpts->pWorkDir) && chdir(pOpts->pWorkDir) != 0 &&
        (!xstrused(pOpts->pWorkDirFallback) || chdir(pOpts->pWorkDirFallback) != 0))
        XEXEC_CHILD_FAIL("cannot change to the working directory\n", XEXEC_EXIT_NOTFOUND);

    XExec_CloseFrom3(pChild->nMaxFd);
    execve(pOpts->pPath, ppArgv, ppEnv);

    if (errno == ENOENT || errno == ENOTDIR)
        XEXEC_CHILD_FAIL("cannot find the program\n", XEXEC_EXIT_NOTFOUND);

    XEXEC_CHILD_FAIL("cannot execute the program\n", XEXEC_EXIT_NOEXEC);
}

XSTATUS XExec_Spawn(xexec_t *pExec, const xexec_opts_t *pOpts)
{
    XCHECK_NL((pExec != NULL && pOpts != NULL), XSTDINV);
    XCHECK_NL((xstrused(pOpts->pPath)), XSTDINV);
    XCHECK_NL((!pOpts->bSetIdentity || pOpts->nGroups == 0 || pOpts->pGroups != NULL), XSTDINV);
    XExec_Init(pExec);

    uint32_t nFlags = pOpts->nFlags;
    xbool_t bPipe[XEXEC_STREAMS];
    bPipe[0] = (nFlags & XEXEC_STDIN) ? XTRUE : XFALSE;
    bPipe[1] = (nFlags & XEXEC_STDOUT) ? XTRUE : XFALSE;
    bPipe[2] = ((nFlags & XEXEC_STDERR) && !(nFlags & XEXEC_MERGE_STDERR)) ? XTRUE : XFALSE;

    int nPipes[XEXEC_STREAMS][2] = { { -1, -1 }, { -1, -1 }, { -1, -1 } };
    int nNull = XSTDERR;
    xbool_t bOk = XFALSE;
    int nError = 0;

    do
    {
        nNull = open("/dev/null", O_RDWR | O_CLOEXEC);
        if (nNull < 0) break;

        int i = 0;
        for (i = 0; i < XEXEC_STREAMS; i++)
            if (bPipe[i] && XExec_Pipe(nPipes[i]) != XSTDOK) break;

        if (i < XEXEC_STREAMS) break;
        bOk = XTRUE;
    }
    while (XFALSE);

    if (!bOk)
    {
        nError = errno;
        for (int i = 0; i < XEXEC_STREAMS; i++)
        {
            XExec_CloseFd(&nPipes[i][0]);
            XExec_CloseFd(&nPipes[i][1]);
        }

        XExec_CloseFd(&nNull);
        errno = nError;
        return XSTDERR;
    }

    /* The child's end of each stream: the read end of stdin, the write end of the outputs */
    xexec_child_t child;
    child.nFds[0] = bPipe[0] ? nPipes[0][0] : nNull;
    child.nFds[1] = bPipe[1] ? nPipes[1][1] : nNull;
    child.nFds[2] = bPipe[2] ? nPipes[2][1] : nNull;

    if ((nFlags & XEXEC_MERGE_STDERR) && (nFlags & XEXEC_STDERR))
        child.nFds[2] = child.nFds[1];

    /* Only the descriptor-by-descriptor fallback uses it: bounded, since a limit of a million would make
       the child spend its start-up closing descriptors that were never open */
    long nOpenMax = sysconf(_SC_OPEN_MAX);
    if (nOpenMax < 0) nOpenMax = 1024;
    if (nOpenMax > XEXEC_FD_CLOSE_MAX) nOpenMax = XEXEC_FD_CLOSE_MAX;
    child.nMaxFd = (int)nOpenMax;

    /* Prepared before fork(), so the child allocates nothing */
    char *aDefArgv[2] = { (char*)pOpts->pPath, NULL };
    char *const *ppArgv = pOpts->ppArgv != NULL ? pOpts->ppArgv : aDefArgv;
    char *const *ppEnv = pOpts->ppEnv != NULL ? pOpts->ppEnv : environ;
    child.nParent = getpid();

    /* Every signal held back across the fork: until the child has put its dispositions back to default, one
       sent to it would run a copy of the caller's handler in a process that is only half set up. The caller
       has its own mask back at once; the child unblocks only after the reset. */
    sigset_t allSignals, savedMask;
    sigfillset(&allSignals);
    pthread_sigmask(SIG_BLOCK, &allSignals, &savedMask);

    pid_t nPid = fork();
    if (nPid == 0) XExec_Child(pOpts, &child, ppArgv, ppEnv);

    nError = errno;
    pthread_sigmask(SIG_SETMASK, &savedMask, NULL);
    XExec_CloseFd(&nPipes[0][0]);
    XExec_CloseFd(&nPipes[1][1]);
    XExec_CloseFd(&nPipes[2][1]);
    XExec_CloseFd(&nNull);

    if (nPid < 0)
    {
        XExec_CloseFd(&nPipes[0][1]);
        XExec_CloseFd(&nPipes[1][0]);
        XExec_CloseFd(&nPipes[2][0]);
        errno = nError;
        return XSTDERR;
    }

    pExec->nPid = nPid;
    pExec->bGroup = (nFlags & XEXEC_GROUP) ? XTRUE : XFALSE;
    pExec->nStdIn = nPipes[0][1];
    pExec->nStdOut = nPipes[1][0];
    pExec->nStdErr = nPipes[2][0];

    if (nFlags & XEXEC_NONBLOCK)
    {
        /* Best effort: a descriptor left blocking is still a working pipe */
        if (pExec->nStdIn >= 0) (void)XExec_SetNonBlock(pExec->nStdIn);
        if (pExec->nStdOut >= 0) (void)XExec_SetNonBlock(pExec->nStdOut);
        if (pExec->nStdErr >= 0) (void)XExec_SetNonBlock(pExec->nStdErr);
    }

    return XSTDOK;
}

static void XExec_SetStatus(xexec_t *pExec, int nStatus)
{
    if (WIFSIGNALED(nStatus))
    {
        pExec->nSignal = WTERMSIG(nStatus);
        pExec->nExitCode = 128 + pExec->nSignal;
    }
    else
    {
        /* Without WUNTRACED or WCONTINUED waitpid reports nothing but these two */
        pExec->nSignal = 0;
        pExec->nExitCode = WEXITSTATUS(nStatus);
    }

    pExec->bExited = XTRUE;
}

XSTATUS XExec_Wait(xexec_t *pExec, int nTimeoutMs)
{
    XCHECK_NL((pExec != NULL), XSTDINV);
    if (pExec->bExited) return XSTDOK;
    XCHECK_NL((pExec->nPid > 0), XSTDERR);

    uint64_t nDeadlineMs = nTimeoutMs > 0 ? XTime_GetMonoMs() + (uint64_t)nTimeoutMs : 0;
    int nOptions = nTimeoutMs == XEXEC_WAIT_FOREVER ? 0 : WNOHANG;

    for (;;)
    {
        int nStatus = 0;
        pid_t nRet = waitpid(pExec->nPid, &nStatus, nOptions);

        if (nRet == pExec->nPid)
        {
            XExec_SetStatus(pExec, nStatus);
            return XSTDOK;
        }

        if (nRet < 0)
        {
            if (errno == EINTR) continue;
            return XSTDERR;
        }

        if (nTimeoutMs <= 0 || XTime_GetMonoMs() >= nDeadlineMs) return XSTDNON;
        xusleep(XEXEC_WAIT_STEP_USEC);
    }
}

XSTATUS XExec_Kill(xexec_t *pExec, int nSignal)
{
    XCHECK_NL((pExec != NULL), XSTDINV);
    if (pExec->bExited) return XSTDNON;
    XCHECK_NL((pExec->nPid > 0), XSTDERR);

    /* Not yet collected, so the id cannot belong to anyone else: a group whose leader has exited
       stays addressable until the leader is waited for. */
    if (pExec->bGroup)
    {
        if (kill(-pExec->nPid, nSignal) == 0) return XSTDOK;
        if (errno != ESRCH) return XSTDERR;

        /* No such group yet: the child has not reached setsid(). It holds every signal blocked until it
           has reset them, so this one is taken, by default, as soon as it can be. */
    }

    if (kill(pExec->nPid, nSignal) == 0) return XSTDOK;
    return errno == ESRCH ? XSTDNON : XSTDERR;
}

void XExec_Close(xexec_t *pExec)
{
    XCHECK_VOID_NL((pExec != NULL));
    XExec_CloseSock(&pExec->nStdIn);
    XExec_CloseSock(&pExec->nStdOut);
    XExec_CloseSock(&pExec->nStdErr);
}

#else /* _WIN32 */

/*
    WSAPoll waits on sockets only, so each piped stream is a socket pair on
    the caller's side and one thread per stream copies between the pair and
    the child's pipe. The pump owns its pipe handle and closes it on the way
    out: closing the write end of stdin is how the child sees end of file.
*/
typedef struct XExecPump {
    HANDLE hPipe;
    XSOCKET nSock;
    xbool_t bToChild;
} xexec_pump_t;

static DWORD WINAPI XExec_PumpThread(LPVOID pArg)
{
    xexec_pump_t pump = *(xexec_pump_t*)pArg;
    char sBuffer[XEXEC_IO_CHUNK];
    free(pArg);

    if (pump.bToChild)
    {
        for (;;)
        {
            int nRecv = recv(pump.nSock, sBuffer, (int)sizeof(sBuffer), 0);
            if (nRecv <= 0) break;

            int nDone = 0;
            while (nDone < nRecv)
            {
                DWORD nWritten = 0;
                if (!WriteFile(pump.hPipe, sBuffer + nDone, (DWORD)(nRecv - nDone), &nWritten, NULL)) break;
                nDone += (int)nWritten;
            }

            if (nDone < nRecv) break;
        }
    }
    else
    {
        DWORD nRead = 0;
        while (ReadFile(pump.hPipe, sBuffer, (DWORD)sizeof(sBuffer), &nRead, NULL) && nRead > 0)
        {
            DWORD nSent = 0;
            while (nSent < nRead)
            {
                int nRet = send(pump.nSock, sBuffer + nSent, (int)(nRead - nSent), 0);
                if (nRet <= 0) break;
                nSent += (DWORD)nRet;
            }

            if (nSent < nRead) break;
        }

        /* The caller reads end of file */
        shutdown(pump.nSock, SD_SEND);
    }

    CloseHandle(pump.hPipe);
    return 0;
}

/* The program and its arguments as one command line, quoted for CommandLineToArgvW
   the rules the C runtime of nearly every program parses its command line by. */
static char* XExec_BuildCmdLine(const char *pPath, char *const *ppArgv)
{
    char *aDefArgv[2] = { (char*)pPath, NULL };
    char *const *ppArgs = ppArgv != NULL ? ppArgv : aDefArgv;
    size_t nSize = 1;

    for (size_t i = 0; ppArgs[i] != NULL; i++) nSize += strlen(ppArgs[i]) * 2 + 3;

    char *pCmdLine = (char*)malloc(nSize);
    XCHECK_NL((pCmdLine != NULL), NULL);
    size_t nPos = 0;

    for (size_t i = 0; ppArgs[i] != NULL; i++)
    {
        const char *pArg = ppArgs[i];
        if (i > 0) pCmdLine[nPos++] = ' ';

        if (*pArg != '\0' && strpbrk(pArg, " \t\n\v\"") == NULL)
        {
            size_t nLen = strlen(pArg);
            memcpy(pCmdLine + nPos, pArg, nLen);
            nPos += nLen;
            continue;
        }

        pCmdLine[nPos++] = '"';

        for (const char *pChar = pArg; ; pChar++)
        {
            size_t nSlashes = 0;
            while (*pChar == '\\') { pChar++; nSlashes++; }

            if (*pChar == '\0')
            {
                for (size_t j = 0; j < nSlashes * 2; j++) pCmdLine[nPos++] = '\\';
                break;
            }

            size_t nEscaped = *pChar == '"' ? nSlashes * 2 + 1 : nSlashes;
            for (size_t j = 0; j < nEscaped; j++) pCmdLine[nPos++] = '\\';
            pCmdLine[nPos++] = *pChar;
        }

        pCmdLine[nPos++] = '"';
    }

    pCmdLine[nPos] = '\0';
    return pCmdLine;
}

static char* XExec_BuildEnvBlock(char *const *ppEnv)
{
    size_t nSize = 2;
    for (size_t i = 0; ppEnv[i] != NULL; i++) nSize += strlen(ppEnv[i]) + 1;

    char *pBlock = (char*)calloc(1, nSize);
    XCHECK_NL((pBlock != NULL), NULL);
    size_t nPos = 0;

    for (size_t i = 0; ppEnv[i] != NULL; i++)
    {
        size_t nLen = strlen(ppEnv[i]);
        memcpy(pBlock + nPos, ppEnv[i], nLen + 1);
        nPos += nLen + 1;
    }

    return pBlock;
}

static void XExec_CloseHandle(HANDLE *pHandle)
{
    if (*pHandle == NULL || *pHandle == INVALID_HANDLE_VALUE) return;
    CloseHandle(*pHandle);
    *pHandle = NULL;
}

/* Starts the pump that bridges hPipe to a new socket pair; the caller keeps aPair[0]. The pipe handle
   belongs to the pump once it runs, and is closed here when it cannot be started. */
static XSTATUS XExec_StartPump(xexec_t *pExec, int nStream, HANDLE hPipe, XSOCKET *pCallerEnd, xbool_t bNonBlock)
{
    XSOCKET aPair[2] = { XSOCK_INVALID, XSOCK_INVALID };
    xexec_pump_t *pPump = NULL;

    do
    {
        if (XSock_CreatePair(aPair) != XSTDOK) break;

        pPump = (xexec_pump_t*)malloc(sizeof(xexec_pump_t));
        if (pPump == NULL) break;

        pPump->hPipe = hPipe;
        pPump->nSock = aPair[1];
        pPump->bToChild = nStream == 0 ? XTRUE : XFALSE;

        HANDLE hThread = CreateThread(NULL, 0, XExec_PumpThread, pPump, 0, NULL);
        if (hThread == NULL) break;

        if (bNonBlock)
        {
            u_long nNonBlock = 1;
            ioctlsocket(aPair[0], FIONBIO, &nNonBlock);
        }

        pExec->hPumps[nStream] = hThread;
        pExec->nBridges[nStream] = aPair[1];
        *pCallerEnd = aPair[0];
        return XSTDOK;
    }
    while (XFALSE);

    free(pPump);
    XExec_CloseSock(&aPair[0]);
    XExec_CloseSock(&aPair[1]);
    CloseHandle(hPipe);
    return XSTDERR;
}

XSTATUS XExec_Spawn(xexec_t *pExec, const xexec_opts_t *pOpts)
{
    XCHECK_NL((pExec != NULL && pOpts != NULL), XSTDINV);
    XCHECK_NL((xstrused(pOpts->pPath) || xstrused(pOpts->pCmdLine)), XSTDINV);
    XExec_Init(pExec);

    uint32_t nFlags = pOpts->nFlags;
    xbool_t bPipe[XEXEC_STREAMS];
    bPipe[0] = (nFlags & XEXEC_STDIN) ? XTRUE : XFALSE;
    bPipe[1] = (nFlags & XEXEC_STDOUT) ? XTRUE : XFALSE;
    bPipe[2] = ((nFlags & XEXEC_STDERR) && !(nFlags & XEXEC_MERGE_STDERR)) ? XTRUE : XFALSE;

    HANDLE hParent[XEXEC_STREAMS] = { NULL, NULL, NULL };
    HANDLE hChild[XEXEC_STREAMS] = { NULL, NULL, NULL };
    HANDLE hNull = NULL;

    LPPROC_THREAD_ATTRIBUTE_LIST pAttrs = NULL;
    PROCESS_INFORMATION procInfo;
    memset(&procInfo, 0, sizeof(procInfo));

    char *pCmdLine = NULL;
    char *pEnvBlock = NULL;
    xbool_t bCreated = XFALSE;

    SECURITY_ATTRIBUTES inherit;
    memset(&inherit, 0, sizeof(inherit));
    inherit.nLength = sizeof(inherit);
    inherit.bInheritHandle = TRUE;

    do
    {
        int i = 0;
        for (i = 0; i < XEXEC_STREAMS; i++)
        {
            if (!bPipe[i]) continue;

            HANDLE hRead = NULL, hWrite = NULL;
            if (!CreatePipe(&hRead, &hWrite, &inherit, 0)) break;

            hParent[i] = i == 0 ? hWrite : hRead;
            hChild[i] = i == 0 ? hRead : hWrite;

            /* Only the child's end is inherited */
            if (!SetHandleInformation(hParent[i], HANDLE_FLAG_INHERIT, 0)) break;
        }

        if (i < XEXEC_STREAMS) break;

        if (!bPipe[0] || !bPipe[1] || !bPipe[2])
        {
            hNull = CreateFileA("NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                &inherit, OPEN_EXISTING, 0, NULL);

            if (hNull == INVALID_HANDLE_VALUE)
            {
                hNull = NULL;
                break;
            }
        }

        HANDLE hStd[XEXEC_STREAMS];
        for (i = 0; i < XEXEC_STREAMS; i++) hStd[i] = bPipe[i] ? hChild[i] : hNull;
        if ((nFlags & XEXEC_MERGE_STDERR) && (nFlags & XEXEC_STDERR)) hStd[2] = hStd[1];

        /* The handle list keeps the child from inheriting every other inheritable handle the caller holds */
        HANDLE hInherit[XEXEC_STREAMS];
        DWORD nInherit = 0;

        for (i = 0; i < XEXEC_STREAMS; i++)
        {
            DWORD j = 0;
            while (j < nInherit && hInherit[j] != hStd[i]) j++;
            if (j == nInherit) hInherit[nInherit++] = hStd[i];
        }

        SIZE_T nAttrSize = 0;
        InitializeProcThreadAttributeList(NULL, 1, 0, &nAttrSize);

        pAttrs = (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(nAttrSize);
        if (pAttrs == NULL) break;

        if (!InitializeProcThreadAttributeList(pAttrs, 1, 0, &nAttrSize))
        {
            free(pAttrs);
            pAttrs = NULL;
            break;
        }

        if (!UpdateProcThreadAttribute(pAttrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            hInherit, nInherit * sizeof(HANDLE), NULL, NULL)) break;

        pCmdLine = xstrused(pOpts->pCmdLine) ? strdup(pOpts->pCmdLine) : XExec_BuildCmdLine(pOpts->pPath, pOpts->ppArgv);
        if (pCmdLine == NULL) break;

        if (pOpts->ppEnv != NULL && (pEnvBlock = XExec_BuildEnvBlock(pOpts->ppEnv)) == NULL) break;

        STARTUPINFOEXA startInfo;
        memset(&startInfo, 0, sizeof(startInfo));
        startInfo.StartupInfo.cb = sizeof(startInfo);
        startInfo.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startInfo.StartupInfo.hStdInput = hStd[0];
        startInfo.StartupInfo.hStdOutput = hStd[1];
        startInfo.StartupInfo.hStdError = hStd[2];
        startInfo.lpAttributeList = pAttrs;

        /* A job is joined before the child runs a single instruction, so nothing it starts escapes it */
        xbool_t bJob = (nFlags & (XEXEC_GROUP | XEXEC_DIE_WITH_PARENT)) ? XTRUE : XFALSE;
        DWORD nCreate = EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW;
        if (nFlags & XEXEC_GROUP) nCreate |= CREATE_NEW_PROCESS_GROUP;
        if (bJob) nCreate |= CREATE_SUSPENDED;

        const char *pAppName = xstrused(pOpts->pPath) ? pOpts->pPath : NULL;
        const char *pWorkDir = xstrused(pOpts->pWorkDir) ? pOpts->pWorkDir : NULL;

        BOOL bStarted = CreateProcessA(pAppName, pCmdLine, NULL, NULL, TRUE, nCreate,
            pEnvBlock, pWorkDir, &startInfo.StartupInfo, &procInfo);

        /* The directory is checked by CreateProcess itself here, so the fallback is a second attempt */
        if (!bStarted && GetLastError() == ERROR_DIRECTORY && xstrused(pOpts->pWorkDirFallback))
        {
            bStarted = CreateProcessA(pAppName, pCmdLine, NULL, NULL, TRUE, nCreate,
                pEnvBlock, pOpts->pWorkDirFallback, &startInfo.StartupInfo, &procInfo);
        }

        if (!bStarted) break;

        bCreated = XTRUE;
    }
    while (XFALSE);

    DWORD nError = GetLastError();
    if (pAttrs != NULL)
    {
        DeleteProcThreadAttributeList(pAttrs);
        free(pAttrs);
    }

    free(pCmdLine);
    free(pEnvBlock);
    XExec_CloseHandle(&hNull);
    for (int i = 0; i < XEXEC_STREAMS; i++) XExec_CloseHandle(&hChild[i]);

    if (!bCreated)
    {
        for (int i = 0; i < XEXEC_STREAMS; i++) XExec_CloseHandle(&hParent[i]);
        SetLastError(nError);
        return XSTDERR;
    }

    if (nFlags & (XEXEC_GROUP | XEXEC_DIE_WITH_PARENT))
    {
        /* A job reaches what the child starts; without one (a job that refuses nesting), only the child.
           Killed when its last handle closes, it also goes when the caller does. */
        pExec->hJob = CreateJobObjectA(NULL, NULL);
        DWORD nJobError = pExec->hJob == NULL ? GetLastError() : ERROR_SUCCESS;

        if (pExec->hJob != NULL && (nFlags & XEXEC_DIE_WITH_PARENT))
        {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
            memset(&limits, 0, sizeof(limits));
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;

            if (!SetInformationJobObject(pExec->hJob, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
            {
                nJobError = GetLastError();
                XExec_CloseHandle(&pExec->hJob);
            }
        }

        if (pExec->hJob != NULL && !AssignProcessToJobObject(pExec->hJob, procInfo.hProcess))
        {
            nJobError = GetLastError();
            XExec_CloseHandle(&pExec->hJob);
        }

        /* A child that could outlive its caller never runs: as on POSIX, the start fails instead.
           It is still suspended, so not one instruction of it has run. */
        if (pExec->hJob == NULL && (nFlags & XEXEC_DIE_WITH_PARENT))
        {
            TerminateProcess(procInfo.hProcess, XEXEC_EXIT_NOEXEC);
            CloseHandle(procInfo.hThread);
            CloseHandle(procInfo.hProcess);
            for (int i = 0; i < XEXEC_STREAMS; i++) XExec_CloseHandle(&hParent[i]);
            SetLastError(nJobError);
            return XSTDERR;
        }

        ResumeThread(procInfo.hThread);
    }

    CloseHandle(procInfo.hThread);
    pExec->hProcess = procInfo.hProcess;
    pExec->nPid = (xpid_t)procInfo.dwProcessId;
    pExec->bGroup = (nFlags & XEXEC_GROUP) ? XTRUE : XFALSE;

    XSOCKET *pEnds[XEXEC_STREAMS] = { &pExec->nStdIn, &pExec->nStdOut, &pExec->nStdErr };
    xbool_t bNonBlock = (nFlags & XEXEC_NONBLOCK) ? XTRUE : XFALSE;
    xbool_t bPumps = XTRUE;

    for (int i = 0; i < XEXEC_STREAMS; i++)
    {
        if (hParent[i] == NULL) continue;
        if (XExec_StartPump(pExec, i, hParent[i], pEnds[i], bNonBlock) != XSTDOK) bPumps = XFALSE;
        hParent[i] = NULL;
    }

    if (!bPumps)
    {
        XExec_Kill(pExec, SIGKILL);
        XExec_Wait(pExec, XEXEC_RUN_KILL_WAIT_MS);
        XExec_Close(pExec);
        return XSTDERR;
    }

    return XSTDOK;
}

XSTATUS XExec_Wait(xexec_t *pExec, int nTimeoutMs)
{
    XCHECK_NL((pExec != NULL), XSTDINV);
    if (pExec->bExited) return XSTDOK;
    XCHECK_NL((pExec->hProcess != NULL), XSTDERR);

    DWORD nWait = WaitForSingleObject(pExec->hProcess, nTimeoutMs < 0 ? INFINITE : (DWORD)nTimeoutMs);
    if (nWait == WAIT_TIMEOUT) return XSTDNON;
    if (nWait != WAIT_OBJECT_0) return XSTDERR;

    DWORD nCode = 0;
    if (!GetExitCodeProcess(pExec->hProcess, &nCode)) return XSTDERR;

    /* Windows has no signals: an exit with the code XExec_Kill chose is reported as that signal */
    pExec->nExitCode = (int)nCode;
    if (pExec->nKillSignal && pExec->nExitCode == 128 + pExec->nKillSignal) pExec->nSignal = pExec->nKillSignal;

    pExec->bExited = XTRUE;
    return XSTDOK;
}

XSTATUS XExec_Kill(xexec_t *pExec, int nSignal)
{
    XCHECK_NL((pExec != NULL), XSTDINV);
    if (pExec->bExited) return XSTDNON;
    XCHECK_NL((pExec->hProcess != NULL), XSTDERR);

    UINT nCode = (UINT)(128 + nSignal);
    BOOL bDone = (pExec->bGroup && pExec->hJob != NULL) ?
        TerminateJobObject(pExec->hJob, nCode) :
        TerminateProcess(pExec->hProcess, nCode);

    if (!bDone) return XExec_Wait(pExec, 0) == XSTDOK ? XSTDNON : XSTDERR;

    /* Termination completes asynchronously; XExec_Wait reports it, without this call waiting for it */
    pExec->nKillSignal = nSignal;
    return XSTDOK;
}

void XExec_Close(xexec_t *pExec)
{
    XCHECK_VOID_NL((pExec != NULL));
    XExec_CloseSock(&pExec->nStdIn);
    XExec_CloseSock(&pExec->nStdOut);
    XExec_CloseSock(&pExec->nStdErr);

    for (int i = 0; i < XEXEC_STREAMS; i++)
    {
        if (pExec->hPumps[i] == NULL) continue;

        /* Breaks whichever call the pump is in: the socket side by shutdown, the pipe side by cancelling
           its synchronous read or write. A pump that still does not leave keeps its socket, which is then
           left open rather than closed under a thread that may still be using it. */
        if (pExec->nBridges[i] != XSOCK_INVALID) shutdown(pExec->nBridges[i], SD_BOTH);
        CancelSynchronousIo(pExec->hPumps[i]);

        if (WaitForSingleObject(pExec->hPumps[i], 2000) == WAIT_OBJECT_0)
            XExec_CloseSock(&pExec->nBridges[i]);

        XExec_CloseHandle(&pExec->hPumps[i]);
        pExec->nBridges[i] = XSOCK_INVALID;
    }

    XExec_CloseHandle(&pExec->hJob);
    XExec_CloseHandle(&pExec->hProcess);
}

#endif /* _WIN32 */

void XExec_ResultInit(xexec_result_t *pResult)
{
    XCHECK_VOID_NL((pResult != NULL));
    memset(pResult, 0, sizeof(*pResult));
    XByteBuffer_Init(&pResult->output, XSTDNON, XFALSE);
    XByteBuffer_Init(&pResult->errors, XSTDNON, XFALSE);
}

void XExec_ResultClear(xexec_result_t *pResult)
{
    XCHECK_VOID_NL((pResult != NULL));
    XByteBuffer_Clear(&pResult->output);
    XByteBuffer_Clear(&pResult->errors);
    XExec_ResultInit(pResult);
}

#ifdef _WIN32
typedef WSAPOLLFD xexec_pollfd_t;
#define XExec_Poll(fds, count, ms) WSAPoll(fds, (ULONG)(count), ms)
#else
typedef struct pollfd xexec_pollfd_t;
#define XExec_Poll(fds, count, ms) poll(fds, (nfds_t)(count), ms)
#endif

#ifndef _WIN32
/* Interrupted, or not ready yet: the call is worth making again. EWOULDBLOCK is EAGAIN wherever it can be. */
static xbool_t XExec_Retry(int nError)
{
#if EAGAIN != EWOULDBLOCK
    if (nError == EWOULDBLOCK) return XTRUE;
#endif
    return (nError == EAGAIN || nError == EINTR) ? XTRUE : XFALSE;
}
#endif

/* > 0 bytes moved, 0 end of file (or a broken stream), XSTDERR nothing to do right now */
static int XExec_IoRead(XSOCKET nFd, uint8_t *pBuffer, size_t nSize)
{
#ifdef _WIN32
    int nRead = recv(nFd, (char*)pBuffer, (int)nSize, 0);
    if (nRead >= 0) return nRead;
    int nError = WSAGetLastError();
    return (nError == WSAEWOULDBLOCK || nError == WSAEINTR) ? XSTDERR : 0;
#else
    ssize_t nRead = read(nFd, pBuffer, nSize);
    if (nRead >= 0) return (int)nRead;
    return XExec_Retry(errno) ? XSTDERR : 0;
#endif
}

static int XExec_IoWrite(XSOCKET nFd, const uint8_t *pData, size_t nSize)
{
    if (nSize > XEXEC_IO_CHUNK) nSize = XEXEC_IO_CHUNK;
#ifdef _WIN32
    int nSent = send(nFd, (const char*)pData, (int)nSize, 0);
    if (nSent > 0) return nSent;
    int nError = WSAGetLastError();
    return (nSent < 0 && (nError == WSAEWOULDBLOCK || nError == WSAEINTR)) ? XSTDERR : 0;
#else
    /* A write of a non-empty buffer to a pipe never returns 0: that is taken as broken, like an error */
    ssize_t nSent = write(nFd, pData, nSize);
    if (nSent >= 0) return (int)nSent;
    return XExec_Retry(errno) ? XSTDERR : 0;
#endif
}

/* Drains what a stream has ready into pBuffer, keeping at most nMax bytes of it (0 keeps everything).
   Closes the stream at end of file. */
static void XExec_Collect(XSOCKET *pFd, xbyte_buffer_t *pBuffer, size_t nMax, xbool_t *pTruncated)
{
    uint8_t sChunk[XEXEC_IO_CHUNK];

    /* At most 16 chunks per call, so one busy stream cannot keep the other waiting */
    for (int nRound = 0; nRound < 16; nRound++)
    {
        int nRead = XExec_IoRead(*pFd, sChunk, sizeof(sChunk));
        if (nRead < 0) return;

        if (nRead == 0)
        {
            XExec_CloseSock(pFd);
            return;
        }

        size_t nKeep = (size_t)nRead;
        if (nMax > 0)
        {
            size_t nRoom = pBuffer->nUsed < nMax ? nMax - pBuffer->nUsed : 0;
            if (nKeep > nRoom)
            {
                nKeep = nRoom;
                *pTruncated = XTRUE;
            }
        }

        /* Out of memory: the output is cut short rather than the run abandoned */
        if (nKeep > 0 && XByteBuffer_Add(pBuffer, sChunk, nKeep) <= 0) *pTruncated = XTRUE;
    }
}

#ifndef _WIN32
static xbool_t XExec_IsPending(int nSignal)
{
    sigset_t pending;
    sigemptyset(&pending);
    if (sigpending(&pending) != 0) return XFALSE;
    return sigismember(&pending, nSignal) == 1 ? XTRUE : XFALSE;
}
#endif

XSTATUS XExec_Run(const xexec_opts_t *pOpts, const uint8_t *pInput, size_t nInput,
                  size_t nMaxOutput, int nTimeoutMs, xexec_result_t *pResult)
{
    XCHECK_NL((pOpts != NULL && pResult != NULL), XSTDINV);
    XCHECK_NL((pInput != NULL || !nInput), XSTDINV);

    /* Its own group, so a timeout reaches everything the program started, and never outliving its caller */
    xexec_opts_t opts = *pOpts;
    opts.nFlags |= XEXEC_STDIN | XEXEC_STDOUT | XEXEC_STDERR | XEXEC_NONBLOCK | XEXEC_GROUP | XEXEC_DIE_WITH_PARENT;

    pResult->nExitCode = XSTDERR;
    pResult->nSignal = 0;
    pResult->bTimedOut = XFALSE;
    pResult->bTruncated = XFALSE;

#ifndef _WIN32
    /* A child that exits before it has read its input turns the next write into SIGPIPE, which ends a
       caller that has not handled it. Held back for this thread while the run lasts, and taken if it came. */
    sigset_t pipeSet, savedMask;
    sigemptyset(&pipeSet);
    sigaddset(&pipeSet, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &pipeSet, &savedMask);
    xbool_t bPipePending = XExec_IsPending(SIGPIPE);
#endif

    xexec_t exec;
    XSTATUS nStatus = XExec_Spawn(&exec, &opts);

    if (nStatus == XSTDOK)
    {
        if (!nInput) XExec_CloseStdIn(&exec);

        uint64_t nStartMs = XTime_GetMonoMs();
        uint64_t nExitMs = 0;
        size_t nWritten = 0;

        for (;;)
        {
            uint64_t nNowMs = XTime_GetMonoMs();
            if (!exec.bExited && XExec_Wait(&exec, 0) == XSTDOK) nExitMs = nNowMs;

            if (exec.bExited)
            {
                if (exec.nStdOut == XSOCK_INVALID && exec.nStdErr == XSOCK_INVALID) break;

                /* Something the program started still holds its output open: not waited for */
                if (nNowMs - nExitMs >= XEXEC_RUN_GRACE_MS) break;
            }
            else if (nTimeoutMs > 0 && nNowMs - nStartMs >= (uint64_t)nTimeoutMs)
            {
                pResult->bTimedOut = XTRUE;
                XExec_Kill(&exec, SIGKILL);
                if (XExec_Wait(&exec, XEXEC_RUN_KILL_WAIT_MS) != XSTDOK) break;

                nExitMs = XTime_GetMonoMs();
                continue;
            }

            xexec_pollfd_t fds[XEXEC_STREAMS];
            XSOCKET *pFds[XEXEC_STREAMS];
            int nCount = 0;

            if (exec.nStdOut != XSOCK_INVALID) { pFds[nCount] = &exec.nStdOut; fds[nCount].events = POLLIN; nCount++; }
            if (exec.nStdErr != XSOCK_INVALID) { pFds[nCount] = &exec.nStdErr; fds[nCount].events = POLLIN; nCount++; }
            if (exec.nStdIn != XSOCK_INVALID) { pFds[nCount] = &exec.nStdIn; fds[nCount].events = POLLOUT; nCount++; }

            if (!nCount)
            {
                XExec_Wait(&exec, XEXEC_RUN_STEP_MS);
                continue;
            }

            for (int i = 0; i < nCount; i++)
            {
                fds[i].fd = *pFds[i];
                fds[i].revents = 0;
            }

            int nReady = XExec_Poll(fds, nCount, XEXEC_RUN_STEP_MS);
            if (nReady <= 0) continue;

            for (int i = 0; i < nCount; i++)
            {
                if (!fds[i].revents) continue;

                if (pFds[i] == &exec.nStdOut)
                    XExec_Collect(&exec.nStdOut, &pResult->output, nMaxOutput, &pResult->bTruncated);
                else if (pFds[i] == &exec.nStdErr)
                    XExec_Collect(&exec.nStdErr, &pResult->errors, nMaxOutput, &pResult->bTruncated);
                else
                {
                    int nSent = XExec_IoWrite(exec.nStdIn, pInput + nWritten, nInput - nWritten);
                    if (nSent > 0) nWritten += (size_t)nSent;

                    /* All of it delivered, or the child stopped reading */
                    if (nSent == 0 || nWritten >= nInput) XExec_CloseStdIn(&exec);
                }
            }
        }

        if (!exec.bExited)
        {
            XExec_Kill(&exec, SIGKILL);
            XExec_Wait(&exec, XEXEC_RUN_KILL_WAIT_MS);
        }

        pResult->nExitCode = exec.nExitCode;
        pResult->nSignal = exec.nSignal;
        nStatus = exec.bExited ? XSTDOK : XSTDERR;
        XExec_Close(&exec);
    }

#ifndef _WIN32
    if (!bPipePending && XExec_IsPending(SIGPIPE))
    {
        int nTaken = 0;
        sigwait(&pipeSet, &nTaken);
    }

    pthread_sigmask(SIG_SETMASK, &savedMask, NULL);
#endif

    return nStatus;
}
