/* libxutils: child process execution.
 *
 * Every case runs real programs through /bin/sh. The event-loop half of the
 * API (spawn, poll the caller's ends, wait) and the blocking runner are both
 * driven, and the properties a caller relies on without being able to see
 * them are asserted by value: what the child inherits (descriptors, signal
 * dispositions), what a failed start reports, and that a timeout reaches what
 * the program started, not only the program.
 */

#include "test.h"
#include "exec.h"
#include "xtime.h"
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#define EXEC_SHELL "/bin/sh"

/* A child that ends before it reaches the program leaves through _exit(), which flushes nothing - except under
   Valgrind, whose exit path runs __libc_freeres() and writes out the stdio buffer the child inherited from this
   process. The harness's own pending output would then turn up as the child's. */
static void exec_flush(void)
{
    fflush(stdout);
    fflush(stderr);
}

static int exec_run_sh(const char *pScript, const uint8_t *pInput, size_t nInput,
                       size_t nMax, int nTimeoutMs, xexec_result_t *pResult)
{
    exec_flush();
    char *aArgv[] = { (char*)"sh", (char*)"-c", (char*)pScript, NULL };
    xexec_opts_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.pPath = EXEC_SHELL;
    opts.ppArgv = aArgv;

    XExec_ResultInit(pResult);
    return XExec_Run(&opts, pInput, nInput, nMax, nTimeoutMs, pResult);
}

static int exec_output_is(const xbyte_buffer_t *pBuffer, const char *pExpected)
{
    size_t nLen = strlen(pExpected);
    return pBuffer->nUsed == nLen && (nLen == 0 || memcmp(pBuffer->pData, pExpected, nLen) == 0);
}

static int exec_output_has(const xbyte_buffer_t *pBuffer, const char *pNeedle)
{
    size_t nLen = strlen(pNeedle);
    if (pBuffer->nUsed < nLen) return 0;

    for (size_t i = 0; i + nLen <= pBuffer->nUsed; i++)
        if (memcmp(pBuffer->pData + i, pNeedle, nLen) == 0) return 1;

    return 0;
}

static int XTest_null_args(void)
{
    xexec_t exec;
    xexec_opts_t opts;
    memset(&opts, 0, sizeof(opts));

    XExec_Init(NULL);
    XExec_Close(NULL);
    XExec_CloseStdIn(NULL);
    XExec_ResultInit(NULL);
    XExec_ResultClear(NULL);

    CHECK(XExec_Spawn(NULL, &opts) == XSTDINV, "Spawning without a handle is refused");
    CHECK(XExec_Spawn(&exec, NULL) == XSTDINV, "Spawning without options is refused");
    CHECK(XExec_Spawn(&exec, &opts) == XSTDINV, "Spawning without a program is refused");

    opts.pPath = "";
    CHECK(XExec_Spawn(&exec, &opts) == XSTDINV, "Spawning an empty program path is refused");

    opts.pPath = EXEC_SHELL;
    opts.bSetIdentity = XTRUE;
    opts.nGroups = 2;
    CHECK(XExec_Spawn(&exec, &opts) == XSTDINV, "An identity with groups but no group list is refused");

    XExec_Init(&exec);
    CHECK(XExec_Wait(NULL, 0) == XSTDINV, "Waiting without a handle is refused");
    CHECK(XExec_Wait(&exec, 0) == XSTDERR, "Waiting for a child that was never started fails");
    CHECK(XExec_Kill(NULL, SIGTERM) == XSTDINV, "Killing without a handle is refused");
    CHECK(XExec_Kill(&exec, SIGTERM) == XSTDERR, "Killing a child that was never started fails");
    CHECK(exec.nStdIn == XSOCK_INVALID && exec.nStdOut == XSOCK_INVALID && exec.nStdErr == XSOCK_INVALID,
        "An initialized handle has no streams");

    xexec_result_t result;
    XExec_ResultInit(&result);
    CHECK(XExec_Run(NULL, NULL, 0, 0, 0, &result) == XSTDINV, "Running without options is refused");
    CHECK(XExec_Run(&opts, NULL, 3, 0, 0, &result) == XSTDINV, "Input with a length but no bytes is refused");
    CHECK(XExec_Run(&opts, NULL, 0, 0, 0, NULL) == XSTDINV, "Running without a result is refused");
    XExec_ResultClear(&result);
    return 0;
}

static int XTest_exit_status(void)
{
    xexec_result_t result;

    CHECK(exec_run_sh("exit 7", NULL, 0, 0, 0, &result) == XSTDOK, "A failing program still runs to completion");
    CHECK(result.nExitCode == 7 && result.nSignal == 0, "The exit status is reported as the program set it");
    XExec_ResultClear(&result);

    CHECK(exec_run_sh("exit 0", NULL, 0, 0, 0, &result) == XSTDOK, "A succeeding program runs");
    CHECK(result.nExitCode == 0 && result.nSignal == 0 && !result.bTimedOut, "Success reports exit status 0");
    XExec_ResultClear(&result);

    CHECK(exec_run_sh("kill -TERM $$", NULL, 0, 0, 0, &result) == XSTDOK, "A program ended by a signal is collected");
    CHECK(result.nSignal == SIGTERM && result.nExitCode == 128 + SIGTERM,
        "A signal ending the program is reported as the signal and as 128 + signal");
    XExec_ResultClear(&result);
    return 0;
}

