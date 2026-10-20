# mdtp.c

## Purpose

MDTP (Modern Data Transmit Protocol) packet framing and parsing.

## API Reference

### Enum/string helpers

#### `xpacket_type_t XPacket_GetType(const char *pType)`

#### `const char *XPacket_GetTypeStr(xpacket_type_t eType)`

#### `const char *XPacket_GetStatusStr(xpacket_status_t eStatus)`

- Convert between enum and static text.

### Lifecycle

#### `xpacket_status_t XPacket_Init(xpacket_t *pPacket, uint8_t *pData, uint32_t nSize)`

- Initializes packet state and optionally parses raw data.
- Returns packet status.

#### `xpacket_t *XPacket_New(uint8_t *pData, uint32_t nSize)`

- Allocates packet and initializes it.
- Returns packet or `NULL`.

#### `void XPacket_Clear(xpacket_t *pPacket)`

- Runs optional callback with `XPACKET_CB_CLEAR`, frees header object and clears raw storage.

#### `void XPacket_Free(xpacket_t **pPacket)`

- Clears and frees packet, then nulls caller pointer.

### Parse / assemble

#### `xpacket_status_t XPacket_Parse(xpacket_t *pPacket, const uint8_t *pData, size_t nSize)`

- Parses `4-byte header length + JSON header + payload`.
- Returns:
  - `XPACKET_INCOMPLETE` when even header is incomplete.
  - `XPACKET_PARSED` when header is complete but payload is incomplete.
  - `XPACKET_COMPLETE` when full packet is available.
  - `XPACKET_ERR_ALLOC` when the header could not be parsed for want of memory, `XPACKET_INVALID` when it is
    malformed, and another error status otherwise.
  - `XPACKET_INVALID` for a header length of zero, whatever follows it. `XPacket_Create()` never writes one, and
    it used to parse as a complete packet of size 0, which left a reader of the stream with nothing to say how far
    it had come: the MDTP reader in `api.c` then skipped everything buffered after it.

#### `xpacket_status_t XPacket_UpdateHeader(xpacket_t *pPacket)`

- Rebuilds JSON header from `pPacket->header`.
- Returns packet status.

#### `void XPacket_ParseHeader(xpacket_header_t *pHeader, xjson_obj_t *pHeaderObj)`

- Extracts known fields from parsed header JSON.

#### `xpacket_status_t XPacket_Create(xbyte_buffer_t *pBuffer, const char *pHeader, size_t nHdrLen, uint8_t *pData, size_t nSize)`

- Appends one serialized packet into caller byte buffer, growing it once at most. A `NULL` `pData` adds the header
  only, whatever `nSize` says.
- `pHeader` and `pData` may lie in the content of `pBuffer` itself, `[pData, pData + nUsed)`; they are found again if
  the buffer has to grow. The rest of its storage, past `nUsed`, is where the packet is written, and is refused.
- Returns packet status: `XPACKET_BIGDATA`, with nothing written, for a header or a payload longer than the 32 bits
  the length prefix and the payload size have, or for a packet that does not fit behind what `pBuffer` holds
  (`INT_MAX` bytes in all); `XPACKET_INVALID_ARGS` for missing arguments, or a header or payload in the storage of
  `pBuffer` that is not wholly in its content; `XPACKET_ERR_ALLOC` when the buffer cannot grow.
- On any failure `pBuffer` holds what it held before. It used to be cleared when it could not grow, which threw away
  every packet queued in it before this one.

#### `xbyte_buffer_t *XPacket_Assemble(xpacket_t *pPacket)`

- Serializes packet header object plus payload into `rawData`.
- Returns pointer to `rawData`, or `NULL` when the packet could not be built, a header that cannot be written
  included. A packet that could not be built leaves `rawData` empty.

#### `xpacket_status_t XPacket_WriteTo(xpacket_t *pPacket, xbyte_buffer_t *pBuffer)`

- Appends the same bytes `XPacket_Assemble()` builds to the end of `pBuffer`, for example an `xapi_session_t` TX
  buffer (`XAPI_GetTxBuff()`). The payload is copied once, straight into `pBuffer`, which makes this the cheap way
  to queue a packet compared with `XPacket_Assemble()` followed by copying `rawData`.
- Updates the header object and the header fields as `XPacket_Assemble()` does, and leaves `rawData` and
  `nHeaderLength` alone. The same packet can be appended any number of times.
- Returns `XPACKET_ERR_NONE`, `XPACKET_INVALID_ARGS` for a missing packet or buffer, `XPACKET_INVALID` for an error
  or invalid packet type or a header that cannot be written, or a status of `XPacket_Create()`. On any failure
  `pBuffer` holds what it held before.

### Access helpers

#### `const uint8_t *XPacket_GetHeader(xpacket_t *pPacket)`

#### `const uint8_t *XPacket_GetPayload(xpacket_t *pPacket)`

#### `size_t XPacket_GetSize(xpacket_t *pPacket)`

- Return header bytes, payload pointer or full packet size.
- Return `NULL`/`0` when missing.
