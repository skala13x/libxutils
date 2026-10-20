# json.c

## Purpose

JSON parser, mutable object builder and formatter.

## Core Types

- `xjson_t`: parser state and root object.
- `xjson_obj_t`: one JSON node.
- `xjson_writer_t`: output formatter/writer.
- `xjson_format_t`: pretty/color formatting palette.

## Strings and Names

A string or a name is kept as what it says, never as it was written: the parser decodes every escape, and the
writer escapes whatever needs it. A string a caller builds is the same kind of value as a parsed one, so a
backslash in it is written as `\\` whatever follows it, and `XJSON_GetString()` and `XJSON_GetObject()` work
with plain text: `"\u006eame"` is the name `name`, and an object that spells one name two ways has it twice and
is refused like any other repeated name.

- `\uXXXX` escapes become UTF-8, and a surrogate pair one four byte character.
- Two values have no place in a C string of UTF-8, and are kept in bytes that are never valid UTF-8: U+0000 as
  `C0 80` and a surrogate that is not half of a pair as the three bytes it would take (`ED A0 80` to
  `ED BF BF`). The writer turns those back into `\u0000` and `\ud800` to `\udfff`, so the document reads back
  the same, and a caller puts either into a string the same way.
- Other bytes are taken and written as they are: input that is not valid UTF-8 is not refused.

## API Reference

### Constructors and object allocation

#### `xjson_obj_t *XJSON_FromStr(xpool_t *pPool, const char *pFmt, ...)`

- Formats input text, parses it as JSON and returns root object.
- Returns parsed root object or `NULL`.

#### `xjson_obj_t *XJSON_CreateObject(xpool_t *pPool, const char *pName, void *pValue, xjson_type_t nType)`

- Creates one node from already prepared payload storage.
- Returns node or `NULL`.

#### `xjson_obj_t *XJSON_NewObject / XJSON_NewArray(...)`

- Create object/array nodes backed by `xmap_t` / `xarray_t`.
- `nAllowUpdate` controls duplicate-key replacement on object insert.
- Return node or `NULL`.

#### `xjson_obj_t *XJSON_NewU64 / XJSON_NewU32 / XJSON_NewU16 / XJSON_NewInt / XJSON_NewFloat / XJSON_NewString / XJSON_NewBool / XJSON_NewNull(...)`

- Build primitive nodes from scalar input or string input.
- Number/bool/null values are stored as text internally.
- `XJSON_NewFloat` writes 15 significant digits, or as many more, up to 17, as it takes for the text to read back
  as the same double, with a point whatever the process locale, and `.0` after a whole number so that it reads
  back as a float. It refuses a value that is not finite.
- Return node or `NULL`.

#### `void XJSON_FreeObject(xjson_obj_t *pObj)`

- Frees one node recursively, including child map/array structures.

### Add / mutate helpers

#### `xjson_error_t XJSON_AddObject(xjson_obj_t *pDst, xjson_obj_t *pSrc)`

- For object destinations:
  - inserts by `pSrc->pName`.
  - replaces existing child only when `nAllowUpdate` is true.
- For array destinations:
  - appends child pointer without duplicating it.
- Returns:
  - `XJSON_ERR_NONE` on success.
  - `XJSON_ERR_EXITS`, `XJSON_ERR_ALLOC` or `XJSON_ERR_INVALID` on failure.

#### `xjson_error_t XJSON_AddU64 / AddU32 / AddU16 / AddInt / AddFloat / AddString / AddBool / AddNull(...)`

- Build a primitive child and insert it into `pObject`.
- Return `XJSON_ERR_NONE` or a specific error.

#### `xjson_error_t XJSON_AddStrIfUsed(xjson_obj_t *pObject, const char *pName, const char *pValue)`

- No-op success when `pValue` is null/empty.
- Otherwise behaves like `AddString`.

#### `void XJSON_SetStrict(xjson_obj_t *pObj, xbool_t bStrict)`

- Off by default. A strict object remembers when an add fails for want of memory (`XJSON_ERR_ALLOC`), and
  `XJSON_WriteObject()` - so every dump, format and `XJSON_Write()` - refuses it from then on: a document built
  without checking each add is never sent or saved with a member missing.
- Adds refused for other reasons (a name already taken, an unnamed member of an object, a non-finite float)
  do not count: they leave the document as it was.
- Containers created inside a strict object by `XJSON_GetOrCreateObject()` / `XJSON_GetOrCreateArray()`, or
  added to it, become strict too; a loss anywhere inside fails the whole document.
- `nStrict` reads `XJSON_STRICT_OFF`, `XJSON_STRICT_ON` or `XJSON_STRICT_LOST`. Calling the function again
  starts over, so a caller that put back what was lost can declare the object complete.

### Parser lifecycle

#### `void XJSON_Init(xjson_t *pJson)`

- Zeroes parser state and last-token info.

#### `int XJSON_Parse(xjson_t *pJson, xpool_t *pPool, const char *pData, size_t nSize)`

- Parses input whose first token must be `{` or `[`.
- Stores root object in `pJson->pRootObj`.
- Returns `XJSON_SUCCESS` (`1`) or `XJSON_FAILURE` (`0`).