static int XTest_streams(void)
{
    xexec_result_t result;

    CHECK(exec_run_sh("printf out; printf err >&2", NULL, 0, 0, 0, &result) == XSTDOK, "A program writing both streams runs");
    CHECK(exec_output_is(&result.output, "out"), "stdout is captured on its own");
    CHECK(exec_output_is(&result.errors, "err"), "stderr is captured on its own");
    XExec_ResultClear(&result);

    /* Merged: one stream, in write order */
    char *aArgv[] = { (char*)"sh", (char*)"-c", (char*)"printf 1; printf 2 >&2; printf 3", NULL };
    xexec_opts_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.pPath = EXEC_SHELL;
    opts.ppArgv = aArgv;
    opts.nFlags = XEXEC_MERGE_STDERR;

    XExec_ResultInit(&result);
    CHECK(XExec_Run(&opts, NULL, 0, 0, 0, &result) == XSTDOK, "A merged run completes");
    CHECK(exec_output_is(&result.output, "123"), "Merged stderr lands in stdout in the order it was written");
    CHECK(result.errors.nUsed == 0, "A merged run has no separate stderr");
    XExec_ResultClear(&result);

    /* A stream that is not piped is the null device, never the caller's stdio */
    xexec_t exec;
    char *aCat[] = { (char*)"sh", (char*)"-c", (char*)"cat; echo done-$?", NULL };
    opts.ppArgv = aCat;
    opts.nFlags = XEXEC_STDOUT;
    CHECK(XExec_Spawn(&exec, &opts) == XSTDOK, "A child with only stdout piped starts");
    CHECK(exec.nStdIn == XSOCK_INVALID && exec.nStdErr == XSOCK_INVALID && exec.nStdOut != XSOCK_INVALID,
        "Only the requested stream comes back to the caller");

    char sBuffer[64] = {0};
    size_t nTotal = 0;
    for (;;)
    {
        ssize_t nRead = read(exec.nStdOut, sBuffer + nTotal, sizeof(sBuffer) - 1 - nTotal);
        if (nRead <= 0) break;
        nTotal += (size_t)nRead;
    }

    CHECK(XExec_Wait(&exec, XEXEC_WAIT_FOREVER) == XSTDOK && exec.nExitCode == 0, "The child exits by itself");
    CHECK(strcmp(sBuffer, "done-0\n") == 0, "An unpiped stdin reads end of file at once instead of the caller's input");
    XExec_Close(&exec);
    return 0;
}

/* Runs in a process whose own stdin, stdout and stderr are closed, so the pipes are created at 0, 1 and 2 and
   installing one end in the child would overwrite another before it was used. Exit status is the verdict. */
static int exec_low_body(void)
{
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);

    xexec_result_t result;
    const char *pInput = "in";
    if (exec_run_sh("cat; printf out; printf err >&2", (const uint8_t*)pInput, 2, 0, 0, &result) != XSTDOK) return 10;
    if (!exec_output_is(&result.output, "inout")) return 11;
    if (!exec_output_is(&result.errors, "err")) return 12;
    XExec_ResultClear(&result);

    /* stdin not piped: the null device is opened first and lands on 0 itself, where installing it is a
       no-op that would leave it close-on-exec - and the program would start with no stdin at all */
    char *aArgv[] = { (char*)"sh", (char*)"-c", (char*)"cat; echo rc=$?", NULL };
    xexec_opts_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.pPath = EXEC_SHELL;
    opts.ppArgv = aArgv;
    opts.nFlags = XEXEC_STDOUT;

    xexec_t exec;
    if (XExec_Spawn(&exec, &opts) != XSTDOK) return 13;

    char sBuffer[64] = {0};
    size_t nTotal = 0;
    for (;;)
    {
        ssize_t nRead = read(exec.nStdOut, sBuffer + nTotal, sizeof(sBuffer) - 1 - nTotal);
        if (nRead <= 0) break;
        nTotal += (size_t)nRead;
    }

    XExec_Wait(&exec, XEXEC_WAIT_FOREVER);
    XExec_Close(&exec);
    return strcmp(sBuffer, "rc=0\n") == 0 ? 0 : 14;
}

static int XTest_low_descriptors(void)
{
    fflush(stdout);
    fflush(stderr);

    pid_t nPid = fork();
    CHECK(nPid >= 0, "A helper process");
    if (nPid == 0) XTEST_EXIT(exec_low_body());

    int nStatus = 0;
    CHECK(waitpid(nPid, &nStatus, 0) == nPid, "The helper is collected");
    CHECK(WIFEXITED(nStatus) && WEXITSTATUS(nStatus) == 0,
        "A caller without its own stdio still gives the child the right stream on each descriptor");
    return 0;
}

