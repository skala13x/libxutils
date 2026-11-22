/* libxutils: child process execution when the system says no.
 *
 * Every system call exec.c makes is wrapped (-Wl,--wrap) and made to fail on request, in the caller or in the
 * child, and each case asserts what the caller sees: the status, errno, the child's exit code and its one line of
 * reason, and that nothing is left behind - no descriptor, no signal mask, no child. The wrappers pass everything
 * that is not theirs straight through: the coverage runtime opens, locks, reads and writes its data files through
 * the same symbols, in this process and in every child, so a fault only fires for the exact call it was armed for.
 */

#define _GNU_SOURCE
#include "test.h"
#include "exec.h"
#include "buf.h"
#include "xtime.h"
#include <dirent.h>
#include <grp.h>
#include <signal.h>
#include <stdarg.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#define FAULT_FD_MAX 8

typedef struct {
    pid_t nOwner;               /* Caller-side faults fire only in this process */
    int nForkErrno;
    int nPipe2Call;             /* Fail the Nth pipe2() from now (1-based), 0 never */
    int nPipe2Errno;
    xbool_t bPipeFails;
    xbool_t bNullFails;
    xbool_t bGetFlFails;
    xbool_t bSetFlFails;
    xbool_t bDupFdFails;        /* Child side */
    xbool_t bDup2Fails;         /* Child side */
    xbool_t bSetsidFails;       /* Child side */
    int nIdentity;              /* Child side: 0 real, 1 all succeed, 2/3/4 the 1st/2nd/3rd call fails */
    xbool_t bCloseRangeFails;   /* Child side */
    xbool_t bPrctlFails;        /* Child side */
    xbool_t bOrphaned;          /* Child side: getppid() names someone else */
    long nOpenMax;              /* sysconf(_SC_OPEN_MAX) answer, 0 real */
    int nWaitEintr;             /* waitpid() answers EINTR this many times */
    xbool_t bWaitNever;         /* waitpid() never reports the child */
    int nWaitErrno;
    int nGroupKillErrno;        /* kill() of a group fails with this */
    int nKillErrno;             /* kill() of a single process fails with this */
    int nPipeFds[FAULT_FD_MAX]; /* Caller ends created through pipe2(), the only ones I/O faults touch */
    int nPipeFdCount;
    int nReadErrno;             /* Next reads of a recorded fd fail with this, nReadFaults times */
    int nReadFaults;
    int nReadTrickle;           /* Next reads of a recorded fd return one byte, this many times */
    int nWriteErrno;
    int nWriteFaults;
    xbool_t bBufferFails;
    xbool_t bPendingFails;
} exec_fault_t;

static exec_fault_t g_fault;

static void fault_reset(void)
{
    memset(&g_fault, 0, sizeof(g_fault));
    g_fault.nOwner = getpid();
}

static xbool_t fault_owner(void) { return getpid() == g_fault.nOwner ? XTRUE : XFALSE; }

static xbool_t fault_recorded(int nFd)
{
    for (int i = 0; i < g_fault.nPipeFdCount; i++)
        if (g_fault.nPipeFds[i] == nFd) return XTRUE;
    return XFALSE;
}

pid_t __real_fork(void);
int __real_pipe2(int[2], int);
int __real_pipe(int[2]);
int __real_open(const char*, int, ...);
int __real_fcntl(int, int, ...);
int __real_dup2(int, int);
pid_t __real_setsid(void);
int __real_setgroups(size_t, const gid_t*);
int __real_setgid(gid_t);
int __real_setuid(uid_t);
int __real_close_range(unsigned int, unsigned int, int);
long __real_sysconf(int);
int __real_prctl(int, ...);
pid_t __real_getppid(void);
pid_t __real_waitpid(pid_t, int*, int);
int __real_kill(pid_t, int);
ssize_t __real_read(int, void*, size_t);
ssize_t __real___read_chk(int, void*, size_t, size_t);
ssize_t __real_write(int, const void*, size_t);
int __real_XByteBuffer_Add(xbyte_buffer_t*, const uint8_t*, size_t);
int __real_sigpending(sigset_t*);

