# http.c

## Purpose

HTTP request/response object, parser, assembler and simple blocking client helpers.

## Callback Semantics

- `XHTTP_SetCallback()` installs an optional callback plus a bitmask of enabled callback kinds.
- Read helpers interpret callback return values specially:
  - `XSTDERR`: terminate parsing and return `XHTTP_TERMINATED`.
  - `XSTDNON`: mark the message complete immediately.
  - `XSTDOK`: treat bytes as consumed but do not append them to `rawData`.
  - any other value: append/read normally.

## Message Framing

Where a body ends follows RFC 9112, 6.3, and `Content-Type` has no part in it. The parser records the rule it
applied in `eFraming`:

- `XHTTP_FRAME_NONE`: no body. A request without `Content-Length` or `Transfer-Encoding`, and a `1xx`, `204` or
  `304` response whatever it announces. The bytes after the header belong to the next message.
- `XHTTP_FRAME_LENGTH`: `Content-Length` bytes. A length is a run of digits, or a comma-separated list of the
  same one, on one line or over several (RFC 9112, 6.3). Every element of the list is a length: `,5`, `5,` and
  `5,,5` are malformed.
- `XHTTP_FRAME_CHUNKED`: `Transfer-Encoding` whose last coding is `chunked`, listed once, over all of its lines.
  A `Content-Length` next to it is ignored, and `nKeepAlive` is then false. A request with any other transfer
  coding has no length that can be known and is `XHTTP_INVALID`.
- `XHTTP_FRAME_CLOSE`: a response without a length. Its body ends with the connection, so a parse of a buffer
  stops at `XHTTP_PARSED`, and only `XHTTP_ReadContent()` reaching the end of the stream completes it.
  `nKeepAlive` is false.

A `Content-Length` that is not a length makes the message `XHTTP_INVALID`, whatever else would frame it: an empty
value, one with anything but digits in it (`banana`, `5x`, `-5`, `+5`), a list with an empty element in it, one
too big for a `size_t`, or values that differ, on one line or over several. Lengths are compared as numbers, so `05` and `5` agree. Such a value used to
be read as 0, and the body after it was taken for the next message.

A chunked body is decoded before anyone sees it: the content callback, `XHTTP_GetBody()` and `nContentLength` all
give the data of its chunks, never their framing. Trailer fields are read and dropped. Every line must end in
CRLF, a size line is at most 4096 bytes and the trailer section at most `XHTTP_HEADER_MAX`; anything else is
`XHTTP_INVALID`.

- A parse leaves the bytes it was given as they were, whether the buffer is the handle's own or the caller's
  (`XHTTP_ParseBuff()`). A chunked body that a parse finds whole is decoded into `bodyData` instead, and the
  accessors report that body; `XHTTP_GetPacketSize()` is still the size the message took on the wire.
- The socket readers decode as they read, in `rawData`, which then holds the header, `nContentLength` body bytes
  and whatever was read after the message.

`XHTTP_ScanChunked()` finds the end of a chunked body without decoding it, a piece at a time.

## API Reference

### Enum and status helpers

#### `const char *XHTTP_GetStatusStr(xhttp_status_t eStatus)`

#### `const char *XHTTP_GetCodeStr(int nCode)`

#### `const char *XHTTP_GetMethodStr(xhttp_method_t eMethod)`

#### `xhttp_method_t XHTTP_GetMethodType(const char *pData)`

- Arguments:
  - enum/code or method prefix string.
- Does:
  - converts HTTP status, status code and method enums to text, or parses a request-line method prefix.
- Returns:
  - static string pointers or a method enum.
  - `XHTTP_DUMMY` for unknown method text.

#### `xbool_t XHTTP_IsSuccessCode(xhttp_t *pHandle)`

- Arguments:
  - `pHandle`: HTTP object.
- Does:
  - checks whether `nStatusCode` is in `[200, 299]`.
- Returns:
  - `XTRUE` or `XFALSE`.

### Object lifecycle

#### `int XHTTP_SetCallback(xhttp_t *pHttp, xhttp_cb_t callback, void *pCbCtx, uint16_t nCbTypes)`