static int XTest_input(void)
{
    xexec_result_t result;
    const char *pText = "line one\nline two\n";

    CHECK(exec_run_sh("cat", (const uint8_t*)pText, strlen(pText), 0, 0, &result) == XSTDOK, "A program reading stdin runs");
    CHECK(exec_output_is(&result.output, pText), "The input reaches the program, followed by end of file");
    XExec_ResultClear(&result);

    /* More than a pipe holds, so the writes and reads have to interleave */
    size_t nBig = 1024 * 1024 + 17;
    uint8_t *pBig = (uint8_t*)malloc(nBig);
    CHECK(pBig != NULL, "A large input buffer");
    for (size_t i = 0; i < nBig; i++) pBig[i] = (uint8_t)(i * 31 + 7);

    int nStatus = exec_run_sh("cat", pBig, nBig, 0, 0, &result);
    int bSame = result.output.nUsed == nBig && memcmp(result.output.pData, pBig, nBig) == 0;
    free(pBig);
    CHECK(nStatus == XSTDOK && bSame, "An input larger than a pipe buffer comes back intact");
    XExec_ResultClear(&result);

    /* A program that never reads its input: the writes fail, and the caller is not ended by SIGPIPE */
    uint8_t sJunk[256 * 1024];
    memset(sJunk, 'x', sizeof(sJunk));
    struct sigaction oldAction, defAction;
    memset(&defAction, 0, sizeof(defAction));
    defAction.sa_handler = SIG_DFL;
    sigaction(SIGPIPE, &defAction, &oldAction);

    nStatus = exec_run_sh("exit 3", sJunk, sizeof(sJunk), 0, 0, &result);
    sigaction(SIGPIPE, &oldAction, NULL);
    CHECK(nStatus == XSTDOK && result.nExitCode == 3, "Input a program does not read neither hangs nor kills the caller");

    sigset_t pending;
    sigemptyset(&pending);
    sigpending(&pending);
    CHECK(!sigismember(&pending, SIGPIPE), "A SIGPIPE held back during the run is not left pending afterwards");
    XExec_ResultClear(&result);
    return 0;
}

static int XTest_start_failures(void)
{
    xexec_result_t result;
    xexec_opts_t opts;
    memset(&opts, 0, sizeof(opts));

    exec_flush();

    /* Missing program: the child reports it the way a shell does, and the caller is not held up by it */
    opts.pPath = "/nonexistent/xexec-program";
    XExec_ResultInit(&result);
    CHECK(XExec_Run(&opts, NULL, 0, 0, 0, &result) == XSTDOK, "A missing program still yields a child to collect");
    CHECK(result.nExitCode == XEXEC_EXIT_NOTFOUND, "A missing program exits with 127");
    CHECK(exec_output_has(&result.errors, "cannot find the program"), "A missing program says so on stderr");
    XExec_ResultClear(&result);

    /* Present but not executable */
    char sDir[] = "/tmp/xexec_regression.XXXXXX";
    CHECK(mkdtemp(sDir) != NULL, "A scratch directory");

    char sFile[256];
    snprintf(sFile, sizeof(sFile), "%s/plain.txt", sDir);
    FILE *pFile = fopen(sFile, "w");
    CHECK(pFile != NULL, "A plain file");
    fputs("not a program\n", pFile);
    fclose(pFile);
    chmod(sFile, 0644);

    opts.pPath = sFile;
    XExec_ResultInit(&result);
    int nStatus = XExec_Run(&opts, NULL, 0, 0, 0, &result);
    int nCode = result.nExitCode;
    int bSaid = exec_output_has(&result.errors, "cannot execute the program");
    XExec_ResultClear(&result);
    unlink(sFile);

    CHECK(nStatus == XSTDOK && nCode == XEXEC_EXIT_NOEXEC, "A file that is not executable exits with 126");
    CHECK(bSaid, "A file that is not executable says so on stderr");

    /* A path through something that is not a directory is as missing as a path to nothing */
    char sThrough[300];
    snprintf(sThrough, sizeof(sThrough), "%s/plain.txt/program", sDir);
    FILE *pAgain = fopen(sFile, "w");
    CHECK(pAgain != NULL, "The plain file again");
    fclose(pAgain);
    opts.pPath = sThrough;
    XExec_ResultInit(&result);
    nStatus = XExec_Run(&opts, NULL, 0, 0, 0, &result);
    nCode = result.nExitCode;
    bSaid = exec_output_has(&result.errors, "cannot find the program");
    XExec_ResultClear(&result);
    unlink(sFile);
    CHECK(nStatus == XSTDOK && nCode == XEXEC_EXIT_NOTFOUND && bSaid, "A path through a file is not found, with 127");

    /* A working directory that cannot be entered */
    char *aArgv[] = { (char*)"sh", (char*)"-c", (char*)"echo ran", NULL };
    char sMissing[300];
    snprintf(sMissing, sizeof(sMissing), "%s/missing", sDir);
    opts.pPath = EXEC_SHELL;
    opts.ppArgv = aArgv;
    opts.pWorkDir = sMissing;

    XExec_ResultInit(&result);
    nStatus = XExec_Run(&opts, NULL, 0, 0, 0, &result);
    nCode = result.nExitCode;
    bSaid = exec_output_has(&result.errors, "cannot change to the working directory");
    int bRan = exec_output_has(&result.output, "ran");
    XExec_ResultClear(&result);
    rmdir(sDir);

    CHECK(nStatus == XSTDOK && nCode == XEXEC_EXIT_NOTFOUND, "A missing working directory exits with 127");
    CHECK(bSaid && !bRan, "A missing working directory is reported and the program never runs elsewhere");

    /* With a fallback the program runs there instead, and only there */
    char *aPwd[] = { (char*)"sh", (char*)"-c", (char*)"pwd -P", NULL };
    opts.ppArgv = aPwd;
    opts.pWorkDirFallback = "/";

    XExec_ResultInit(&result);
    nStatus = XExec_Run(&opts, NULL, 0, 0, 0, &result);
    int bFallback = exec_output_is(&result.output, "/\n") && result.nExitCode == 0;
    XExec_ResultClear(&result);
    CHECK(nStatus == XSTDOK && bFallback, "A working directory that cannot be entered falls back when a fallback is given");

    /* A fallback that cannot be entered either still fails */
    opts.pWorkDirFallback = sMissing;
    XExec_ResultInit(&result);
    nStatus = XExec_Run(&opts, NULL, 0, 0, 0, &result);
    nCode = result.nExitCode;
    XExec_ResultClear(&result);
    CHECK(nStatus == XSTDOK && nCode == XEXEC_EXIT_NOTFOUND, "A fallback that cannot be entered either fails the start");
    return 0;
}

