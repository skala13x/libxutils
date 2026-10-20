# buf.c

## Purpose

Three buffer families:

- `XByteBuffer_*`: dynamic byte buffer
- `XDataBuffer_*`: pointer buffer
- `XRingBuffer_*`: circular queue of byte buffers

## `XByteBuffer_*` API

### `uint8_t *XByteData_Dup(const uint8_t *pBuff, size_t nSize)`

- Duplicates `nSize` bytes and appends a trailing NUL.
- Returns allocated buffer or `NULL`.

### `xbyte_buffer_t *XByteBuffer_New(size_t nSize, int nFastAlloc)`

### `int XByteBuffer_Init(xbyte_buffer_t *pBuffer, size_t nSize, int nFastAlloc)`

- `nFastAlloc` enables geometric growth on reserve.
- `New` allocates the struct and initializes it.
- `Init` returns new reserved size or negative status.

### `void XByteBuffer_Clear(xbyte_buffer_t *pBuffer)`

### `void XByteBuffer_Free(xbyte_buffer_t **pBuffer)`

### `void XByteBuffer_Reset(xbyte_buffer_t *pBuffer)`

- `Clear`: frees owned storage and resets fields.
- `Free`: clears and frees the struct when it was heap-allocated.
- `Reset`: keeps allocated storage but sets used length to zero.

### `int XByteBuffer_Resize(xbyte_buffer_t *pBuffer, size_t nSize)`

### `int XByteBuffer_Reserve(xbyte_buffer_t *pBuffer, size_t nSize)`

- `Resize` reallocates exact capacity.
- `Reserve` ensures space for `nUsed + nSize`.
- Return resulting size or negative error.
- `Resize(0)` clears buffer and returns `XSTDNON`.

### `int XByteBuffer_SetData(xbyte_buffer_t *pBuffer, uint8_t *pData, size_t nSize)`

### `int XByteBuffer_Set(xbyte_buffer_t *pBuffer, xbyte_buffer_t *pSrc)`

- Attach external storage without owning/resizing semantics.
- Storage the buffer owned is freed first, so it is not leaked, unless the new storage lies inside it: a view into
  its own allocation leaves that allocation to the caller, who has to free it.
- Return attached used length.

### `int XByteBuffer_OwnData(xbyte_buffer_t *pBuffer, uint8_t *pData, size_t nSize)`

### `int XByteBuffer_Own(xbyte_buffer_t *pBuffer, xbyte_buffer_t *pSrc)`

- Transfer ownership into `pBuffer`.
- A buffer handed itself keeps what it has.
- Return resulting owned size.

### `int XByteBuffer_Add(xbyte_buffer_t *pBuffer, const uint8_t *pData, size_t nSize)`

### `int XByteBuffer_AddByte(xbyte_buffer_t *pBuffer, uint8_t nByte)`

### `int XByteBuffer_AddStr(xbyte_buffer_t *pBuffer, xstring_t *pStr)`

### `int XByteBuffer_AddFmt(xbyte_buffer_t *pBuffer, const char *pFmt, ...)`

### `int XByteBuffer_AddBuff(xbyte_buffer_t *pBuffer, xbyte_buffer_t *pSrc)`

- Append bytes, one byte, `xstring_t`, formatted text or another byte buffer.
- `pData` may lie in the buffer's own content, `[pData, pData + nUsed)`; it is found again if the buffer grows.
  The rest of the buffer's storage, past `nUsed`, is where the bytes are copied to, so bytes there are refused with
  `XSTDERR` and `nStatus` set to it, as bytes running past the end of the content are. They used to be copied onto
  themselves or, once the buffer grew, read from the storage that was freed.
- Return new used length on success.
- Return `XSTDERR`/`XSTDNON` on failure depending on path.

### `int XByteBuffer_Insert(xbyte_buffer_t *pBuffer, size_t nPosit, const uint8_t *pData, size_t nSize)`

- Inserts bytes at position or appends when `nPosit >= nUsed`.
- `pData` may lie in the buffer's own content, which is copied before the insertion moves it. Bytes in the rest of
  its storage, past `nUsed`, are where the content after `nPosit` moves to, and are refused as `XByteBuffer_Add()`
  refuses them.
- Returns new used length or `XSTDERR`.

### `int XByteBuffer_Remove(xbyte_buffer_t *pBuffer, size_t nPosit, size_t nSize)`

- Removes bytes in place without shrinking allocation.
- Returns number of removed bytes or `0`.