pid_t __wrap_fork(void)
{
    if (fault_owner() && g_fault.nForkErrno) { errno = g_fault.nForkErrno; return -1; }
    return __real_fork();
}

int __wrap_pipe2(int nFds[2], int nFlags)
{
    if (fault_owner() && g_fault.nPipe2Call && --g_fault.nPipe2Call == 0)
    {
        errno = g_fault.nPipe2Errno;
        return -1;
    }

    int nStatus = __real_pipe2(nFds, nFlags);
    if (nStatus == 0 && fault_owner() && g_fault.nPipeFdCount + 2 <= FAULT_FD_MAX)
    {
        g_fault.nPipeFds[g_fault.nPipeFdCount++] = nFds[0];
        g_fault.nPipeFds[g_fault.nPipeFdCount++] = nFds[1];
    }

    return nStatus;
}

int __wrap_pipe(int nFds[2])
{
    if (fault_owner() && g_fault.bPipeFails) { errno = EMFILE; return -1; }
    return __real_pipe(nFds);
}

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

    if (fault_owner() && g_fault.bNullFails && strcmp(pPath, "/dev/null") == 0) { errno = EMFILE; return -1; }
    return __real_open(pPath, nFlags, nMode);
}

int __wrap_fcntl(int nFd, int nCommand, ...)
{
    va_list args;
    va_start(args, nCommand);

    /* Every argument travels as the pointer-sized word glibc itself reads: the coverage runtime's lock
       commands pass a struct flock* through here */
    void *pArg = va_arg(args, void*);
    va_end(args);

    if (nCommand == F_GETFL && fault_owner() && g_fault.bGetFlFails && fault_recorded(nFd)) { errno = EBADF; return -1; }
    if (nCommand == F_SETFL && fault_owner() && g_fault.bSetFlFails && fault_recorded(nFd)) { errno = EBADF; return -1; }
    if (nCommand == F_DUPFD && !fault_owner() && g_fault.bDupFdFails) { errno = EMFILE; return -1; }
    return __real_fcntl(nFd, nCommand, pArg);
}

int __wrap_dup2(int nOld, int nNew)
{
    if (!fault_owner() && g_fault.bDup2Fails) { errno = EBADF; return -1; }
    return __real_dup2(nOld, nNew);
}

pid_t __wrap_setsid(void)
{
    if (!fault_owner() && g_fault.bSetsidFails) { errno = EPERM; return -1; }
    return __real_setsid();
}

/* nIdentity: 1 lets all three "succeed" without changing anything, 2..4 fails the setgroups, setgid or setuid */
static int fault_identity(int nCall)
{
    if (fault_owner() || !g_fault.nIdentity) return -2;
    if (g_fault.nIdentity == nCall + 1) { errno = EPERM; return -1; }
    return 0;
}

int __wrap_setgroups(size_t nCount, const gid_t *pGroups)
{
    int nFault = fault_identity(1);
    return nFault != -2 ? nFault : __real_setgroups(nCount, pGroups);
}

int __wrap_setgid(gid_t nGid)
{
    int nFault = fault_identity(2);
    return nFault != -2 ? nFault : __real_setgid(nGid);
}

int __wrap_setuid(uid_t nUid)
{
    int nFault = fault_identity(3);
    return nFault != -2 ? nFault : __real_setuid(nUid);
}

int __wrap_close_range(unsigned int nFirst, unsigned int nLast, int nFlags)
{
    if (!fault_owner() && g_fault.bCloseRangeFails) { errno = ENOSYS; return -1; }
    return __real_close_range(nFirst, nLast, nFlags);
}

long __wrap_sysconf(int nName)
{
    if (nName == _SC_OPEN_MAX && fault_owner() && g_fault.nOpenMax) return g_fault.nOpenMax;
    return __real_sysconf(nName);
}

int __wrap_prctl(int nOption, ...)
{
    va_list args;
    va_start(args, nOption);
    unsigned long nArgs[4];
    for (int i = 0; i < 4; i++) nArgs[i] = va_arg(args, unsigned long);
    va_end(args);

    if (nOption == PR_SET_PDEATHSIG && !fault_owner() && g_fault.bPrctlFails) { errno = EINVAL; return -1; }
    return __real_prctl(nOption, nArgs[0], nArgs[1], nArgs[2], nArgs[3]);
}