static int XTest_workdir_env(void)
{
    char sDir[] = "/tmp/xexec_regression.XXXXXX";
    CHECK(mkdtemp(sDir) != NULL, "A scratch directory");

    char sReal[PATH_MAX];
    CHECK(realpath(sDir, sReal) != NULL, "The scratch directory resolves");

    char *aArgv[] = { (char*)"sh", (char*)"-c", (char*)"pwd -P; echo \"[$XEXEC_ONLY][$XEXEC_INHERITED]\"", NULL };
    char *aEnv[] = { (char*)"XEXEC_ONLY=given", (char*)"PATH=/usr/bin:/bin", NULL };

    xexec_opts_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.pPath = EXEC_SHELL;
    opts.ppArgv = aArgv;
    opts.ppEnv = aEnv;
    opts.pWorkDir = sDir;

    setenv("XEXEC_INHERITED", "parent", 1);

    xexec_result_t result;
    XExec_ResultInit(&result);
    int nStatus = XExec_Run(&opts, NULL, 0, 0, 0, &result);

    char sExpected[PATH_MAX + 64];
    snprintf(sExpected, sizeof(sExpected), "%s\n[given][]\n", sReal);
    int bGiven = exec_output_is(&result.output, sExpected);
    XExec_ResultClear(&result);

    /* No environment given: the caller's is inherited */
    opts.ppEnv = NULL;
    XExec_ResultInit(&result);
    nStatus = nStatus == XSTDOK ? XExec_Run(&opts, NULL, 0, 0, 0, &result) : nStatus;

    snprintf(sExpected, sizeof(sExpected), "%s\n[][parent]\n", sReal);
    int bInherited = exec_output_is(&result.output, sExpected);
    XExec_ResultClear(&result);

    unsetenv("XEXEC_INHERITED");
    rmdir(sDir);

    CHECK(nStatus == XSTDOK, "Both runs complete");
    CHECK(bGiven, "The child starts in the given directory with exactly the given environment");
    CHECK(bInherited, "Without an environment the child inherits the caller's");
    return 0;
}

static int XTest_timeout(void)
{
    char sDir[] = "/tmp/xexec_regression.XXXXXX";
    CHECK(mkdtemp(sDir) != NULL, "A scratch directory");

    char sPidFile[256], sScript[600];
    snprintf(sPidFile, sizeof(sPidFile), "%s/child.pid", sDir);
    snprintf(sScript, sizeof(sScript), "sleep 30 & echo $! > %s; wait", sPidFile);

    uint64_t nStartMs = XTime_GetMonoMs();
    xexec_result_t result;
    int nStatus = exec_run_sh(sScript, NULL, 0, 0, 300, &result);
    uint64_t nElapsedMs = XTime_GetMonoMs() - nStartMs;

    int bTimedOut = result.bTimedOut;
    int nSignal = result.nSignal;
    XExec_ResultClear(&result);

    CHECK(nStatus == XSTDOK, "A timed-out run is still collected");
    CHECK(bTimedOut && nSignal == SIGKILL, "Outliving the timeout ends the program with SIGKILL");
    CHECK(nElapsedMs < 5000, "The timeout is honoured, not the program's own duration");

    /* The background child was in the same group, so the kill reached it as well */
    FILE *pFile = fopen(sPidFile, "r");
    CHECK(pFile != NULL, "The background child recorded its pid");
    int nChild = 0;
    int nScanned = fscanf(pFile, "%d", &nChild);
    fclose(pFile);
    unlink(sPidFile);
    rmdir(sDir);
    CHECK(nScanned == 1 && nChild > 0, "The background child's pid can be read");

    int bGone = 0;
    for (int i = 0; i < 200 && !bGone; i++)
    {
        bGone = kill(nChild, 0) != 0 && errno == ESRCH;
        if (!bGone) usleep(10000);
    }

    CHECK(bGone, "A timeout also ends what the program started");
    return 0;
}

