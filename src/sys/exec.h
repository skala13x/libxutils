/*!
 *  @file libxutils/src/sys/exec.h
 *
 *  This source is part of the "libxutils" project
 *  2015-2026  Sandro Kalatozishvili (s.kalatoz@gmail.com)
 *
 * @brief Child process execution with standard streams an event loop can poll.
 */

#ifndef __XUTILS_XEXEC_H__
#define __XUTILS_XEXEC_H__

#include "xstd.h"
#include "sock.h"
#include "buf.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
    Which of the child's standard streams come back to the caller. A stream
    that is not piped is connected to the null device, never to the caller's
    own stdio, so a child cannot read the caller's terminal by accident.
*/
#define XEXEC_STDIN             (1U << 0)
#define XEXEC_STDOUT            (1U << 1)
#define XEXEC_STDERR            (1U << 2)

/* stderr goes into the stdout pipe: one stream, in the order it was written */
#define XEXEC_MERGE_STDERR      (1U << 3)

/*
    The child leads a process group of its own (a job object on Windows), so
    XExec_Kill reaches everything it starts, and a signal sent to the caller's
    group - Ctrl-C at the caller's terminal - does not reach the child.
*/
#define XEXEC_GROUP             (1U << 4)

/* The caller's ends of the pipes do not block */
#define XEXEC_NONBLOCK          (1U << 5)

/*
    The program does not outlive its caller. Linux: it is killed (SIGKILL)
    when the thread that started it exits, and does not start at all when
    that thread is already gone. Windows: it runs in a job killed when the
    last handle to it closes - when the caller exits, or calls XExec_Close.
    Where that cannot be arranged the spawn fails rather than run without it.
    Elsewhere nothing: the caller has to stop it itself.
*/
#define XEXEC_DIE_WITH_PARENT   (1U << 6)

/* Exit codes of a child that never ran the program, as shells report them */
#define XEXEC_EXIT_NOEXEC       126     /* Found, but could not be executed */
#define XEXEC_EXIT_NOTFOUND     127     /* Missing program, identity or working directory */

#define XEXEC_WAIT_FOREVER      -1

typedef struct XExecOptions {
    const char *pPath;          /* Program to run. There is no PATH search: give its full path */
    char *const *ppArgv;        /* NULL-terminated, argv[0] included. NULL runs pPath with no arguments */
    char *const *ppEnv;         /* NULL-terminated "NAME=value" list. NULL inherits the caller's environment */
    const char *pWorkDir;       /* NULL keeps the caller's working directory */
    const char *pWorkDirFallback; /* Tried when pWorkDir cannot be entered, as login falls back to /; NULL fails */
    uint32_t nFlags;            /* XEXEC_* */

#ifdef _WIN32
    /* Used as is instead of quoting ppArgv: cmd.exe parses its command line by rules of its own */
    const char *pCmdLine;
#else
    /*
        Identity the child takes before it runs the program: supplementary
        groups first, then the gid, then the uid. Resolved by the caller,
        because after fork() a threaded caller's child may only make system
        calls - a name-service lookup there can wait forever on a lock whose
        owner was not copied into the child.
    */
    xbool_t bSetIdentity;
    uid_t nUid;
    gid_t nGid;
    const gid_t *pGroups;
    size_t nGroups;
#endif
} xexec_opts_t;

typedef struct XExec {
    /*
        The caller's ends of the child's streams, XSOCK_INVALID when a stream
        is not piped. POSIX: pipe descriptors. Windows: sockets of a socket
        pair, which WSAPoll can wait on where it cannot wait on a pipe, bridged
        to the child's pipes by one thread per stream.
    */
    XSOCKET nStdIn;
    XSOCKET nStdOut;
    XSOCKET nStdErr;

    xpid_t nPid;                /* Valid while running, XSTDERR before a spawn and after a failed one */
    int nExitCode;              /* Once bExited: the exit status, or 128 + the signal that ended it */
    int nSignal;                /* POSIX, once bExited: the signal that ended it, 0 for a normal exit */
    xbool_t bExited;
    xbool_t bGroup;

#ifdef _WIN32
    HANDLE hProcess;
    HANDLE hJob;
    HANDLE hPumps[3];           /* One thread per piped stream; each owns its pipe handle */
    XSOCKET nBridges[3];        /* The pump side of each socket pair */
    int nKillSignal;            /* What XExec_Kill was asked for, reported once the exit code matches */
#endif
} xexec_t;

void XExec_Init(xexec_t *pExec);

/*
    Starts the program. XSTDOK when the child exists: a program that turns out
    to be missing, an identity the child cannot take or a working directory it
    cannot enter make the child exit with XEXEC_EXIT_NOTFOUND (or
    XEXEC_EXIT_NOEXEC) after a one-line reason on its stderr, the way a shell
    reports them. The caller is never blocked on the child: nothing here waits
    for it to reach the program, so a working directory on a dead network mount
    stalls the child, not the caller.
*/
XSTATUS XExec_Spawn(xexec_t *pExec, const xexec_opts_t *pOpts);

/*
    Collects the exit status. nTimeoutMs 0 only checks, XEXEC_WAIT_FOREVER
    blocks. XSTDOK once exited (nExitCode, nSignal and bExited are set),
    XSTDNON while still running, XSTDERR when there is no child to wait for.
*/
XSTATUS XExec_Wait(xexec_t *pExec, int nTimeoutMs);

/*
    Sends nSignal - to the whole group with XEXEC_GROUP. Windows has no
    signals: any signal ends the job (or the process) with exit code
    128 + nSignal. XSTDNON when the child has already been collected.
*/
XSTATUS XExec_Kill(xexec_t *pExec, int nSignal);

/* The child reads end of file from then on */
void XExec_CloseStdIn(xexec_t *pExec);

/*
    Releases the caller's ends and handles. Neither waits for nor kills the
    child: a caller that wants it gone kills and collects it first. A stream
    the caller has handed to an owner that closes it (an event loop) is set to
    XSOCK_INVALID beforehand so that it is not closed twice.
*/
void XExec_Close(xexec_t *pExec);

typedef struct XExecResult {
    xbyte_buffer_t output;      /* What the child wrote to stdout, at most nMaxOutput bytes */
    xbyte_buffer_t errors;      /* What it wrote to stderr, at most nMaxOutput bytes */
    int nExitCode;
    int nSignal;
    xbool_t bTimedOut;          /* Killed because it outlived nTimeoutMs */
    xbool_t bTruncated;         /* A stream wrote more than nMaxOutput: the rest was read and dropped */
} xexec_result_t;

void XExec_ResultInit(xexec_result_t *pResult);
void XExec_ResultClear(xexec_result_t *pResult);

/*
    Runs the program to completion: feeds it pInput (then end of file), keeps
    the first nMaxOutput bytes of each output stream (0 keeps everything) and
    kills its group once it outlives nTimeoutMs (0 waits as long as it takes).
    The program runs with XEXEC_GROUP and XEXEC_DIE_WITH_PARENT.
    A child that exits while something it started still holds its output open
    is not waited for beyond a short grace. Blocks the caller throughout.
    XSTDOK when the child ran and was collected, whatever its exit code.
*/
XSTATUS XExec_Run(const xexec_opts_t *pOpts, const uint8_t *pInput, size_t nInput,
                  size_t nMaxOutput, int nTimeoutMs, xexec_result_t *pResult);

#ifdef __cplusplus
}
#endif

#endif /* __XUTILS_XEXEC_H__ */