#### `void XJSON_Destroy(xjson_t *pJson)`

- Frees root object and clears parser state.

#### `xbool_t XJSON_ScanFlat(const char *pData, size_t nSize, xjson_field_t *pFields, size_t nCount)`

- Reads the members named in `pFields` from a flat JSON object without building a tree or allocating.
- Uses the parser's own lexer: `XTRUE` means `XJSON_Parse()` accepts the same input. Every value must be a
  string, number, `true`, `false` or `null`, no name may repeat, and only whitespace may follow the object.
- Each field gets `nType` (`XJSON_TYPE_INVALID` when absent), and `pValue`/`nLength` pointing into `pData`:
  a string without its quotes and with its escapes as written, anything else as written. A name is matched by
  what it says, escapes decoded, the way `XJSON_GetObject()` finds it, and two spellings of one name repeat it.
- Returns `XFALSE` for invalid input and for what it does not handle - arrays, nested values, objects with more
  than `XJSON_SCAN_MEMBERS` members - so a caller that needs more falls back to `XJSON_Parse()`.

#### `size_t XJSON_GetErrorStr(xjson_t *pJson, char *pOutput, size_t nSize)`

- Formats the current parser error and byte offset into `pOutput`.
- Returns written length.

### Object and array access

#### `xarray_t *XJSON_GetObjects(xjson_obj_t *pObj)`

- Collects object members into an array of `xmap_pair_t` payload pointers.
- Returns array or `NULL`.

#### `xjson_obj_t *XJSON_GetObject(xjson_obj_t *pObj, const char *pName)`

- Returns named child from an object or `NULL`.

#### `xjson_obj_t *XJSON_GetArrayItem(xjson_obj_t *pObj, size_t nIndex)`

- Returns array child at index or `NULL`.

#### `size_t XJSON_GetArrayLength(xjson_obj_t *pObj)`

- Returns array length or `0`.

#### `int XJSON_RemoveArrayItem(xjson_obj_t *pObj, size_t nIndex)`

- Removes array item and frees that child object.
- Always returns `0`.

#### `xjson_obj_t *XJSON_GetOrCreateObject(xjson_obj_t *pObj, const char *pName, uint8_t nAllowUpdate)`

#### `xjson_obj_t *XJSON_GetOrCreateArray(xjson_obj_t *pObj, const char *pName, uint8_t nAllowUpdate)`

- Return existing named child when found.
- Otherwise create and insert a new object/array child.
- Return child pointer or `NULL`.

### Value extraction

#### `const char *XJSON_GetString(xjson_obj_t *pObj)`

- Returns the string, decoded (see Strings and Names).
- Returns `""` on type mismatch.

#### `int XJSON_GetInt(xjson_obj_t *pObj)`

#### `uint16_t XJSON_GetU16(xjson_obj_t *pObj)`

#### `uint32_t XJSON_GetU32(xjson_obj_t *pObj)`

#### `uint64_t XJSON_GetU64(xjson_obj_t *pObj)`

- Parse numeric text from a number node.
- Return `0` on type mismatch.

#### `double XJSON_GetFloat(xjson_obj_t *pObj)`

- Returns floating-point value or `0.0` on type mismatch.
- Reads the text with a point whatever the process locale (`LC_NUMERIC`).

#### `uint8_t XJSON_GetBool(xjson_obj_t *pObj)`

- Returns `1` when boolean text is `"true"`, else `0`.

### Writer / formatter

#### `void XJSON_FormatInit(xjson_format_t *pFormat)`

- Fills default ANSI/color formatting fields.

#### `int XJSON_InitWriter(xjson_writer_t *pWriter, xpool_t *pPool, char *pOutput, size_t nSize)`

- Initializes writer.
- When `pOutput == NULL` and `nSize > 0`, allocates output storage.
- Returns `1` on success, `0` on allocation failure.

#### `void XJSON_DestroyWriter(xjson_writer_t *pWriter)`

- Frees writer-owned output storage when allocated by `InitWriter`.

#### `int XJSON_WriteObject(xjson_obj_t *pObj, xjson_writer_t *pWriter)`

- Serializes one object/array/primitive into writer storage.
- Returns `XJSON_SUCCESS` or `XJSON_FAILURE`.

#### `int XJSON_Write(xjson_t *pJson, char *pOutput, size_t nSize)`

- Serializes `pJson->pRootObj` into caller buffer.
- Returns `XJSON_SUCCESS` or `XJSON_FAILURE`.

#### `char *XJSON_FormatObj / XJSON_Format(...)`

- Pretty-print object/root JSON with optional color formatting.
- Return allocated string or `NULL`.
- Store output length in optional `pLength`.

#### `char *XJSON_DumpObj / XJSON_Dump(...)`

- Compact-print object/root JSON.
- Return allocated string or `NULL`.

## Important Notes

- Objects use `xmap_t`; arrays use `xarray_t`.
- Strings and names are stored decoded, and written escaped (see Strings and Names).
- Writer return strings may be pool-backed when the source object/parser uses a pool.