- Arguments:
  - `pHttp`: HTTP object.
  - `callback`: callback function, may be `NULL`.
  - `pCbCtx`: caller context saved into the object.
  - `nCbTypes`: enabled callback bitmask.
- Does:
  - stores callback configuration.
- Returns:
  - `XSTDOK` on success.
  - `XSTDERR` when `pHttp == NULL`.

#### `int XHTTP_Init(xhttp_t *pHttp, xhttp_method_t eMethod, size_t nSize)`

- Arguments:
  - `pHttp`: object to initialize.
  - `eMethod`: initial method.
  - `nSize`: initial raw-buffer capacity.
- Does:
  - resets all counters and flags.
  - initializes the header map and raw byte buffer.
  - sets default header/content limits.
- Returns:
  - result of `XByteBuffer_Init()`.

#### `int XHTTP_InitRequest(xhttp_t *pHttp, xhttp_method_t eMethod, const char *pUri, const char *pVer)`

- Arguments:
  - `pHttp`: request object.
  - `eMethod`: HTTP method.
  - `pUri`: request URI, defaults to `"\\"` when `NULL`.
  - `pVer`: HTTP version, defaults to `XHTTP_VER_DEFAULT`.
- Does:
  - initializes the object and marks it as a request.
- Returns:
  - init status or `XSTDERR`.

#### `int XHTTP_InitResponse(xhttp_t *pHttp, uint16_t nStatusCode, const char *pVer)`

- Arguments:
  - `pHttp`: response object.
  - `nStatusCode`: HTTP code.
  - `pVer`: version string, defaults to `XHTTP_VER_DEFAULT`.
- Does:
  - initializes the object and marks it as a response.
- Returns:
  - init status on success.
  - `XSTDERR` when base init fails.

#### `size_t XHTTP_SetUnixAddr(xhttp_t *pHttp, const char *pUnixAddr)`

- Arguments:
  - `pHttp`: HTTP object.
  - `pUnixAddr`: Unix-domain socket path.
- Does:
  - stores the Unix socket path in `sUnixAddr`.
- Returns:
  - copied length.
  - `XSTDERR` when arguments are invalid.

#### `void XHTTP_Reset(xhttp_t *pHttp, xbool_t bHard)`

- Arguments:
  - `pHttp`: object to reset.
  - `bHard`: whether to fully destroy/recreate internal storage.
- Does:
  - hard reset:
    - clears and reinitializes the raw buffer and header map.
  - soft reset:
    - preserves allocations but resets used data and map entries.
- Returns:
  - no return value.

#### `xhttp_t *XHTTP_Alloc(xhttp_method_t eMethod, size_t nDataSize)`

- Arguments:
  - `eMethod`: initial method.
  - `nDataSize`: initial buffer size.
- Does:
  - heap-allocates and initializes an `xhttp_t`.
- Returns:
  - allocated object on success.
  - `NULL` on allocation/init failure.

#### `int XHTTP_Copy(xhttp_t *pDst, xhttp_t *pSrc)`

- Arguments:
  - `pDst`: destination object.
  - `pSrc`: source object.
- Does:
  - deep-copies raw bytes, header map, metadata and callback configuration.
- Returns:
  - `XSTDOK` on success.
  - `XSTDERR` on invalid args, buffer-copy failure or header-map copy failure.

#### `void XHTTP_Clear(xhttp_t *pHttp)`

#### `void XHTTP_Free(xhttp_t **pHttp)`

- Arguments:
  - `pHttp`: object or pointer-to-pointer for owned objects.
- Does:
  - destroys header-map contents and raw buffer.
  - `XHTTP_Free()` additionally frees heap storage when `nAllocated` is set.
- Returns:
  - no return value.

### Header and auth helpers

#### `int XHTTP_AddHeader(xhttp_t *pHttp, const char *pHeader, const char *pStr, ...)`

- Arguments:
  - `pHttp`: HTTP object.
  - `pHeader`: header key.
  - `pStr`: printf-style value format string.
- Does:
  - formats the header value.
  - if the key already exists with the same value, leaves it unchanged.
  - if the key exists with a different value, updates it only when `nAllowUpdate` is true.
- Returns:
  - current header count on success.
  - `XSTDEXC` when a different value already exists and updates are disallowed.
  - `XSTDERR` on formatting/allocation/map failure.

