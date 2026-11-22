# exec.c

## Purpose

Starts child processes with their standard streams piped back to the caller, in two shapes:

- an event-loop shape (`XExec_Spawn`, `XExec_Wait`, `XExec_Kill`, `XExec_Close`) whose pipe ends a poll loop or
  `xapi_t` can wait on, and which never blocks the caller on the child;
- a blocking runner (`XExec_Run`) that feeds input, captures both outputs up to a cap and enforces a timeout.

## Platform Notes

- POSIX: `fork()` + `execve()`. The child makes system calls only, so a threaded caller is safe: everything the
  child needs (argument and environment vectors, the identity to take, the null device) is prepared before the fork.
  The calling thread blocks every signal across the `fork()` and restores its mask right after, so the child cannot
  run one of the caller's handlers before it has reset them all.
- Windows: `CreateProcessA` with an explicit handle list, so the child inherits its three streams and nothing else.
  WSAPoll cannot wait on a pipe, so each piped stream reaches the caller as one end of a socket pair
  (`XSock_CreatePair`), bridged to the child's pipe by a thread per stream. With `XEXEC_GROUP` or
  `XEXEC_DIE_WITH_PARENT` the child is created suspended and joins a job object before it runs; with `XEXEC_GROUP`
  the job is what a kill reaches.
- There is no `PATH` search on either platform: `pPath` is the program's full path.

## What the Child Inherits

- Streams that are not piped are connected to the null device, never to the caller's own stdin, stdout or stderr.
- POSIX: every descriptor from 3 up is closed (`close_range` where the kernel has it), whether or not it was marked
  close-on-exec. Every signal is reset to its default disposition and the signal mask is cleared, so a caller that
  ignores `SIGPIPE` does not hand that to the program.
- The environment is `ppEnv` exactly, or the caller's when `ppEnv` is `NULL`.

## Start Failures

On Windows `CreateProcess` checks the working directory itself, so a directory that cannot be entered (and no
usable fallback) makes `XExec_Spawn` return `XSTDERR` rather than producing a child that exits with 127.

`XExec_Spawn` returns `XSTDOK` as soon as the child exists. It does not wait for the child to reach the program,
so a working directory on a dead network mount stalls the child and not the caller. A child that cannot start
writes one line to its own stderr and exits the way a shell reports the same failures:

| Exit code | stderr | Cause |
|-----------|--------|-------|
| 127 (`XEXEC_EXIT_NOTFOUND`) | `cannot find the program` | `pPath` does not exist |
| 126 (`XEXEC_EXIT_NOEXEC`) | `cannot execute the program` | exists, cannot be executed |
| 127 | `cannot start a new process group` | `XEXEC_GROUP` and `setsid()` failed |
| 127 | `cannot switch to the requested user` | `bSetIdentity` and the change was refused |
| 127 | `cannot tie the program to its parent` | `XEXEC_DIE_WITH_PARENT` and the death signal could not be set, or the caller was already gone (Linux) |
| 127 | `cannot change to the working directory` | neither `pWorkDir` nor `pWorkDirFallback` can be entered (checked as the target identity) |

## API Reference

### Options

`xexec_opts_t`:

- `pPath`: program to run (full path).
- `ppArgv`: `NULL`-terminated, `argv[0]` included; `NULL` runs `pPath` with no arguments.
- `ppEnv`: `NULL`-terminated `NAME=value` list; `NULL` inherits.
- `pWorkDir`: working directory; `NULL` keeps the caller's.
- `pWorkDirFallback`: tried when `pWorkDir` cannot be entered (as login falls back to `/` for a missing home);
  `NULL` makes that a start failure. Leave it unset when running in the wrong directory would be harmful.
- `nFlags`:
  - `XEXEC_STDIN`, `XEXEC_STDOUT`, `XEXEC_STDERR`: pipe that stream back to the caller.
  - `XEXEC_MERGE_STDERR`: stderr goes into the stdout pipe, one stream in write order.
  - `XEXEC_GROUP`: the child leads its own process group (POSIX `setsid`) or job (Windows). `XExec_Kill` then
    reaches everything it starts, and Ctrl-C at the caller's terminal does not reach it.
  - `XEXEC_NONBLOCK`: the caller's ends are non-blocking.
  - `XEXEC_DIE_WITH_PARENT`: the program does not outlive its caller. Linux: `PR_SET_PDEATHSIG` with `SIGKILL`,
    set after the identity change (which would clear it), so the program is killed when the thread that started it
    exits - spawn from a thread that lives as long as the program should - and never starts when that thread is
    already gone. Windows: the job is `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`, so the program and everything in its job
    go when the last handle to the job closes: when the caller exits, or on `XExec_Close`. A job that cannot be
    created or joined fails the spawn (`XSTDERR`) before the child has run. Other systems: no effect.