pid_t __wrap_getppid(void)
{
    if (!fault_owner() && g_fault.bOrphaned) return 1;
    return __real_getppid();
}

pid_t __wrap_waitpid(pid_t nPid, int *pStatus, int nOptions)
{
    if (fault_owner() && g_fault.nWaitEintr > 0) { g_fault.nWaitEintr--; errno = EINTR; return -1; }
    if (fault_owner() && g_fault.nWaitErrno) { errno = g_fault.nWaitErrno; return -1; }
    if (fault_owner() && g_fault.bWaitNever && (nOptions & WNOHANG)) return 0;
    return __real_waitpid(nPid, pStatus, nOptions);
}

int __wrap_kill(pid_t nPid, int nSignal)
{
    if (fault_owner() && nPid < 0 && g_fault.nGroupKillErrno) { errno = g_fault.nGroupKillErrno; return -1; }
    if (fault_owner() && nPid > 0 && g_fault.nKillErrno) { errno = g_fault.nKillErrno; return -1; }
    return __real_kill(nPid, nSignal);
}

static ssize_t fault_read(int nFd, void *pData, size_t nSize, xbool_t *pFaulted)
{
    *pFaulted = XFALSE;
    if (!fault_owner() || !fault_recorded(nFd)) return 0;

    if (g_fault.nReadFaults > 0)
    {
        g_fault.nReadFaults--;
        *pFaulted = XTRUE;
        errno = g_fault.nReadErrno;
        return -1;
    }

    if (g_fault.nReadTrickle > 0 && nSize > 1)
    {
        /* One byte at a time: the stream always has more to give than one pass takes */
        g_fault.nReadTrickle--;
        *pFaulted = XTRUE;
        ssize_t nRead = __real_read(nFd, pData, 1);
        return nRead;
    }

    return 0;
}

ssize_t __wrap_read(int nFd, void *pData, size_t nSize)
{
    xbool_t bFaulted = XFALSE;
    ssize_t nRet = fault_read(nFd, pData, nSize, &bFaulted);
    return bFaulted ? nRet : __real_read(nFd, pData, nSize);
}

ssize_t __wrap___read_chk(int nFd, void *pData, size_t nSize, size_t nCapacity)
{
    xbool_t bFaulted = XFALSE;
    ssize_t nRet = fault_read(nFd, pData, nSize, &bFaulted);
    return bFaulted ? nRet : __real___read_chk(nFd, pData, nSize, nCapacity);
}

ssize_t __wrap_write(int nFd, const void *pData, size_t nSize)
{
    if (fault_owner() && fault_recorded(nFd) && g_fault.nWriteFaults > 0)
    {
        g_fault.nWriteFaults--;
        errno = g_fault.nWriteErrno;
        return -1;
    }

    return __real_write(nFd, pData, nSize);
}

int __wrap_XByteBuffer_Add(xbyte_buffer_t *pBuffer, const uint8_t *pData, size_t nSize)
{
    if (fault_owner() && g_fault.bBufferFails) return XSTDERR;
    return __real_XByteBuffer_Add(pBuffer, pData, nSize);
}

int __wrap_sigpending(sigset_t *pSet)
{
    if (fault_owner() && g_fault.bPendingFails) { errno = EFAULT; return -1; }
    return __real_sigpending(pSet);
}

/* ---- helpers ---------------------------------------------------------------------------------------- */

static int open_fds(void)
{
    int nCount = 0;
    DIR *pDir = opendir("/proc/self/fd");
    if (pDir == NULL) return -1;

    struct dirent *pEntry;
    while ((pEntry = readdir(pDir)) != NULL)
        if (pEntry->d_name[0] != '.') nCount++;

    closedir(pDir);
    return nCount - 1;      /* The directory's own descriptor */
}