#### `size_t XHTTP_GetAuthToken(char *pToken, size_t nSize, const char *pUser, const char *pPass)`

- Arguments:
  - `pToken`: destination buffer.
  - `nSize`: caller-provided destination size.
  - `pUser`, `pPass`: credential strings.
- Does:
  - builds `user:pass`, base64-encodes it and copies the encoded token into `pToken`.
- Returns:
  - copied byte count.
  - `XSTDNON` when base64 encoding fails.
- Caveat:
  - the current implementation ignores `nSize` when copying and instead passes the encoded length to `xstrncpy()`. Treat this helper carefully.

#### `int XHTTP_SetAuthBasic(xhttp_t *pHttp, const char *pUser, const char *pPwd)`

- Arguments:
  - `pHttp`: HTTP object.
  - `pUser`, `pPwd`: credentials.
- Does:
  - base64-encodes `user:password` and inserts `Authorization: Basic ...`.
  - temporarily enables header updates while doing so.
- Returns:
  - `XSTDNON` when either credential is empty.
  - `XSTDERR` on encode failure.
  - otherwise the return from `XHTTP_AddHeader()`.

#### `const char *XHTTP_GetHeader(xhttp_t *pHttp, const char *pHeader)`

- Arguments:
  - `pHttp`: HTTP object.
  - `pHeader`: header name to look up.
- Does:
  - lowercases the lookup key and probes the header map.
- Returns:
  - stored header value pointer or `NULL`.
- Caveat:
  - parsed packets store keys lowercased, so lookup is reliable there.
  - `XHTTP_AddHeader()` stores keys as provided, so for manually assembled objects the safest approach is to use lowercase keys consistently.

### Assembly and access helpers

#### `xbyte_buffer_t *XHTTP_Assemble(xhttp_t *pHttp, const uint8_t *pContent, size_t nLength)`

- Arguments:
  - `pHttp`: request or response object.
  - `pContent`: optional body.
  - `nLength`: body length.
- Does:
  - serializes the start line and headers.
  - auto-adds `Content-Length` when a body exists.
  - auto-adds `Connection: keep-alive` when `nKeepAlive` is set and that header is absent.
  - appends the body and marks the packet complete.
  - reuses `rawData` without rebuilding when `nComplete` is already set.
  - sizes the whole message first and writes it into the allocation `rawData` already has (the 4 KiB
    `XHTTP_InitRequest()` and `XHTTP_InitResponse()` give it), growing it once at most.
  - `pContent` may be the body of a message parsed into the same handle: the message then goes to new storage, and
    the old one is released after the body was copied out of it. It used to be released first.
- Returns:
  - pointer to the internal `rawData` buffer.
  - `NULL` on serialization or allocation failure, with `rawData` empty.

#### `xhttp_status_t XHTTP_WriteTo(xhttp_t *pHttp, xbyte_buffer_t *pBuffer, const uint8_t *pContent, size_t nLength)`

- Arguments:
  - `pHttp`: request or response object.
  - `pBuffer`: buffer the message is appended to, for example an `xapi_session_t` TX buffer (`XAPI_GetTxBuff()`).
  - `pContent`, `nLength`: optional body, as for `XHTTP_Assemble()`.
- Does:
  - appends the same bytes `XHTTP_Assemble()` builds to the end of `pBuffer`: the header is written straight into
    it and the body is copied once, with `pBuffer` grown once at most. This is the cheap way to queue a message,
    compared with `XHTTP_Assemble()` followed by copying `rawData`.
  - adds `Content-Length` and `Connection: keep-alive` to the header map as `XHTTP_Assemble()` does, and leaves
    everything else in the handle alone: `rawData`, `nHeaderLength` and `nComplete` keep describing whatever the
    handle held before. The same handle can be appended any number of times, with a different body each time.
  - `pContent` may lie in the content of `pBuffer` itself, `[pData, pData + nUsed)`; it is found again if the buffer
    has to grow. The rest of its storage, past `nUsed`, is where the message is written, and is refused.
