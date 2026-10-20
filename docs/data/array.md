# array.c

## Purpose

Growable array of `xarray_data_t *` entries. Supports copied payloads, ownership transfer, pool-backed allocation, sorting and key-based search.

## Types

- `xarray_data_t`: one array slot containing `pData`, `nSize`, `nKey` and pool pointer.
- `xarray_t`: array runtime state, optional pool owner, cleanup callback and capacity/usage counters.

## API Reference

### Entry helpers

#### `xarray_data_t *XArray_NewData(xarray_t *pArr, void *pData, size_t nSize, uint32_t nKey)`

- Args:
  - `pArr`: destination array context; its pool is used. `NULL` makes no entry.
  - `pData`: source pointer.
  - `nSize`: payload size; when `> 0` the payload is copied, when `0` the pointer is stored directly.
  - `nKey`: key stored in the entry.
- Returns:
  - new entry on success.
  - `NULL` on allocation failure.

#### `void XArray_FreeData(xarray_data_t *pArrData)`

- Frees one entry and, when `nSize > 0`, frees its payload too.
- No return value.

#### `void XArray_ClearData(xarray_t *pArr, xarray_data_t *pArrData)`

- Runs `pArr->clearCb` first if set, then frees the entry.
- No return value.

### Array creation and teardown

#### `void *XArray_Init(xarray_t *pArr, xpool_t *pPool, size_t nSize, uint8_t nFixed)`

#### `void *XArray_InitPool(xarray_t *pArr, size_t nPoolSize, size_t nSize, uint8_t nFixed)`

- Args:
  - `pArr`: array to initialize.
  - `pPool` / `nPoolSize`: external or internal pool source.
  - `nSize`: initial capacity.
  - `nFixed`: whether realloc/shrink is disabled.
- Does:
  - zeroes runtime state and optionally allocates slot storage.
  - `InitPool` also creates and owns a pool.
- Returns:
  - `pArr->pData` when initial storage exists.
  - non-`NULL` success even with zero initial size.
  - `NULL` on allocation failure, for a `NULL` `pArr`, and for an `nSize` with more slots than an `int` can
    index or whose size in bytes does not fit `size_t`.

#### `xarray_t *XArray_New(xpool_t *pPool, size_t nSize, uint8_t nFixed)`

#### `xarray_t *XArray_NewPool(size_t nPoolSize, size_t nSize, uint8_t nFixed)`

- Heap/pool allocate the `xarray_t` itself and then initialize it.
- `NewPool` places the `xarray_t` itself first in its pool; `XArray_Clear` resets the pool around it.
- Return allocated array or `NULL`.

#### `size_t XArray_Realloc(xarray_t *pArr)`

- Grows when full and shrinks when usage drops below 25%, unless fixed.
- Returns new capacity.
- Returns `0` on failure, also when doubling would pass the slot limit; `eStatus` is then
  `XARRAY_STATUS_NO_MEMORY`.

#### `void XArray_Clear(xarray_t *pArr)`

- Clears every slot via `XArray_ClearData`.
- Resets owned pool when `nHasPool` is true, which gives back all the memory the entries used, also for an array
  made by `XArray_NewPool` that lives in that pool.

#### `void XArray_Destroy(xarray_t *pArr)`

- Clears entries, frees slot array, destroys owned pool and optionally frees `pArr` itself.

#### `void XArray_Free(xarray_t **ppArr)`

- Destroys the array and nulls the caller pointer.

### Insert / append / replace

#### `int XArray_Add(xarray_t *pArr, xarray_data_t *pNewData)`

- Appends a ready-made entry.
- Returns appended index on success.
- Returns `XARRAY_FAILURE` on failure and clears `pNewData`.

#### `int XArray_AddData(xarray_t *pArr, void *pData, size_t nSize)`

- Creates a new entry with `XArray_NewData` and appends it.
- When the entry was made but there is no room, `XArray_Add` clears it (`clearCb` runs); when the entry could not
  be made, `pData` stays with the caller. The two failures return the same value.
- Returns appended index or `XARRAY_FAILURE`.

#### `int XArray_PushData(xarray_t *pArr, void *pData, size_t nSize)`

- Stores the raw pointer without copying and then records `nSize`.
- Use this as ownership transfer. The entry releases the buffer the way it releases its own copies: with `free()`
  for an array without a pool, back to the pool for one with a pool.
- On failure the buffer is released or stays with the caller as described for `AddData`.
- Returns appended index or `XARRAY_FAILURE`.

#### `int XArray_AddDataKey(xarray_t *pArr, void *pData, size_t nSize, uint32_t nKey)`

- Same as `AddData` but stores `nKey`.
- Returns appended index or `XARRAY_FAILURE`.