static int mask_is(const sigset_t *pExpected)
{
    sigset_t current;
    pthread_sigmask(SIG_SETMASK, NULL, &current);
    for (int nSig = 1; nSig < SIGRTMIN; nSig++)
        if (sigismember(&current, nSig) != sigismember(pExpected, nSig)) return 0;
    return 1;
}

static void sh_opts(xexec_opts_t *pOpts, char **ppArgv, const char *pScript)
{
    ppArgv[0] = (char*)"sh";
    ppArgv[1] = (char*)"-c";
    ppArgv[2] = (char*)pScript;
    ppArgv[3] = NULL;

    memset(pOpts, 0, sizeof(*pOpts));
    pOpts->pPath = "/bin/sh";
    pOpts->ppArgv = ppArgv;
}

/* Runs a script with the faults already armed; the result goes to pResult. */
static int run_sh(const char *pScript, uint32_t nFlags, xexec_result_t *pResult)
{
    char *aArgv[4];
    xexec_opts_t opts;
    sh_opts(&opts, aArgv, pScript);
    opts.nFlags = nFlags;

    fflush(stdout);
    fflush(stderr);
    XExec_ResultInit(pResult);
    return XExec_Run(&opts, NULL, 0, 0, 10000, pResult);
}

static int errors_have(const xexec_result_t *pResult, const char *pText)
{
    size_t nLength = strlen(pText);
    for (size_t i = 0; pResult->errors.nUsed >= nLength && i + nLength <= pResult->errors.nUsed; i++)
        if (!memcmp(pResult->errors.pData + i, pText, nLength)) return 1;
    return 0;
}

/* ---- cases ------------------------------------------------------------------------------------------ */

static int XTest_spawn_failures(void)
{
    char *aArgv[4];
    xexec_opts_t opts;
    sh_opts(&opts, aArgv, "exit 0");
    opts.nFlags = XEXEC_STDIN | XEXEC_STDOUT | XEXEC_STDERR;

    sigset_t before;
    pthread_sigmask(SIG_SETMASK, NULL, &before);
    int nBefore = open_fds();
    xexec_t exec;

    fault_reset();
    g_fault.bNullFails = XTRUE;
    CHECK(XExec_Spawn(&exec, &opts) == XSTDERR && errno == EMFILE, "No null device: the start fails with its errno");
    CHECK(open_fds() == nBefore, "A failed start leaves no descriptor open");

    for (int nCall = 1; nCall <= 3; nCall++)
    {
        fault_reset();
        g_fault.nPipe2Call = nCall;
        g_fault.nPipe2Errno = EMFILE;
        CHECK(XExec_Spawn(&exec, &opts) == XSTDERR && errno == EMFILE, "A pipe that cannot be made fails the start");
        CHECK(open_fds() == nBefore, "The pipes made before the one that failed are closed again");
    }

    fault_reset();
    g_fault.nForkErrno = EAGAIN;
    CHECK(XExec_Spawn(&exec, &opts) == XSTDERR && errno == EAGAIN, "A fork that fails fails the start with its errno");

    xexec_result_t result;
    XExec_ResultInit(&result);
    CHECK(XExec_Run(&opts, NULL, 0, 0, 1000, &result) == XSTDERR && result.nExitCode == XSTDERR,
        "A run whose program cannot be started fails and reports no exit");
    XExec_ResultClear(&result);
    CHECK(open_fds() == nBefore, "A failed fork leaves no descriptor open");
    CHECK(mask_is(&before), "A failed fork gives the caller its signal mask back");

    /* pipe2 missing (an old kernel or libc): pipe and close-on-exec by hand */
    fault_reset();
    g_fault.nPipe2Call = 1;
    g_fault.nPipe2Errno = ENOSYS;
    CHECK(XExec_Spawn(&exec, &opts) == XSTDOK, "Without pipe2 the pipes are made the old way");
    int bCloexec = (__real_fcntl(exec.nStdIn, F_GETFD, 0) & FD_CLOEXEC) && (__real_fcntl(exec.nStdOut, F_GETFD, 0) & FD_CLOEXEC);
    CHECK(XExec_Wait(&exec, 5000) == XSTDOK && exec.nExitCode == 0, "The child started on pipes made the old way runs");
    XExec_Close(&exec);
    CHECK(bCloexec, "Pipes made the old way are still close-on-exec");

    fault_reset();
    g_fault.nPipe2Call = 1;
    g_fault.nPipe2Errno = ENOSYS;
    g_fault.bPipeFails = XTRUE;
    CHECK(XExec_Spawn(&exec, &opts) == XSTDERR && errno == EMFILE, "Without pipe2, a pipe() that fails fails the start");
    CHECK(open_fds() == nBefore, "Nothing is left open by the fallback failing");

    fault_reset();
    CHECK(mask_is(&before), "Every start, failed or not, leaves the caller's signal mask as it was");
    return 0;
}