### `int XByteBuffer_Delete(xbyte_buffer_t *pBuffer, size_t nPosit, size_t nSize)`

- Removes bytes and then resizes to exact `nUsed + 1`.
- Returns buffer status.

### `int XByteBuffer_Advance(xbyte_buffer_t *pBuffer, size_t nSize)`

- Deletes from the front.
- Returns new used length.

### `int XByteBuffer_Terminate(xbyte_buffer_t *pBuffer, size_t nPosit)`

### `int XByteBuffer_NullTerm(xbyte_buffer_t *pBuffer)`

- `Terminate` truncates used length at `nPosit`.
- `NullTerm` ensures trailing NUL at current end.
- Return `XSTDOK`/used length or `XSTDERR`.

### `int XByteBuffer_ReadStdin(xbyte_buffer_t *pBuffer)`

- Reads stdin until EOF and appends.
- Returns final used length, or `XSTDERR` when a read failed (`ferror(stdin)`) or the bytes cannot be kept; what
  was appended before stays in the buffer.

### `uint8_t XByteBuffer_GetByte(xbyte_buffer_t *pBuffer, size_t nIndex)`

- Returns byte at index or `0` when out of range.

### `xbool_t XByteBuffer_HasData(xbyte_buffer_t *pBuffer)`

- Returns `XTRUE` when buffer exists and `nUsed > 0`.

## `XDataBuffer_*` API

### `int XDataBuffer_Init(xdata_buffer_t *pBuffer, size_t nSize, int nFixed)`

- Allocates pointer table of `nSize`.
- Returns capacity or `XSTDERR`.

### `void XDataBuffer_Clear(xdata_buffer_t *pBuffer)`

### `void XDataBuffer_Destroy(xdata_buffer_t *pBuffer)`

- `Clear` runs `clearCb` for every slot and resets usage.
- `Destroy` also frees the pointer table.

### `int XDataBuffer_Add(xdata_buffer_t *pBuffer, void *pData)`

- Appends pointer.
- Returns appended index on success.
- Returns `-2` when reallocation failed after full buffer path, or `XSTDERR` when capacity is already full before growth.

### `void *XDataBuffer_Set(xdata_buffer_t *pBuffer, unsigned int nIndex, void *pData)`

- Replaces pointer at index.
- Returns previous pointer.
- Caveat:
  - current bounds condition is loose and should not be trusted blindly.

### `void *XDataBuffer_Get(xdata_buffer_t *pBuffer, unsigned int nIndex)`

- Returns pointer at index.
- Returns `NULL` when `nIndex >= nUsed` or `nIndex == 0`.

### `void *XDataBuffer_Pop(xdata_buffer_t *pBuffer, unsigned int nIndex)`

- Removes pointer and shifts later items left.
- Returns removed pointer or `NULL`.

## `XRingBuffer_*` API

### `int XRingBuffer_Init(xring_buffer_t *pBuffer, size_t nSize)`

- Allocates `nSize` internal `xbyte_buffer_t` slots.
- Returns capacity or `0`.

### `void XRingBuffer_Reset(xring_buffer_t *pBuffer)`

### `void XRingBuffer_Destroy(xring_buffer_t *pBuffer)`

- `Reset` clears/free each slot and resets indices.
- `Destroy` also frees the slot table.

### `void XRingBuffer_Update(xring_buffer_t *pBuffer, int nAdd)`

### `void XRingBuffer_Advance(xring_buffer_t *pBuffer)`

- Internal index maintenance helpers.
- `Advance` drops the oldest item.

### `int XRingBuffer_AddData(xring_buffer_t *pBuffer, const uint8_t *pData, size_t nSize)`

### `int XRingBuffer_AddDataAdv(xring_buffer_t *pBuffer, const uint8_t *pData, size_t nSize)`

- `AddData` appends into current back slot when ring is not full.
- `AddDataAdv` drops the oldest element of a full ring for the new one, once the new one is made: nothing to add
  and a failed add leave the ring as it was.
- Return append status from `XByteBuffer_Add` or `0`.

### `int XRingBuffer_GetData(xring_buffer_t *pBuffer, uint8_t **pData, size_t *pSize)`

- Returns front slot pointer and size.
- Returns `1` on success, `0` on failure.
- Caveat:
  - current full-buffer check is inverted; a full ring may be treated as unavailable.

### `int XRingBuffer_Pop(xring_buffer_t *pBuffer, uint8_t *pData, size_t nSize)`

- Copies front slot bytes into caller buffer, clears that slot and advances.
- Returns copied byte count.