- Windows only: `pCmdLine`, used verbatim instead of quoting `ppArgv` (`cmd.exe /c` has its own parsing rules).
- POSIX only: `bSetIdentity`, `nUid`, `nGid`, `pGroups`, `nGroups`. The child sets the supplementary groups, then
  the gid, then the uid, before it changes directory. Resolve the account before the call: a name-service lookup
  is not safe in the child of a threaded process.

### `void XExec_Init(xexec_t *pExec)`

- Resets the handle: no streams (`XSOCK_INVALID`), no child (`nPid == XSTDERR`).

### `XSTATUS XExec_Spawn(xexec_t *pExec, const xexec_opts_t *pOpts)`

- Returns:
  - `XSTDOK` when the child exists; the caller's ends are in `nStdIn`, `nStdOut`, `nStdErr`.
  - `XSTDINV` for missing arguments, an empty `pPath`, or groups without a group list.
  - `XSTDERR` when pipes, the null device or the process could not be created; `errno` / `GetLastError()` says why.

### `XSTATUS XExec_Wait(xexec_t *pExec, int nTimeoutMs)`

- `nTimeoutMs`: `0` only checks, `XEXEC_WAIT_FOREVER` blocks.
- Returns `XSTDOK` once the child has exited (`bExited`, `nExitCode`, `nSignal` set; repeated calls report the same
  exit), `XSTDNON` while it runs, `XSTDERR` when there is no child to wait for.
- `nExitCode` is the exit status, or `128 + signal` for a child ended by a signal; `nSignal` is that signal (`0`
  for a normal exit).

### `XSTATUS XExec_Kill(xexec_t *pExec, int nSignal)`

- POSIX: sends `nSignal` to the child, or its whole group with `XEXEC_GROUP`. The child has not been collected yet,
  so its id cannot have been reused. A child so new that it has not reached `setsid()` has no group yet; it is
  signalled directly then, and since it holds every signal blocked until it has reset them, it takes the signal at
  its default disposition.
- Windows: terminates the job (with `XEXEC_GROUP`) or the process with exit code `128 + nSignal`, which
  `XExec_Wait` then reports as `nSignal`. Does not wait for the termination to complete.
- Returns `XSTDOK` when sent, `XSTDNON` when the child was already collected (it is never signalled again),
  `XSTDERR` otherwise.

### `void XExec_CloseStdIn(xexec_t *pExec)`

- Closes the caller's end of stdin; the child reads end of file.

### `void XExec_Close(xexec_t *pExec)`

- Closes the caller's ends and, on Windows, stops the pump threads and releases the process and job handles.
- Neither waits for nor kills the child: kill and collect it first when it must be gone.
- A stream handed to an owner that closes it (an `xapi_t` endpoint, for one) must be set to `XSOCK_INVALID` first.

### `XSTATUS XExec_Run(...)`

```c
XSTATUS XExec_Run(const xexec_opts_t *pOpts, const uint8_t *pInput, size_t nInput,
                  size_t nMaxOutput, int nTimeoutMs, xexec_result_t *pResult);
```

- Always pipes all three streams, makes them non-blocking and runs the child in its own group with
  `XEXEC_DIE_WITH_PARENT` (`XEXEC_MERGE_STDERR` is honoured).
- Writes `pInput`, then closes stdin; a child that stops reading ends the writing without a `SIGPIPE` reaching
  the caller (held back for the calling thread during the run and taken if it came).
- Keeps the first `nMaxOutput` bytes of each stream (`0`: everything) and keeps draining past that, so a chatty
  program is never stopped by a full pipe. `bTruncated` reports dropped output.
- `nTimeoutMs > 0`: a child that outlives it is killed with `SIGKILL` along with its group; `bTimedOut` is set.
- A child that exits while something it started still holds its output open is not waited for beyond a short
  grace (250 ms).
- Returns `XSTDOK` when the child ran and was collected, whatever its exit code; `XSTDERR` when it could not be
  started or collected. Release the buffers with `XExec_ResultClear`.

## Coverage Builds

A child that fails before `execve()` leaves with `_exit()`, which skips the gcov exit handler. Built with
`XUTILS_TEST_COVERAGE` and `--coverage`, the library defines `XEXEC_GCOV_DUMP` and the child calls `__gcov_dump()`
first, so the failure paths it ran are counted. Other builds are unaffected.