static int XTest_nonblock_failures(void)
{
    char *aArgv[4];
    xexec_opts_t opts;
    sh_opts(&opts, aArgv, "exit 0");
    opts.nFlags = XEXEC_STDOUT | XEXEC_NONBLOCK;

    /* Best effort: a pipe that cannot be made non-blocking is still a working pipe */
    for (int nCase = 0; nCase < 2; nCase++)
    {
        xexec_t exec;
        fault_reset();
        if (nCase == 0) g_fault.bGetFlFails = XTRUE;
        else g_fault.bSetFlFails = XTRUE;

        CHECK(XExec_Spawn(&exec, &opts) == XSTDOK, "A start whose pipe stays blocking still succeeds");
        int nFlags = __real_fcntl(exec.nStdOut, F_GETFL, 0);
        CHECK(!(nFlags & O_NONBLOCK), "The pipe that could not be switched is left blocking");
        CHECK(XExec_Wait(&exec, 5000) == XSTDOK && exec.nExitCode == 0, "The child runs");
        XExec_Close(&exec);
    }

    fault_reset();
    return 0;
}

static int XTest_child_failures(void)
{
    xexec_result_t result;

    fault_reset();
    g_fault.bDup2Fails = XTRUE;
    CHECK(run_sh("echo ran", 0, &result) == XSTDOK, "A child that cannot set up its streams is collected");
    CHECK(result.nExitCode == XEXEC_EXIT_NOTFOUND && result.output.nUsed == 0,
        "A child that cannot set up its streams exits 127 without running the program");
    XExec_ResultClear(&result);

    fault_reset();
    g_fault.bSetsidFails = XTRUE;
    CHECK(run_sh("echo ran", 0, &result) == XSTDOK, "A child that cannot lead a group is collected");
    CHECK(result.nExitCode == XEXEC_EXIT_NOTFOUND && errors_have(&result, "cannot start a new process group") &&
        result.output.nUsed == 0, "A child that cannot lead its group says so and never runs the program");
    XExec_ResultClear(&result);

    /* Each of the three identity steps failing on its own, then all of them succeeding */
    char *aArgv[4];
    xexec_opts_t opts;
    sh_opts(&opts, aArgv, "echo ran");
    opts.bSetIdentity = XTRUE;
    opts.nUid = getuid();
    opts.nGid = getgid();

    for (int nStep = 2; nStep <= 4; nStep++)
    {
        fault_reset();
        g_fault.nIdentity = nStep;
        fflush(stdout);
        XExec_ResultInit(&result);
        CHECK(XExec_Run(&opts, NULL, 0, 0, 10000, &result) == XSTDOK, "A child refused an identity step is collected");
        CHECK(result.nExitCode == XEXEC_EXIT_NOTFOUND && errors_have(&result, "cannot switch to the requested user") &&
            result.output.nUsed == 0, "Any one identity step failing stops the program from running");
        XExec_ResultClear(&result);
    }

    gid_t aGroups[1] = { getgid() };
    opts.pGroups = aGroups;
    opts.nGroups = 1;

    fault_reset();
    g_fault.nIdentity = 1;
    XExec_ResultInit(&result);
    CHECK(XExec_Run(&opts, NULL, 0, 0, 10000, &result) == XSTDOK && result.nExitCode == 0 &&
        result.output.nUsed == 4, "With every identity step accepted the program runs");
    XExec_ResultClear(&result);

    /* Tied to the caller: it cannot be, or the caller is already gone */
    fault_reset();
    g_fault.bPrctlFails = XTRUE;
    CHECK(run_sh("echo ran", XEXEC_DIE_WITH_PARENT, &result) == XSTDOK, "A child that cannot be tied to its caller is collected");
    CHECK(result.nExitCode == XEXEC_EXIT_NOTFOUND && errors_have(&result, "cannot tie the program to its parent") &&
        result.output.nUsed == 0, "A program that could outlive its caller is not started");
    XExec_ResultClear(&result);

    fault_reset();
    g_fault.bOrphaned = XTRUE;
    CHECK(run_sh("echo ran", XEXEC_DIE_WITH_PARENT, &result) == XSTDOK, "An orphaned child is collected");
    CHECK(result.nExitCode == XEXEC_EXIT_NOTFOUND && result.output.nUsed == 0,
        "A program whose caller is already gone is not started");
    XExec_ResultClear(&result);

    fault_reset();
    return 0;
}