static int XTest_output_cap(void)
{
    xexec_result_t result;

    /* 200 KB on each stream against a 1000-byte cap */
    const char *pScript = "i=0; while [ $i -lt 2000 ]; do "
        "echo 0123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789; "
        "echo 0123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789 >&2; "
        "i=$((i+1)); done; exit 5";

    CHECK(exec_run_sh(pScript, NULL, 0, 1000, 0, &result) == XSTDOK, "A run over its output cap completes");
    CHECK(result.output.nUsed == 1000 && result.errors.nUsed == 1000, "Each stream keeps exactly the cap");
    CHECK(result.bTruncated, "Dropped output is reported");
    CHECK(result.nExitCode == 5, "Output beyond the cap is drained, so the program runs to its own end");
    XExec_ResultClear(&result);

    CHECK(exec_run_sh("printf abc", NULL, 0, 3, 0, &result) == XSTDOK, "A run exactly at its cap completes");
    CHECK(exec_output_is(&result.output, "abc") && !result.bTruncated, "Output that fits the cap is not reported cut");
    XExec_ResultClear(&result);
    return 0;
}

static int XTest_grace(void)
{
    /* The program exits at once but leaves a child holding its stdout open */
    uint64_t nStartMs = XTime_GetMonoMs();
    xexec_result_t result;
    int nStatus = exec_run_sh("sleep 3 & echo started", NULL, 0, 0, 0, &result);
    uint64_t nElapsedMs = XTime_GetMonoMs() - nStartMs;

    int bOutput = exec_output_is(&result.output, "started\n");
    XExec_ResultClear(&result);

    CHECK(nStatus == XSTDOK && bOutput, "The program's own output is captured");
    CHECK(nElapsedMs < 2000, "A child left holding the output open is not waited for");
    return 0;
}

static int XTest_grace_stderr(void)
{
    /* stdout ends first and something the program started keeps stderr open: the exit is not held up either */
    uint64_t nStartMs = XTime_GetMonoMs();
    xexec_result_t result;
    int nStatus = exec_run_sh("exec 1>&-; sleep 3 >/dev/null & exit 0", NULL, 0, 0, 0, &result);
    uint64_t nElapsedMs = XTime_GetMonoMs() - nStartMs;
    int nCode = result.nExitCode;
    XExec_ResultClear(&result);

    CHECK(nStatus == XSTDOK && nCode == 0, "A program that closed stdout first is collected");
    CHECK(nElapsedMs < 2000, "A child keeping stderr open alone is not waited for either");
    return 0;
}

static int XTest_descriptors(void)
{
    /* A descriptor without close-on-exec, at a number the shell can name */
    int nFd = open("/dev/null", O_RDONLY);
    CHECK(nFd >= 0, "A descriptor to leak");
    CHECK(dup2(nFd, 9) == 9, "The descriptor at 9");
    if (nFd != 9) close(nFd);

    xexec_result_t result;
    int nStatus = exec_run_sh("if true <&9 2>/dev/null; then echo open; else echo closed; fi", NULL, 0, 0, 0, &result);
    int bClosed = exec_output_is(&result.output, "closed\n");
    XExec_ResultClear(&result);

    /* The same descriptor reaches a child of the shell itself, which proves the check can see one */
    int nControl = system("if true <&9 2>/dev/null; then exit 0; else exit 1; fi");
    close(9);

    CHECK(nStatus == XSTDOK, "The descriptor check runs");
    CHECK(WIFEXITED(nControl) && WEXITSTATUS(nControl) == 0, "The check sees a descriptor that is open");
    CHECK(bClosed, "The caller's other descriptors are not inherited");
    return 0;
}

