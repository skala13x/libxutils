# thread.c

## Purpose

Thread helpers plus a detached repeating-task wrapper.

## API Reference

#### `void XThread_Init(xthread_t *pThread)`

- Arguments:
  - thread object.
- Does:
  - sets default stack size and clears callback, argument, detach and status fields.
- Returns:
  - no return value.

#### `int XThread_Run(xthread_t *pThread)`

- Arguments:
  - initialized thread object with `functionCb` and `pArgument` set.
- Does:
  - creates the OS thread.
  - uses detached thread creation on POSIX when `nDetached` is true.
  - closes the Windows handle immediately for detached threads.
- Returns:
  - `XSTDOK` on success.
  - `XSTDNON` when `functionCb == NULL`.
  - `XSTDERR` on thread-creation failure.

#### `int XThread_Create(xthread_t *pThread, xthread_cb_t func, void *pArg, uint8_t nDtch)`

- Arguments:
  - thread object, callback, callback argument and detached flag.
- Does:
  - initializes the object, stores parameters and runs the thread.
- Returns:
  - forwarded result from `XThread_Run()`.

#### `void *XThread_Join(xthread_t *pThread)`

- Arguments:
  - thread object.
- Does:
  - waits for a non-detached thread to finish.
- Returns:
  - `NULL` for detached threads.
  - joined thread return value on successful POSIX `pthread_join()`.
  - `NULL` on join failure.
- Caveat:
  - Windows path always returns `NULL` even after a successful wait because the implementation does not preserve a thread return value.

### Task helpers

#### `int XTask_Start(xtask_t *pTask, xtask_cb_t callback, void *pContext, uint32_t nIntervalU)`

- Arguments:
  - task object, callback, callback context and per-iteration sleep interval in microseconds.
- Does:
  - stores callback and context.
  - sets action to `XTASK_CTRL_RELEASE`.
  - spawns a detached worker thread.
- Returns:
  - `0` when `callback == NULL`.
  - otherwise the thread-creation status from `XThread_Create()`.

#### `int XTask_StartJoinable(xtask_t *pTask, xtask_cb_t callback, void *pContext, uint32_t nIntervalU)`

- As `XTask_Start()`, with a joinable worker: the `XTask_Stop()` that waits for it to stop also joins its thread, and
  returns only once the thread is gone. Until then the task holds its thread, so it has to be stopped that way, but
  a stop that does not wait leaves the join to the next one that does.

#### `uint32_t XTask_Hold(xtask_t *pTask, int nIntervalU)`

#### `uint32_t XTask_Release(xtask_t *pTask, int nIntervalU)`

#### `uint32_t XTask_Stop(xtask_t *pTask, int nIntervalU)`

- Arguments:
  - task object and poll sleep interval used while waiting for the target state.
- Does:
  - updates the control action to pause, release or stop.
  - waits until worker status becomes `XTASK_STAT_PAUSED`, `XTASK_STAT_ACTIVE` or `XTASK_STAT_STOPPED`, sleeping
    `nIntervalU` between checks, yielding for `0`, and not waiting at all when it is negative.
  - does not wait for what the task can no longer do: a stopped task is never paused or active again, and one that
    never started, or failed to, never stops.
  - `XTask_Stop()` joins a worker started with `XTask_StartJoinable()` once it has stopped.
- Returns:
  - the microseconds slept, stopping at `UINT32_MAX` rather than wrapping round.

## Worker Semantics

- the worker thread is internal but important:
  - sets status to `XTASK_STAT_ACTIVE` on start
  - `XTASK_CTRL_PAUSE` switches status to `XTASK_STAT_PAUSED` and sleeps
  - `XTASK_CTRL_RELEASE` resumes active callback execution
  - callback return `< 0` stops the worker
  - stop sets status to `XTASK_STAT_STOPPED`

## Important Notes

- `XTask_Start()` tasks are detached; `Stop()` is the intended shutdown path. Once a task is `XTASK_STAT_STOPPED`
  its worker touches neither the task nor its context again, so either can be freed, but the thread may still be
  on its way out: code it returns through, such as a plugin that links the library, must not be unloaded until it is
  gone. `XTask_StartJoinable()` is for that.