/* In a process whose stdio is closed, so the null device and the pipes land on 0, 1 and 2 and have to be moved
   out of the way first; with F_DUPFD failing that move is impossible. Exit status is the verdict. */
static int exec_dupfd_body(void)
{
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);

    fault_reset();
    g_fault.bDupFdFails = XTRUE;

    xexec_result_t result;
    if (run_sh("echo ran", 0, &result) != XSTDOK) return 10;
    int nCode = result.nExitCode == XEXEC_EXIT_NOTFOUND && result.output.nUsed == 0 ? 0 : 11;
    XExec_ResultClear(&result);
    return nCode;
}

static int XTest_dupfd_failure(void)
{
    fflush(stdout);
    fflush(stderr);

    pid_t nPid = __real_fork();
    CHECK(nPid >= 0, "A helper process");
    if (nPid == 0) XTEST_EXIT(exec_dupfd_body());

    int nStatus = 0;
    CHECK(__real_waitpid(nPid, &nStatus, 0) == nPid, "The helper is collected");
    CHECK(WIFEXITED(nStatus) && WEXITSTATUS(nStatus) == 0,
        "A child that cannot move a low descriptor out of the way exits 127 without running the program");
    return 0;
}

static int XTest_descriptor_limits(void)
{
    /* close_range missing: every descriptor up to the limit is closed one by one, whatever the limit says */
    int nFd = __real_open("/dev/null", O_RDONLY);
    CHECK(nFd >= 0 && __real_dup2(nFd, 9) == 9, "A descriptor at 9 without close-on-exec");
    if (nFd != 9) close(nFd);

    const long nLimits[3] = { -1, 1L << 30, 0 };
    for (int i = 0; i < 3; i++)
    {
        fault_reset();
        g_fault.bCloseRangeFails = XTRUE;
        g_fault.nOpenMax = nLimits[i];

        xexec_result_t result;
        CHECK(run_sh("if true <&9 2>/dev/null; then echo open; else echo closed; fi", 0, &result) == XSTDOK,
            "The descriptor check runs without close_range");
        CHECK(result.output.nUsed == 7 && !memcmp(result.output.pData, "closed\n", 7),
            "Without close_range a leaked descriptor is still closed, for any limit sysconf reports");
        XExec_ResultClear(&result);
    }

    close(9);
    fault_reset();
    return 0;
}

static int XTest_wait_failures(void)
{
    char *aArgv[4];
    xexec_opts_t opts;
    sh_opts(&opts, aArgv, "exit 4");
    xexec_t exec;

    fault_reset();
    CHECK(XExec_Spawn(&exec, &opts) == XSTDOK, "A child to wait for");
    g_fault.nWaitEintr = 3;
    CHECK(XExec_Wait(&exec, XEXEC_WAIT_FOREVER) == XSTDOK && exec.nExitCode == 4, "An interrupted wait is resumed");
    XExec_Close(&exec);

    fault_reset();
    CHECK(XExec_Spawn(&exec, &opts) == XSTDOK, "A child to fail waiting for");
    g_fault.nWaitErrno = EINVAL;
    CHECK(XExec_Wait(&exec, 0) == XSTDERR && !exec.bExited, "A wait the system refuses is reported, not taken as an exit");
    g_fault.nWaitErrno = 0;
    CHECK(XExec_Wait(&exec, XEXEC_WAIT_FOREVER) == XSTDOK, "The child is still collected afterwards");
    XExec_Close(&exec);

    fault_reset();
    return 0;
}