static int XTest_signals(void)
{
#ifdef __linux__
    /* Ignored and blocked in the caller: neither may reach the program */
    struct sigaction oldAction, ignAction;
    memset(&ignAction, 0, sizeof(ignAction));
    ignAction.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &ignAction, &oldAction);

    sigset_t blocked, oldMask;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &blocked, &oldMask);

    char *aArgv[] = { (char*)"cat", (char*)"/proc/self/status", NULL };
    xexec_opts_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.pPath = "/bin/cat";
    opts.ppArgv = aArgv;

    xexec_result_t result;
    XExec_ResultInit(&result);
    int nStatus = XExec_Run(&opts, NULL, 0, 0, 0, &result);

    pthread_sigmask(SIG_SETMASK, &oldMask, NULL);
    sigaction(SIGPIPE, &oldAction, NULL);

    XByteBuffer_AddByte(&result.output, '\0');
    const char *pStatus = (const char*)result.output.pData;
    const char *pIgn = pStatus != NULL ? strstr(pStatus, "SigIgn:\t") : NULL;
    const char *pBlk = pStatus != NULL ? strstr(pStatus, "SigBlk:\t") : NULL;

    unsigned long long nIgn = pIgn != NULL ? strtoull(pIgn + 8, NULL, 16) : ~0ULL;
    unsigned long long nBlk = pBlk != NULL ? strtoull(pBlk + 8, NULL, 16) : ~0ULL;
    XExec_ResultClear(&result);

    CHECK(nStatus == XSTDOK && pIgn != NULL && pBlk != NULL, "The child's signal state can be read");
    CHECK(!(nIgn & (1ULL << (SIGPIPE - 1))), "A signal the caller ignores is at its default in the child");
    CHECK(!(nBlk & (1ULL << (SIGUSR1 - 1))), "A signal the caller blocks is not blocked in the child");
    return 0;
#else
    return 77;
#endif
}

static int XTest_event_loop(void)
{
    /* The event-loop half: non-blocking ends polled by the caller, the group signalled while running */
    char *aArgv[] = { (char*)"sh", (char*)"-c", (char*)"read line; echo \"got:$line\"; echo warn >&2; sleep 30", NULL };
    xexec_opts_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.pPath = EXEC_SHELL;
    opts.ppArgv = aArgv;
    opts.nFlags = XEXEC_STDIN | XEXEC_STDOUT | XEXEC_STDERR | XEXEC_NONBLOCK | XEXEC_GROUP;

    xexec_t exec;
    CHECK(XExec_Spawn(&exec, &opts) == XSTDOK, "A child with every stream piped starts");
    CHECK(exec.nPid > 0 && exec.bGroup, "The child is running in a group of its own");
    CHECK(fcntl(exec.nStdOut, F_GETFL) & O_NONBLOCK, "The caller's ends do not block");
    CHECK(fcntl(exec.nStdOut, F_GETFD) & FD_CLOEXEC, "The caller's ends are not passed on to other programs");

    char sNothing[8];
    CHECK(read(exec.nStdOut, sNothing, sizeof(sNothing)) < 0 && errno == EAGAIN, "An empty stream reports would-block");
    CHECK(XExec_Wait(&exec, 0) == XSTDNON, "A running child is reported as running");
    CHECK(XExec_Wait(&exec, 50) == XSTDNON, "A bounded wait returns while the child still runs");

    CHECK(write(exec.nStdIn, "hello\n", 6) == 6, "Input is written to the child");
    XExec_CloseStdIn(&exec);
    CHECK(exec.nStdIn == XSOCK_INVALID, "Closing stdin releases the caller's end");

    char sOut[64] = {0}, sErr[64] = {0};
    size_t nOut = 0, nErr = 0;
    uint64_t nDeadlineMs = XTime_GetMonoMs() + 5000;

    while ((strchr(sOut, '\n') == NULL || strchr(sErr, '\n') == NULL) && XTime_GetMonoMs() < nDeadlineMs)
    {
        struct pollfd fds[2];
        fds[0].fd = exec.nStdOut;
        fds[0].events = POLLIN;
        fds[1].fd = exec.nStdErr;
        fds[1].events = POLLIN;
        if (poll(fds, 2, 100) <= 0) continue;

        if (fds[0].revents)
        {
            ssize_t nRead = read(exec.nStdOut, sOut + nOut, sizeof(sOut) - 1 - nOut);
            if (nRead > 0) nOut += (size_t)nRead;
        }

        if (fds[1].revents)
        {
            ssize_t nRead = read(exec.nStdErr, sErr + nErr, sizeof(sErr) - 1 - nErr);
            if (nRead > 0) nErr += (size_t)nRead;
        }
    }

    CHECK(strcmp(sOut, "got:hello\n") == 0, "The child's answer arrives on stdout");
    CHECK(strcmp(sErr, "warn\n") == 0, "The child's warning arrives on stderr");

    CHECK(XExec_Kill(&exec, SIGTERM) == XSTDOK, "A running group can be signalled");
    CHECK(XExec_Wait(&exec, 5000) == XSTDOK, "The signalled child is collected");
    CHECK(exec.bExited && exec.nSignal == SIGTERM, "The signal that ended the child is reported");
    CHECK(XExec_Kill(&exec, SIGTERM) == XSTDNON, "A collected child is never signalled again");
    CHECK(XExec_Wait(&exec, 0) == XSTDOK, "Waiting again reports the same exit");

    XExec_Close(&exec);
    CHECK(exec.nStdOut == XSOCK_INVALID && exec.nStdErr == XSOCK_INVALID, "Closing releases every stream");
    XExec_Close(&exec);
    return 0;
}