- Returns:
  - `XHTTP_NONE` on success.
  - `XHTTP_INVALID` for a missing handle or buffer, or a body in the storage of `pBuffer` that is not wholly in its
    content.
  - `XHTTP_BIGCNT` for a message that does not fit behind what `pBuffer` holds (`INT_MAX` bytes in all), refused
    before the body is read or the buffer asked to grow.
  - `XHTTP_EALLOC` when a header field or the buffer cannot grow, a borrowed buffer included.
  - on any failure `pBuffer` holds what it held before.

#### `char *XHTTP_GetHeaderRaw(xhttp_t *pHttp)`

- Arguments:
  - `pHttp`: parsed or assembled object.
- Does:
  - copies only the header portion into a new heap string.
- Returns:
  - allocated NUL-terminated header copy.
  - `NULL` when header data is unavailable or allocation fails.

#### `const uint8_t *XHTTP_GetBody(xhttp_t *pHttp)`

#### `size_t XHTTP_GetBodySize(xhttp_t *pHttp)`

- Arguments:
  - `pHttp`: parsed or assembled object.
- Does:
  - exposes the bytes after `nHeaderLength`, which can include the start of the next message: the body itself is
    `nContentLength` bytes long when its framing has a length.
  - for a chunked body a parse decoded into `bodyData`, exposes that body.
- Returns:
  - body pointer or body size.
  - `NULL` / `0` when no complete body is present.

#### `const uint8_t *XHTTP_GetExtraData(xhttp_t *pHttp)`

#### `size_t XHTTP_GetExtraSize(xhttp_t *pHttp)`

- Arguments:
  - `pHttp`: parsed object whose raw buffer may contain more than one packet.
- Does:
  - exposes bytes after the first complete packet: everything after the header for a message without a body,
    nothing for a body that ends with the connection or a chunked one not complete yet.
- Returns:
  - trailing data pointer or size.
  - `NULL` / `0` when there is no extra buffered data.

#### `size_t XHTTP_GetPacketSize(xhttp_t *pHttp)`

- Arguments:
  - `pHttp`: parsed object.
- Does:
  - computes `rawData.nUsed - extraSize`.
- Returns:
  - size of the first complete packet, which is the size it took in the parsed input: advancing that input by
    it lands on the next message, a decoded chunked one included.
  - `0` when `pHttp == NULL`.

### Parse helpers

#### `int XHTTP_AppendData(xhttp_t *pHttp, uint8_t *pData, size_t nSize)`

- Arguments:
  - `pHttp`: HTTP object.
  - `pData`, `nSize`: bytes to append.
- Does:
  - appends raw bytes into `rawData`.
- Returns:
  - `XByteBuffer_Add()` result.

#### `int XHTTP_InitParser(xhttp_t *pHttp, uint8_t *pData, size_t nSize)`

- Arguments:
  - `pHttp`: parser object.
  - `pData`, `nSize`: initial bytes.
- Does:
  - initializes the object and appends the supplied bytes.
- Returns:
  - `XSTDOK` when initialization plus append succeeds.
  - `XSTDERR` when append fails for non-zero input.

#### `xhttp_status_t XHTTP_Parse(xhttp_t *pHttp)`

- Arguments:
  - `pHttp`: object whose `rawData` already contains input.
- Does:
  - detects header boundary.
  - parses type, version, status/method, URI and headers.
  - decides the body framing (see Message Framing), keep-alive state and completeness.
  - decodes a chunked body once all of it is present, into `bodyData`.
  - never changes `rawData`, whatever the outcome.
  - emits `XHTTP_PARSED`/error status callbacks when configured.
- Returns:
  - `XHTTP_INCOMPLETE` when the header/body is not complete yet.
  - `XHTTP_PARSED` when the header is valid but the body is still incomplete, and always for a body that ends
    with the connection.
  - `XHTTP_COMPLETE` when a full packet is present.
  - `XHTTP_INVALID` for a malformed header, a `Content-Length` that is not a length or not the same one on every
    line, a request transfer coding that is not chunked, or malformed chunks.
  - `XHTTP_EALLOC` when a header field or a decoded chunked body cannot be allocated.
  - `XHTTP_TERMINATED` if a callback aborts parsing.

#### `void XHTTP_InitChunked(xhttp_chunked_t *pChunked)`

#### `xhttp_status_t XHTTP_ScanChunked(xhttp_chunked_t *pChunked, const uint8_t *pData, size_t nSize)`