static int XTest_kill_paths(void)
{
    char *aArgv[4];
    xexec_opts_t opts;
    sh_opts(&opts, aArgv, "exec sleep 30");
    xexec_t exec;

    /* A group not there yet - the child before its setsid() - is reached through the child itself */
    fault_reset();
    opts.nFlags = XEXEC_GROUP;
    CHECK(XExec_Spawn(&exec, &opts) == XSTDOK, "A child in a group of its own");
    g_fault.nGroupKillErrno = ESRCH;
    CHECK(XExec_Kill(&exec, SIGTERM) == XSTDOK, "A group that is not there yet is reached through the child");
    g_fault.nGroupKillErrno = 0;
    CHECK(XExec_Wait(&exec, 5000) == XSTDOK && exec.nSignal == SIGTERM, "The child got the signal");
    XExec_Close(&exec);

    fault_reset();
    CHECK(XExec_Spawn(&exec, &opts) == XSTDOK, "A child for a refused group signal");
    g_fault.nGroupKillErrno = EPERM;
    CHECK(XExec_Kill(&exec, SIGTERM) == XSTDERR && errno == EPERM, "A group signal the system refuses is reported");
    g_fault.nGroupKillErrno = ESRCH;
    g_fault.nKillErrno = ESRCH;
    CHECK(XExec_Kill(&exec, SIGTERM) == XSTDNON, "Neither the group nor the child there means nothing is left to signal");
    fault_reset();
    CHECK(XExec_Kill(&exec, SIGKILL) == XSTDOK && XExec_Wait(&exec, 5000) == XSTDOK, "The child is stopped and collected");
    XExec_Close(&exec);

    /* Without a group: the child alone */
    opts.nFlags = 0;
    CHECK(XExec_Spawn(&exec, &opts) == XSTDOK, "A child without a group of its own");
    CHECK(!exec.bGroup, "The child is not a group leader");
    g_fault.nKillErrno = EPERM;
    CHECK(XExec_Kill(&exec, SIGTERM) == XSTDERR, "A signal the system refuses is reported");
    fault_reset();
    CHECK(XExec_Kill(&exec, SIGTERM) == XSTDOK, "A child without a group is signalled on its own");
    CHECK(XExec_Wait(&exec, 5000) == XSTDOK && exec.nSignal == SIGTERM, "The lone child got the signal");
    XExec_Close(&exec);

    fault_reset();
    return 0;
}