#### `xarray_data_t *XArray_Set(xarray_t *pArr, size_t nIndex, xarray_data_t *pNewData)`

- Replaces the slot at `nIndex`.
- Does not change `XArray_Used()`, also for a slot past the used ones. Such an entry is released by `Clear` and
  `Destroy`, kept when the table shrinks, but overwritten by the next `Add`/`Insert` that reaches its slot.
- Returns the old entry pointer.
- Returns `NULL` when index is outside capacity.

#### `xarray_data_t *XArray_SetData(xarray_t *pArr, size_t nIndex, void *pData, size_t nSize)`

- Creates a new entry from data and replaces the slot, as `XArray_Set`.
- Returns old entry pointer or `NULL`.

#### `xarray_data_t *XArray_Insert(xarray_t *pArr, size_t nIndex, xarray_data_t *pData)`

- Inserts one entry at `nIndex` and shifts later entries right.
- Returns the element now located at `nIndex + 1` after insertion.
- Returns `NULL` on allocation/space failure or when insert point had no previous entry.

#### `xarray_data_t *XArray_InsertData(xarray_t *pArr, size_t nIndex, void *pData, size_t nSize)`

- Creates an entry and inserts it.
- Returns same semantics as `XArray_Insert`.

### Read helpers

#### `xarray_data_t *XArray_Get(xarray_t *pArr, size_t nIndex)`

- Returns slot pointer or `NULL`.

#### `void *XArray_GetData(xarray_t *pArr, size_t nIndex)`

- Returns payload pointer or `NULL`.

#### `void *XArray_GetDataOr(xarray_t *pArr, size_t nIndex, void *pRet)`

- Returns payload pointer or fallback `pRet`.

#### `size_t XArray_GetSize(xarray_t *pArr, size_t nIndex)`

- Returns payload size or `0`.

#### `uint32_t XArray_GetKey(xarray_t *pArr, size_t nIndex)`

- Returns entry key or `0`.

#### `uint8_t XArray_Contains(xarray_t *pArr, size_t nIndex)`

- Returns `1` when slot exists and is non-NULL, otherwise `0`.

#### `size_t XArray_Used(xarray_t *pArr)`

#### `size_t XArray_Size(xarray_t *pArr)`

- Return used slots / total capacity.
- Return `0` for null array.

### Removal and movement

#### `xarray_data_t *XArray_Remove(xarray_t *pArr, size_t nIndex)`

- Removes one slot, shifts later entries left and may shrink capacity.
- Returns removed entry pointer for caller-owned cleanup.
- An entry stored past the used slots is taken out of its slot, nothing is shifted.
- Returns `NULL` when the index is outside capacity or the slot is empty (an empty slot is left in place).

#### `void XArray_Delete(xarray_t *pArr, size_t nIndex)`

- Same removal path as `Remove`, but also clears/frees the removed entry.

#### `void XArray_Swap(xarray_t *pArr, size_t nIndex1, size_t nIndex2)`

- Swaps two used entries in place.

### Sorting

A caller's comparator is given empty slots as `NULL`; the comparators of `SortBy` place them after all entries.
The quick sort stays inside the array and ends even when the comparator does not answer consistently (the order is
then unspecified), and finishes ranges of up to 16 entries by insertion.

#### `void XArray_Sort(xarray_t *pArr, xarray_comparator_t compare, void *pCtx)`

- Quick-sorts used entries with the provided comparator.

#### `void XArray_BubbleSort(xarray_t *pArr, xarray_comparator_t compare, void *pCtx)`

- Bubble-sorts used entries, stopping after a pass that swaps nothing.

#### `void XArray_QuickSort(xarray_t *pArr, xarray_comparator_t compare, void *pCtx, int nStart, int nFinish)`

- Recursive quicksort over an explicit range.

#### `void XArray_SortBy(xarray_t *pArr, int nSortBy)`

- Sorts by `XARRAY_SORTBY_SIZE` or `XARRAY_SORTBY_KEY`.

### Searches

#### `int XArray_SentinelSearch(xarray_t *pArr, uint32_t nKey)`

#### `int XArray_LinearSearch(xarray_t *pArr, uint32_t nKey)`

#### `int XArray_DoubleSearch(xarray_t *pArr, uint32_t nKey)`

#### `int XArray_BinarySearch(xarray_t *pArr, uint32_t nKey)`

- Args:
  - array and search key.
- Returns:
  - matching index on success.
  - `XARRAY_FAILURE` (`-1`) when not found or array is unusable.
- Notes:
  - `BinarySearch` assumes entries are already sorted by `nKey`.
  - None of the searches write to the array, so concurrent readers are safe. `SentinelSearch` looks at the last
    entry first and then at the others in order; it no longer parks a sentinel in the last slot.