- Arguments:
  - `pChunked`: scan state, initialized once per body.
  - `pData`, `nSize`: the chunked body from its first byte, as much of it as has arrived.
- Does:
  - scans for the end of the body from where the previous call stopped, without decoding it: each byte is looked
    at once however many pieces the body arrives in, and the data of a chunk is skipped.
  - `nPosit` is the wire size scanned, `nBody` the size of the data found so far.
- Returns:
  - `XHTTP_COMPLETE` at the end of the body, `nPosit` then being its whole wire size.
  - `XHTTP_INCOMPLETE` while more bytes are needed.
  - `XHTTP_INVALID` for malformed coding, every call after it as well, or for `pData` shorter than what was
    already scanned.

#### `xhttp_status_t XHTTP_ParseData(xhttp_t *pHttp, uint8_t *pData, size_t nSize)`

#### `xhttp_status_t XHTTP_ParseBuff(xhttp_t *pHttp, xbyte_buffer_t *pBuffer)`

- Arguments:
  - parser object plus input bytes or an existing buffer.
- Does:
  - initializes the parser from raw input and forwards to `XHTTP_Parse()`.
- Returns:
  - parse status.
  - `XHTTP_EALLOC` when initialization/append fails.

### Socket read helpers

#### `xhttp_status_t XHTTP_ReadHeader(xhttp_t *pHttp, xsock_t *pSock)`

- Arguments:
  - `pHttp`: parser object.
  - `pSock`: socket to read from.
- Does:
  - reads until the header becomes parsed/complete, an error occurs, limits are exceeded, or a non-blocking socket would block.
  - emits optional read-header and immediate body callbacks. The body callback is given the bytes of this message
    that came with the header and none of the next one's, the data of chunks decoded.
- Returns:
  - `XHTTP_PARSED` or `XHTTP_COMPLETE` when header handling succeeds.
  - `XHTTP_INCOMPLETE` for non-blocking partial reads.
  - `XHTTP_BIGHDR`, `XHTTP_EREAD`, `XHTTP_EALLOC`, `XHTTP_TERMINATED`, etc. on failure.

#### `xhttp_status_t XHTTP_ReadContent(xhttp_t *pHttp, xsock_t *pSock)`

- Arguments:
  - `pHttp`: object whose header is already parsed.
  - `pSock`: socket to keep reading from.
- Does:
  - reads the body by its framing (see Message Framing):
    - `Content-Length`: until that many bytes arrived, never asking the socket for more than the body still owes,
      so the next message stays in the socket. Bytes a callback claimed count toward the length, also across
      non-blocking calls (`nBodyClaimed`).
    - chunked: decodes as it reads. What was read past the end of the body is kept after it in `rawData`
      (`XHTTP_GetExtraData()`). The end of the stream before the last chunk is `XHTTP_EREAD`.
    - ends with the connection: until the end of the stream, which over TLS has to be the peer's `close_notify`
      (RFC 9112, 9.8). A stream that just stops is `XHTTP_EREAD`, and what arrived stays in the handle. This
      needs `SSL_OP_IGNORE_UNEXPECTED_EOF` (OpenSSL 3.0 or later); the socket's own setting is restored afterwards.
  - honors the callback special return values documented above.
  - applies `nContentMax` to the bytes kept, whether or not the socket blocks.
- Returns:
  - `XHTTP_COMPLETE` when the body is complete.
  - `XHTTP_INCOMPLETE` after a non-blocking read that left the body incomplete.
  - `XHTTP_EREAD` for a failed read, a read that would block (`XSock_Status()` is then `XSOCK_WANT_READ`), or an
    end of the stream that does not end the body.
  - `XHTTP_INVALID` for malformed chunks, `XHTTP_BIGCNT` past `nContentMax`, `XHTTP_EALLOC` when the bytes cannot
    be kept, including a `rawData` the handle does not own, and `XHTTP_TERMINATED` from a callback.

#### `xhttp_status_t XHTTP_Receive(xhttp_t *pHttp, xsock_t *pSock)`

- Arguments:
  - `pHttp`: response/request object.
  - `pSock`: socket.
