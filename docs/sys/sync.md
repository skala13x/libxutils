# sync.c

## Purpose

Cross-platform mutexes, rwlocks, atomic helpers and a tiny bar/ack primitive.

## API Reference

#### `void xusleep(uint32_t nUsecs)`

- Arguments:
  - microseconds to sleep.
- Does:
  - sleeps with `usleep()` on POSIX.
  - uses millisecond-granularity `Sleep()` on Windows.
- Returns:
  - no return value.

#### `void xyield(void)`

- Does:
  - gives the processor up to another thread without sleeping (`sched_yield()`, `SwitchToThread()`): what the waits
    with no interval do between their checks, instead of a busy loop.

#### Atomic helpers

- `XSYNC_ATOMIC_ADD`, `XSYNC_ATOMIC_SUB`, `XSYNC_ATOMIC_SET`, `XSYNC_ATOMIC_GET` and `XSYNC_ATOMIC_CAS` are full
  barriers: what a thread wrote before one of them is seen by any thread that sees its effect.
- `XSYNC_ATOMIC_SET` exchanges any value and returns the previous one. It used to be `__sync_lock_test_and_set()`,
  an acquire barrier only, which let a store before it be seen after it on weakly ordered processors.
- `XSYNC_ATOMIC_CAS(dst, old, new)` stores `new` only if the value is still `old`, and says whether it did.

#### `void XSync_Init(xsync_mutex_t *pSync)`

#### `void XSync_Destroy(xsync_mutex_t *pSync)`

#### `void XSync_Lock(xsync_mutex_t *pSync)`

#### `void XSync_Unlock(xsync_mutex_t *pSync)`

- Arguments:
  - mutex object.
- Does:
  - initializes a recursive mutex / critical section and controls it.
  - operations are skipped when `bEnabled == XFALSE`.
- Returns:
  - no return value.
- Failure behavior:
  - initialization, destroy, lock and unlock errors print to `stderr` and terminate the process.

#### `void XRWSync_Init(xsync_rw_t *pSync)`

#### `void XRWSync_ReadLock(xsync_rw_t *pSync)`

#### `void XRWSync_WriteLock(xsync_rw_t *pSync)`

#### `void XRWSync_Unlock(xsync_rw_t *pSync)`

#### `void XRWSync_ReadUnlock(xsync_rw_t *pSync)`

#### `void XRWSync_WriteUnlock(xsync_rw_t *pSync)`

#### `void XRWSync_Destroy(xsync_rw_t *pSync)`

- Arguments:
  - rwlock object.
- Does:
  - initializes, locks, unlocks and destroys a read/write lock.
  - `XRWSync_ReadUnlock()` releases a lock taken with `XRWSync_ReadLock()`, `XRWSync_WriteUnlock()` one taken with
    `XRWSync_WriteLock()`; `XRWSync_Unlock()` releases either.
  - an SRW lock on Windows has to be released as the kind it was taken as. The writer holding it marks it exclusive
    for `XRWSync_Unlock()`, and the mark is cleared before the lock is released, so the next holder never finds the
    previous one's mark. The explicit unlocks do not need the mark at all.
- Returns:
  - no return value.
- Failure behavior:
  - underlying OS failures print to `stderr` and terminate the process.

#### `void XSyncBar_Bar(xsync_bar_t *pBar)`

#### `void XSyncBar_Ack(xsync_bar_t *pBar)`

#### `void XSyncBar_Reset(xsync_bar_t *pBar)`

- Arguments:
  - bar/ack state object.
- Does:
  - a handshake between a controller and one thread. The controller raises the barrier, waits for the
    acknowledgement and lowers it, as often and as quickly as it likes. The thread waits for the barrier, does what
    it is for, acknowledges it, and then waits while `XSyncBar_CheckAck()` holds before it looks for the next one.
  - every barrier raised takes a value of its own from one count for the process (odd), a lowered one is zero, and
    an acknowledgement is of the value it saw. So an acknowledgement answers the barrier it was for and no other,
    and stops holding the moment that barrier is lowered or another one raised. A pair of flags used to let a quick
    lower and raise go unseen, or be taken for the barrier before.
  - `XSyncBar_Reset()` sets up a barrier from any memory, as it always has.
- Returns:
  - no return value.

#### `uint8_t XSyncBar_CheckBar(xsync_bar_t *pBar)`

#### `uint8_t XSyncBar_CheckAck(xsync_bar_t *pBar)`

- Arguments:
  - bar/ack state object.
- Does:
  - tells whether the barrier is up, and whether the barrier as it is now has been acknowledged.
- Returns:
  - `XSTDOK` when it is.
  - `XSTDNON` otherwise.

#### `uint32_t XSyncBar_WaitAck(xsync_bar_t *pBar, uint32_t nSleepUsec)`

#### `XSTATUS XSyncBar_WaitAckFor(xsync_bar_t *pBar, uint32_t nSleepUsec, uint32_t nTimeoutUsec)`

- Arguments:
  - bar/ack state object.
  - sleep interval between checks; `0` yields instead of sleeping.
  - for `XSyncBar_WaitAckFor()`, how long to wait at most, by the monotonic clock.
- Does:
  - waits until the barrier is acknowledged; `XSyncBar_WaitAck()` for as long as it takes.
- Returns:
  - `XSyncBar_WaitAck()`: the microseconds slept, stopping at `UINT32_MAX` rather than wrapping round.
  - `XSyncBar_WaitAckFor()`: `XSTDOK` when acknowledged, `XSTDNON` when the time ran out.

## Important Notes

- These primitives are designed for internal runtime code, not for failure-tolerant library boundaries.
- The lifecycle of a lock is its owner's to order: `XSync_Init`/`Destroy` and `XRWSync_Init`/`Destroy` must not run
  while any other thread can lock or unlock it. `bEnabled` tells a lock never set up, or torn down, from one in use
  for a caller that does so in order; no flag can make a lock destroyed under another thread safe to use.
- On Windows, `xusleep()` rounds sub-millisecond waits up to at least `1 ms`.