static int XTest_identity(void)
{
    char *aArgv[] = { (char*)"sh", (char*)"-c", (char*)"id -u; id -g", NULL };
    xexec_opts_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.pPath = EXEC_SHELL;
    opts.ppArgv = aArgv;
    opts.bSetIdentity = XTRUE;

    xexec_result_t result;
    exec_flush();

    if (getuid() != 0)
    {
        /* Not privileged: taking another identity fails in the child, and the program never runs */
        opts.nUid = 0;
        opts.nGid = 0;

        XExec_ResultInit(&result);
        CHECK(XExec_Run(&opts, NULL, 0, 0, 0, &result) == XSTDOK, "A child that cannot switch users is collected");
        CHECK(result.nExitCode == XEXEC_EXIT_NOTFOUND, "A failed identity switch exits with 127");
        CHECK(exec_output_has(&result.errors, "cannot switch to the requested user"), "A failed switch says so");
        CHECK(result.output.nUsed == 0, "The program never runs under the wrong identity");
        XExec_ResultClear(&result);
        return 0;
    }

    gid_t aGroups[1] = { 65534 };
    opts.nUid = 65534;
    opts.nGid = 65534;
    opts.pGroups = aGroups;
    opts.nGroups = 1;

    XExec_ResultInit(&result);
    CHECK(XExec_Run(&opts, NULL, 0, 0, 0, &result) == XSTDOK, "A child switching users is collected");
    CHECK(exec_output_is(&result.output, "65534\n65534\n"), "The program runs with the requested uid and gid");
    XExec_ResultClear(&result);
    return 0;
}

static int XTest_stream_choices(void)
{
    /* Non-blocking with only some streams piped: the rest are the null device, and nothing is switched on them */
    char *aArgv[] = { (char*)"sh", (char*)"-c", (char*)"echo out; echo err >&2", NULL };
    xexec_opts_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.pPath = EXEC_SHELL;
    opts.ppArgv = aArgv;
    opts.nFlags = XEXEC_STDERR | XEXEC_NONBLOCK;

    xexec_t exec;
    CHECK(XExec_Spawn(&exec, &opts) == XSTDOK, "A child with only stderr piped starts");
    CHECK(exec.nStdIn == XSOCK_INVALID && exec.nStdOut == XSOCK_INVALID && exec.nStdErr != XSOCK_INVALID,
        "Only stderr comes back");
    CHECK(fcntl(exec.nStdErr, F_GETFL) & O_NONBLOCK, "The one piped stream is non-blocking");

    char sBuffer[64] = {0};
    size_t nTotal = 0;
    for (int i = 0; i < 500 && strchr(sBuffer, '\n') == NULL; i++)
    {
        ssize_t nRead = read(exec.nStdErr, sBuffer + nTotal, sizeof(sBuffer) - 1 - nTotal);
        if (nRead > 0) nTotal += (size_t)nRead;
        else usleep(10000);
    }

    CHECK(strcmp(sBuffer, "err\n") == 0, "stderr arrives and stdout went to the null device");
    CHECK(XExec_Wait(&exec, 5000) == XSTDOK && exec.nExitCode == 0, "The child exits");
    XExec_Close(&exec);

    /* Merged without stderr asked for: stderr is the null device, never stdout */
    char *aBoth[] = { (char*)"sh", (char*)"-c", (char*)"echo out; echo err >&2", NULL };
    opts.ppArgv = aBoth;
    opts.nFlags = XEXEC_STDOUT | XEXEC_MERGE_STDERR;

    CHECK(XExec_Spawn(&exec, &opts) == XSTDOK, "A merged child without stderr asked for starts");
    memset(sBuffer, 0, sizeof(sBuffer));
    nTotal = 0;
    for (;;)
    {
        ssize_t nRead = read(exec.nStdOut, sBuffer + nTotal, sizeof(sBuffer) - 1 - nTotal);
        if (nRead <= 0) break;
        nTotal += (size_t)nRead;
    }

    CHECK(XExec_Wait(&exec, 5000) == XSTDOK, "The merged child exits");
    XExec_Close(&exec);
    CHECK(strcmp(sBuffer, "out\n") == 0, "Merging without stderr asked for keeps stderr out of stdout");

    /* An identity with no supplementary groups is valid as it stands */
    opts.nFlags = 0;
    opts.bSetIdentity = XTRUE;
    opts.nGroups = 0;
    opts.pGroups = NULL;
    opts.nUid = getuid();
    opts.nGid = getgid();
    CHECK(XExec_Spawn(&exec, &opts) == XSTDOK, "An identity without supplementary groups is accepted");
    CHECK(XExec_Wait(&exec, 5000) == XSTDOK, "The child with that identity is collected");
    XExec_Close(&exec);
    return 0;
}