static int XTest_run_io(void)
{
    xexec_result_t result;

    /* Reads interrupted, not ready and broken: the first two are retried, the last ends the stream */
    const int nErrnos[3] = { EINTR, EAGAIN, EIO };
    for (int i = 0; i < 3; i++)
    {
        fault_reset();
        g_fault.nReadErrno = nErrnos[i];
        g_fault.nReadFaults = 2;
        /* A broken stream is closed, so what the child writes to it afterwards may end it with SIGPIPE */
        CHECK(run_sh("sleep 0.2; printf data", 0, &result) == XSTDOK, "A run with failing reads completes");
        CHECK(nErrnos[i] == EIO || (result.nExitCode == 0 && result.output.nUsed == 4 && !memcmp(result.output.pData, "data", 4)),
            "Reads that are interrupted or not ready yet are tried again and lose nothing");
        XExec_ResultClear(&result);
    }

    /* A stream with more to give than one pass takes still arrives whole */
    fault_reset();
    g_fault.nReadTrickle = 40;
    CHECK(run_sh("printf 0123456789012345678901234567890123456789", 0, &result) == XSTDOK, "A trickling run completes");
    CHECK(result.output.nUsed == 40, "Output read a byte at a time over many passes arrives whole");
    XExec_ResultClear(&result);

    /* Input the child is not ready for, and input it can no longer take */
    char *aArgv[4];
    xexec_opts_t opts;
    sh_opts(&opts, aArgv, "cat");
    const char *pInput = "fed in two goes\n";

    fault_reset();
    g_fault.nWriteErrno = EAGAIN;
    g_fault.nWriteFaults = 3;
    XExec_ResultInit(&result);
    CHECK(XExec_Run(&opts, (const uint8_t*)pInput, strlen(pInput), 0, 10000, &result) == XSTDOK,
        "A run with a busy stdin completes");
    CHECK(result.output.nUsed == strlen(pInput), "Input the child was not ready for is written once it is");
    XExec_ResultClear(&result);

    fault_reset();
    g_fault.nWriteErrno = EINTR;
    g_fault.nWriteFaults = 2;
    XExec_ResultInit(&result);
    CHECK(XExec_Run(&opts, (const uint8_t*)pInput, strlen(pInput), 0, 10000, &result) == XSTDOK &&
        result.output.nUsed == strlen(pInput), "Interrupted writes of the input are made again");
    XExec_ResultClear(&result);

    fault_reset();
    g_fault.nWriteErrno = EPIPE;
    g_fault.nWriteFaults = 1;
    XExec_ResultInit(&result);
    CHECK(XExec_Run(&opts, (const uint8_t*)pInput, strlen(pInput), 0, 10000, &result) == XSTDOK && result.nExitCode == 0,
        "Input the child can no longer take ends its stdin, and the run completes");
    CHECK(result.output.nUsed == 0, "Nothing of the refused input reached the child");
    XExec_ResultClear(&result);

    /* Output that cannot be kept is reported cut, not lost silently */
    fault_reset();
    g_fault.bBufferFails = XTRUE;
    CHECK(run_sh("printf kept", 0, &result) == XSTDOK && result.nExitCode == 0, "A run whose output cannot be kept completes");
    CHECK(result.bTruncated && result.output.nUsed == 0, "Output that could not be stored is reported as cut");
    XExec_ResultClear(&result);

    /* The pending-signal check failing: the run still completes and the mask is restored */
    sigset_t before;
    pthread_sigmask(SIG_SETMASK, NULL, &before);
    fault_reset();
    g_fault.bPendingFails = XTRUE;
    CHECK(run_sh("exit 6", 0, &result) == XSTDOK && result.nExitCode == 6, "A run completes when signals cannot be checked");
    CHECK(mask_is(&before), "The caller's signal mask is restored after the run");
    XExec_ResultClear(&result);

    fault_reset();
    return 0;
}

static int XTest_run_uncollectable(void)
{
    /* A child that cannot be collected after its timeout: killed, waited for in bounded time, reported */
    fault_reset();
    g_fault.bWaitNever = XTRUE;

    char *aArgv[4];
    xexec_opts_t opts;
    sh_opts(&opts, aArgv, "exec sleep 30");

    xexec_result_t result;
    XExec_ResultInit(&result);
    uint64_t nStart = XTime_GetMonoMs();
    XSTATUS nStatus = XExec_Run(&opts, NULL, 0, 0, 100, &result);
    uint64_t nElapsed = XTime_GetMonoMs() - nStart;
    XExec_ResultClear(&result);
    fault_reset();

    /* The killed child is a zombie the faulted waitpid never reported: collect it so nothing is left behind */
    int nStatusCode = 0;
    while (__real_waitpid(-1, &nStatusCode, WNOHANG) > 0) {}

    CHECK(nStatus == XSTDERR, "A child that cannot be collected makes the run fail instead of hang");
    CHECK(nElapsed < 15000, "Waiting for an uncollectable child is bounded");
    return 0;
}

XTEST_MAIN(
    XTEST_CASE(spawn_failures),
    XTEST_CASE(nonblock_failures),
    XTEST_CASE(child_failures),
    XTEST_CASE(dupfd_failure),
    XTEST_CASE(descriptor_limits),
    XTEST_CASE(wait_failures),
    XTEST_CASE(kill_paths),
    XTEST_CASE(run_io),
    XTEST_CASE(run_uncollectable)
)