- Does:
  - runs `XHTTP_ReadHeader()` and, only when that returns `XHTTP_PARSED`, continues with `XHTTP_ReadContent()`.
- Returns:
  - forwarded status from the two helpers.

### Blocking client helpers

#### `xhttp_status_t XHTTP_Exchange(xhttp_t *pRequest, xhttp_t *pResponse, xsock_t *pSock)`

- Arguments:
  - `pRequest`: already assembled request object.
  - `pResponse`: destination response object.
  - `pSock`: connected blocking socket.
- Does:
  - writes `pRequest->rawData` as-is.
  - forwards callback configuration from request to response.
  - receives the response.
- Returns:
  - `XHTTP_EFDMODE` for non-blocking sockets.
  - `XHTTP_EWRITE` on write failure.
  - `XHTTP_TERMINATED` when the write callback aborts.
  - otherwise the receive status.

#### `xhttp_status_t XHTTP_Connect(xhttp_t *pHttp, xsock_t *pSock, xlink_t *pLink)`

- Arguments:
  - `pHttp`: request object, mainly for callback/error reporting and optional Unix socket path.
  - `pSock`: destination socket object.
  - `pLink`: parsed link info.
- Does:
  - defaults missing protocol to `http`.
  - supports `http`, `https` and Unix-domain sockets via `pHttp->sUnixAddr`.
  - resolves the host, applies timeout, optionally inserts Basic auth from the link and opens the socket.
- Returns:
  - `XHTTP_CONNECTED` on success.
  - `XHTTP_EPROTO`, `XHTTP_ERESOLVE`, `XHTTP_EAUTH`, `XHTTP_ECONNECT`, `XHTTP_ETIMEO`, etc. on failure.

#### `xhttp_status_t XHTTP_LinkExchange(xhttp_t *pRequest, xhttp_t *pResponse, xlink_t *pLink)`

#### `xhttp_status_t XHTTP_EasyExchange(xhttp_t *pRequest, xhttp_t *pResponse, const char *pLink)`

- Arguments:
  - request/response objects plus a parsed or raw link.
- Does:
  - opens a temporary connection, exchanges the request and closes the socket.
- Returns:
  - exchange status.
  - `XHTTP_ELINK` when the raw link cannot be parsed.

#### `xhttp_status_t XHTTP_Perform(xhttp_t *pHttp, xsock_t *pSock, const uint8_t *pBody, size_t nLength)`

- Arguments:
  - `pHttp`: request object reused as the response object afterward.
  - `pSock`: connected blocking socket.
  - `pBody`, `nLength`: optional body to assemble.
- Does:
  - assembles the request.
  - writes it.
  - soft-resets the same object.
  - receives the response back into that object.
- Returns:
  - `XHTTP_EFDMODE`, `XHTTP_EASSEMBLE`, `XHTTP_EWRITE`, `XHTTP_TERMINATED`, or receive status.

#### `xhttp_status_t XHTTP_LinkPerform(xhttp_t *pHttp, xlink_t *pLink, const uint8_t *pBody, size_t nLength)`

#### `xhttp_status_t XHTTP_EasyPerform(xhttp_t *pHttp, const char *pLink, const uint8_t *pBody, size_t nLength)`

- Arguments:
  - request/response object plus parsed or raw link and optional body.
- Does:
  - wraps `XHTTP_Connect()` + `XHTTP_Perform()` + socket close.
- Returns:
  - perform status.
  - `XHTTP_ELINK` for invalid raw links.

#### `xhttp_status_t XHTTP_SoloPerform(xhttp_t *pHttp, xhttp_method_t eMethod, const char *pLink, const uint8_t *pBody, size_t nLength)`

- Arguments:
  - `pHttp`: object to initialize and reuse.
  - `eMethod`: request method.
  - `pLink`: raw URL.
  - `pBody`, `nLength`: optional body.
- Does:
  - parses the link.
  - initializes the request with the link URI.
  - auto-adds `Host` and `User-Agent: xutils/<version>`.
  - performs the request.
- Returns:
  - `XHTTP_EINIT` when request init fails.
  - `XHTTP_ESETHDR` on header insertion failure.
  - `XHTTP_EEXISTS` if `Host` or `User-Agent` already exists with a conflicting value.
  - otherwise the `LinkPerform()` status.