static int XTest_lifecycle_edges(void)
{
    char *aArgv[] = { (char*)"sh", (char*)"-c", (char*)"exec sleep 30", NULL };
    xexec_opts_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.pPath = EXEC_SHELL;
    opts.ppArgv = aArgv;

    /* Without a group, a kill reaches the child alone */
    xexec_t exec;
    CHECK(XExec_Spawn(&exec, &opts) == XSTDOK && !exec.bGroup, "A child without a group of its own");
    CHECK(XExec_Kill(&exec, SIGTERM) == XSTDOK, "The lone child is signalled");
    CHECK(XExec_Wait(&exec, 5000) == XSTDOK && exec.nSignal == SIGTERM, "The lone child ends by that signal");
    XExec_Close(&exec);

    /* Collected by someone else: the wait fails instead of pretending */
    CHECK(XExec_Spawn(&exec, &opts) == XSTDOK, "A child someone else collects");
    kill(exec.nPid, SIGKILL);
    int nStatus = 0;
    CHECK(waitpid(exec.nPid, &nStatus, 0) == exec.nPid, "The child is collected behind the handle's back");
    CHECK(XExec_Wait(&exec, 0) == XSTDERR && !exec.bExited, "A child that is no longer ours to wait for is an error");
    XExec_Close(&exec);

    /* A signal sent the instant the child exists, before it can have reached setsid(), is never lost */
    opts.nFlags = XEXEC_GROUP;
    for (int i = 0; i < 50; i++)
    {
        CHECK(XExec_Spawn(&exec, &opts) == XSTDOK, "A child to signal at once");
        CHECK(XExec_Kill(&exec, SIGTERM) == XSTDOK, "The signal is sent at once");
        CHECK(XExec_Wait(&exec, 5000) == XSTDOK && exec.nSignal == SIGTERM,
            "A signal sent before the child set up its group still ends it, by default");
        XExec_Close(&exec);
    }

    /* SIGPIPE already pending before a run stays pending: only one the run itself raised is taken */
    sigset_t pipeSet, savedMask;
    sigemptyset(&pipeSet);
    sigaddset(&pipeSet, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &pipeSet, &savedMask);
    raise(SIGPIPE);

    xexec_result_t result;
    CHECK(exec_run_sh("exit 0", NULL, 0, 0, 0, &result) == XSTDOK, "A run with SIGPIPE already pending completes");
    XExec_ResultClear(&result);

    sigset_t pending;
    sigemptyset(&pending);
    sigpending(&pending);
    int bStillPending = sigismember(&pending, SIGPIPE);
    int nTaken = 0;
    if (bStillPending) sigwait(&pipeSet, &nTaken);
    pthread_sigmask(SIG_SETMASK, &savedMask, NULL);
    CHECK(bStillPending, "A SIGPIPE the caller had pending before the run is left to it");
    return 0;
}

/* The caller of a program started with XEXEC_DIE_WITH_PARENT goes away: the program goes with it. */
static int XTest_die_with_parent(void)
{
#ifdef __linux__
    int nPipe[2];
    CHECK(pipe(nPipe) == 0, "A pipe to hear the program's pid on");
    exec_flush();

    pid_t nHelper = fork();
    CHECK(nHelper >= 0, "A helper that starts the program and leaves");
    if (nHelper == 0)
    {
        char *aArgv[] = { (char*)"sh", (char*)"-c", (char*)"exec sleep 30", NULL };
        xexec_opts_t opts;
        memset(&opts, 0, sizeof(opts));
        opts.pPath = EXEC_SHELL;
        opts.ppArgv = aArgv;
        opts.nFlags = XEXEC_DIE_WITH_PARENT;

        xexec_t exec;
        int nPid = XExec_Spawn(&exec, &opts) == XSTDOK ? (int)exec.nPid : -1;
        ssize_t nDone = write(nPipe[1], &nPid, sizeof(nPid));
        (void)nDone;
        usleep(200000);     /* Long enough for the program to have started */
        XTEST_EXIT(0);
    }

    close(nPipe[1]);
    int nProgram = -1;
    CHECK(read(nPipe[0], &nProgram, sizeof(nProgram)) == (ssize_t)sizeof(nProgram) && nProgram > 0,
        "The helper started the program");
    close(nPipe[0]);
    CHECK(waitpid(nHelper, NULL, 0) == nHelper, "The helper has left");

    int bGone = 0;
    for (int i = 0; i < 300 && !bGone; i++)
    {
        bGone = kill(nProgram, 0) != 0 && errno == ESRCH;
        if (!bGone) usleep(10000);
    }

    CHECK(bGone, "A program tied to its caller does not outlive it");
    return 0;
#else
    return 77;
#endif
}

XTEST_MAIN(
    XTEST_CASE(null_args),
    XTEST_CASE(exit_status),
    XTEST_CASE(streams),
    XTEST_CASE(low_descriptors),
    XTEST_CASE(input),
    XTEST_CASE(start_failures),
    XTEST_CASE(workdir_env),
    XTEST_CASE(timeout),
    XTEST_CASE(output_cap),
    XTEST_CASE(grace),
    XTEST_CASE(grace_stderr),
    XTEST_CASE(descriptors),
    XTEST_CASE(signals),
    XTEST_CASE(event_loop),
    XTEST_CASE(identity),
    XTEST_CASE(stream_choices),
    XTEST_CASE(lifecycle_edges),
    XTEST_CASE(die_with_parent)
)
