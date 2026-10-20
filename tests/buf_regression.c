/* libxutils: aliasing, fixed storage, pointer ownership and queue wraparound. */
#include "test.h"
#include "buf.h"
#include "str.h"
#include <unistd.h>
#include <fcntl.h>

static int XTest_aliasing(void)
{
    for (int nFast = 0; nFast < 2; nFast++)
    {
        xbyte_buffer_t buffer;
        XByteBuffer_Init(&buffer, 0, nFast);
        CHECK(XByteBuffer_Add(&buffer, (const uint8_t*)"abcd", 4) == 4, "Initialize aliased buffer");
        CHECK(XByteBuffer_Resize(&buffer, 5) == 5, "Force the next append to grow");
        CHECK(XByteBuffer_Add(&buffer, buffer.pData + 1, 3) == 7, "Append an interior slice while reallocating");
        CHECK(buffer.nUsed == 7 && memcmp(buffer.pData, "abcdbcd", 7) == 0, "Aliased append preserves the original slice");
        CHECK(XByteBuffer_Insert(&buffer, 2, buffer.pData + 1, 4) > 0, "Insert a slice across the insertion point");
        CHECK(buffer.nUsed == 11, "Insertion has the expected length");
        CHECK(memcmp(buffer.pData, "abbcdbcdbcd", 11) == 0, "Overlapping insertion copies the original bytes");
        CHECK(XByteBuffer_Add(&buffer, buffer.pData, SIZE_MAX) < 0, "Reject addition overflow before reading source");
        CHECK(buffer.nUsed == 11, "Failed growth preserves used length");
        CHECK(XByteBuffer_Remove(&buffer, 2, SIZE_MAX) == 9 && buffer.nUsed == 2, "Oversized removal clamps without overflow");
        XByteBuffer_Clear(&buffer);
    }
    return 0;
}

static int XTest_borrowed(void)
{
    uint8_t data[4] = {'a', 0, 'b', 0x5a};
    xbyte_buffer_t borrowed;
    XByteBuffer_Init(&borrowed, 0, XFALSE);
    CHECK(XByteBuffer_SetData(&borrowed, data, 3) > 0, "Borrow exact binary storage");
    CHECK(XByteBuffer_Terminate(&borrowed, 3) < 0 && data[3] == 0x5a, "Termination must not overrun borrowed capacity");
    XByteBuffer_Clear(&borrowed);
    CHECK(memcmp(data, "a\0bZ", sizeof(data)) == 0, "Clearing a view does not free or mutate the source");
    CHECK(XByteData_Dup(data, SIZE_MAX) == NULL, "Reject impossible duplicate length");
    return 0;
}

static int nCleared;
static void XTest_ClearData(void *pData)
{
    if (pData != NULL) nCleared++;
    free(pData);
}

static int XTest_pointer_ownership(void)
{
    xdata_buffer_t buffer;
    CHECK(XDataBuffer_Init(&buffer, 2, XTRUE) > 0, "Initialize fixed pointer storage");
    buffer.clearCb = XTest_ClearData;
    nCleared = 0;
    int *pFirst = (int*)malloc(sizeof(int)), *pSecond = (int*)malloc(sizeof(int));
    CHECK(pFirst && pSecond, "Allocate owned pointers");
    CHECK(XDataBuffer_Add(&buffer, pFirst) == 0 && XDataBuffer_Add(&buffer, pSecond) == 1, "Fill fixed pointer buffer");
    CHECK(XDataBuffer_Add(&buffer, pFirst) < 0 && buffer.nUsed == 2, "Full buffer rejects additional ownership");
    CHECK(XDataBuffer_Set(&buffer, UINT_MAX, pFirst) == NULL && buffer.nUsed == 2, "Invalid index preserves the container");
    CHECK(XDataBuffer_Pop(&buffer, 0) == pFirst && nCleared == 0, "Pop transfers ownership without cleanup");
    free(pFirst);
    CHECK(XDataBuffer_Get(&buffer, 0) == pSecond, "Pop compacts subsequent pointers");
    XDataBuffer_Destroy(&buffer);
    CHECK(nCleared == 1, "Destroy clears only the remaining owned pointer");
    CHECK(XDataBuffer_Init(&buffer, SIZE_MAX, 0) < 0, "Reject pointer-allocation multiplication overflow");
    XDataBuffer_Destroy(&buffer);
    return 0;
}

static int XTest_ring_model(void)
{
    for (size_t nCapacity = 1; nCapacity <= 7; nCapacity++)
    {
        xring_buffer_t ring;
        CHECK(XRingBuffer_Init(&ring, nCapacity) == (int)nCapacity, "Initialize a bounded ring");
        uint8_t reference[7];
        size_t nUsed = 0;
        uint32_t nSeed = 76531;
        for (size_t nStep = 0; nStep < 1000; nStep++)
        {
            nSeed = nSeed * 1664525u + 1013904223u;
            uint8_t nByte = (uint8_t)(nSeed >> 16), output = 0;
            if (nSeed % 3)
            {
                CHECK(XRingBuffer_AddDataAdv(&ring, &nByte, 1) == 1, "Advancing push accepts one packet");
                if (nUsed == nCapacity)
                {
                    nUsed--;
                    memmove(reference, reference + 1, nUsed);
                }
                reference[nUsed++] = nByte;
            }
            else
            {
                CHECK(XRingBuffer_Pop(&ring, &output, 1) == (nUsed ? 1 : 0), "Pop status agrees with queue model");
                if (nUsed)
                {
                    CHECK(output == reference[0], "Wraparound retains FIFO order");
                    nUsed--;
                    memmove(reference, reference + 1, nUsed);
                }
            }
            CHECK(ring.nUsed == nUsed, "Ring occupancy agrees after every operation");
        }
        XRingBuffer_Destroy(&ring);
    }
    return 0;
}


static int XTest_growth(void)
{
    /* A buffer grows on demand and keeps a spare byte for the terminator,
     * so the stored bytes are always usable as a C string too. */
    xbyte_buffer_t buffer;
    XByteBuffer_Init(&buffer, 0, 0);
    CHECK(XByteBuffer_HasData(&buffer) == XFALSE, "A fresh buffer holds nothing");
    CHECK(buffer.nUsed == 0 && buffer.pData == NULL, "A fresh buffer has no storage");

    CHECK(XByteBuffer_Add(&buffer, (const uint8_t*)"0123456789", 10) == 10, "The add reports the new length");
    CHECK(buffer.nUsed == 10, "The buffer holds what was added");
    CHECK(buffer.nSize > buffer.nUsed, "The capacity leaves room for a terminator");
    CHECK(buffer.pData[buffer.nUsed] == '\0', "The stored bytes are terminated");
    CHECK(XByteBuffer_HasData(&buffer) == XTRUE, "A filled buffer reports its data");

    /* Repeated appends accumulate in order. */
    for (int i = 0; i < 100; i++)
        CHECK(XByteBuffer_AddByte(&buffer, (uint8_t)('a' + i % 26)) > 0, "Every byte appends");
    CHECK(buffer.nUsed == 110, "Every appended byte is counted");
    CHECK(memcmp(buffer.pData, "0123456789", 10) == 0, "Growth preserves the earlier bytes");
    CHECK(buffer.pData[10] == 'a' && buffer.pData[11] == 'b', "Growth preserves the append order");

    /* Reserving grows the capacity without changing the contents. */
    size_t nBefore = buffer.nUsed;
    CHECK(XByteBuffer_Reserve(&buffer, 4096) > 0, "The buffer reserves more room");
    CHECK(buffer.nSize >= nBefore + 4096, "The reservation is at least what was asked for");
    CHECK(buffer.nUsed == nBefore, "Reserving does not change the length");

    /* Resizing down to the used length keeps the contents. */
    CHECK(XByteBuffer_Resize(&buffer, buffer.nUsed + 1) > 0, "The buffer resizes down");
    CHECK(buffer.nUsed == nBefore, "Resizing down to the length keeps every byte");
    CHECK(memcmp(buffer.pData, "0123456789", 10) == 0, "Resizing down keeps the contents");

    /* A reset empties the buffer but keeps its storage; a clear releases it. */
    size_t nCapacity = buffer.nSize;
    XByteBuffer_Reset(&buffer);
    CHECK(buffer.nUsed == 0, "A reset empties the buffer");
    CHECK(buffer.nSize == nCapacity && buffer.pData != NULL, "A reset keeps the storage");

    XByteBuffer_Clear(&buffer);
    CHECK(buffer.nUsed == 0 && buffer.nSize == 0, "A clear releases the storage");
    CHECK(buffer.pData == NULL, "A cleared buffer has no storage");

    /* Clearing twice is safe. */
    XByteBuffer_Clear(&buffer);
    CHECK(buffer.pData == NULL, "A repeatedly cleared buffer stays empty");
    return 0;
}

static int XTest_edit(void)
{
    /* Inserting in the middle shifts the tail; inserting past the end is
     * an append rather than a gap. */
    xbyte_buffer_t buffer;
    XByteBuffer_Init(&buffer, 0, 0);
    CHECK(XByteBuffer_Add(&buffer, (const uint8_t*)"0123456789", 10) == 10, "The buffer is filled");

    CHECK(XByteBuffer_Insert(&buffer, 5, (const uint8_t*)"XY", 2) == 12, "An insert reports the new length");
    CHECK(memcmp(buffer.pData, "01234XY56789", 12) == 0, "The insert shifts the tail out of the way");

    CHECK(XByteBuffer_Insert(&buffer, 0, (const uint8_t*)"<", 1) == 13, "An insert at the start succeeds");
    CHECK(buffer.pData[0] == '<', "The insert at the start lands first");

    CHECK(XByteBuffer_Insert(&buffer, 999, (const uint8_t*)">", 1) == 14, "An insert past the end appends");
    CHECK(buffer.pData[13] == '>', "The out of range insert landed at the end");

    /* Removing takes bytes out and reports how many it took. */
    CHECK(XByteBuffer_Remove(&buffer, 0, 1) == 1, "A removal reports what it took");
    CHECK(buffer.pData[0] == '0', "The removal closed the gap at the start");

    CHECK(XByteBuffer_Remove(&buffer, 5, 2) == 2, "A middle removal reports what it took");
    CHECK(memcmp(buffer.pData, "0123456789>", 11) == 0, "The middle removal closed the gap");

    /* A removal longer than the remainder is clamped to it. */
    CHECK(XByteBuffer_Remove(&buffer, 8, 999) == 3, "An oversized removal is clamped to the remainder");
    CHECK(buffer.nUsed == 8, "The clamped removal took only what was there");
    CHECK(buffer.pData[buffer.nUsed] == '\0', "The shortened buffer is still terminated");

    /* A removal outside the buffer takes nothing. */
    CHECK(XByteBuffer_Remove(&buffer, 999, 1) == 0, "A removal past the end takes nothing");
    CHECK(XByteBuffer_Remove(&buffer, 0, 0) == 0, "A zero length removal takes nothing");
    CHECK(buffer.nUsed == 8, "A refused removal leaves the length alone");

    /* Advancing drops bytes from the front and shrinks the storage. */
    CHECK(XByteBuffer_Advance(&buffer, 3) == 5, "Advancing reports the remaining length");
    CHECK(memcmp(buffer.pData, "34567", 5) == 0, "Advancing drops the leading bytes");

    CHECK(XByteBuffer_Advance(&buffer, 999) == 0, "Advancing past the end empties the buffer");
    CHECK(buffer.nUsed == 0, "The emptied buffer has no length");

    /* Terminating cuts the buffer at a position. */
    CHECK(XByteBuffer_Add(&buffer, (const uint8_t*)"abcdef", 6) == 6, "The buffer is refilled");
    CHECK(XByteBuffer_Terminate(&buffer, 3) > 0, "The buffer terminates at a position");
    CHECK(buffer.nUsed == 3, "Terminating cuts the length");
    CHECK(memcmp(buffer.pData, "abc", 3) == 0, "Terminating keeps the leading bytes");
    CHECK(buffer.pData[3] == '\0', "The cut buffer is terminated");

    CHECK(XByteBuffer_NullTerm(&buffer) > 0, "The buffer null terminates");
    CHECK(buffer.pData[buffer.nUsed] == '\0', "The terminator is past the last byte");

    XByteBuffer_Clear(&buffer);
    return 0;
}

static int XTest_accessors(void)
{
    /* Reading a byte inside the buffer returns it; outside returns zero
     * rather than whatever is next in memory. */
    xbyte_buffer_t buffer;
    XByteBuffer_Init(&buffer, 0, 0);
    CHECK(XByteBuffer_Add(&buffer, (const uint8_t*)"ABC", 3) == 3, "The buffer is filled");

    CHECK(XByteBuffer_GetByte(&buffer, 0) == 'A', "The first byte is readable");
    CHECK(XByteBuffer_GetByte(&buffer, 2) == 'C', "The last byte is readable");
    CHECK(XByteBuffer_GetByte(&buffer, 3) == 0, "The byte past the end reads as zero");
    CHECK(XByteBuffer_GetByte(&buffer, 999) == 0, "A byte far past the end reads as zero");

    /* The formatted append builds onto what is already there. */
    CHECK(XByteBuffer_AddFmt(&buffer, "-%d-%s", 42, "x") > 0, "A formatted append succeeds");
    CHECK(memcmp(buffer.pData, "ABC-42-x", 8) == 0, "The formatted append lands after the existing bytes");

    /* Appending another buffer copies its bytes. */
    xbyte_buffer_t other;
    XByteBuffer_Init(&other, 0, 0);
    CHECK(XByteBuffer_Add(&other, (const uint8_t*)"tail", 4) == 4, "The other buffer is filled");
    CHECK(XByteBuffer_AddBuff(&buffer, &other) > 0, "One buffer appends onto another");
    CHECK(buffer.nUsed == 12, "The appended length is the sum");
    CHECK(memcmp(&buffer.pData[8], "tail", 4) == 0, "The appended bytes are the other buffer's");

    /* The source is untouched by the append. */
    CHECK(other.nUsed == 4, "Appending leaves the source alone");
    XByteBuffer_Clear(&other);
    CHECK(memcmp(&buffer.pData[8], "tail", 4) == 0, "The destination outlives the source");

    /* Rejected arguments write nothing. */
    size_t nBefore = buffer.nUsed;
    CHECK(XByteBuffer_Add(&buffer, NULL, 5) == 0, "A missing source adds nothing");
    CHECK(XByteBuffer_Add(&buffer, (const uint8_t*)"x", 0) == 0, "A zero length source adds nothing");
    CHECK(XByteBuffer_Insert(&buffer, 0, NULL, 5) == 0, "A missing insert source adds nothing");
    CHECK(buffer.nUsed == nBefore, "A rejected call leaves the length alone");

    XByteBuffer_Clear(&buffer);
    return 0;
}

static int XTest_ownership_transfer(void)
{
    /* Set points the destination at the source's bytes without copying,
     * so the source keeps them and the destination owns no storage. */
    xbyte_buffer_t source, target;
    XByteBuffer_Init(&source, 0, 0);
    CHECK(XByteBuffer_Add(&source, (const uint8_t*)"payload", 7) == 7, "The source is filled");

    XByteBuffer_Init(&target, 0, 0);
    CHECK(XByteBuffer_Set(&target, &source) == 7, "Set reports the length it points at");
    CHECK(target.pData == source.pData, "Set aliases the source rather than copying");
    CHECK(target.nSize == 0, "An aliasing target owns no storage of its own");
    CHECK(source.nUsed == 7, "Set leaves the source holding its bytes");

    /* Own moves the storage across and leaves the source empty. */
    xbyte_buffer_t owner;
    XByteBuffer_Init(&owner, 0, 0);
    uint8_t *pMoved = source.pData;
    CHECK(XByteBuffer_Own(&owner, &source) > 0, "Own takes the storage");
    CHECK(owner.pData == pMoved, "The moved storage is the same allocation");
    CHECK(owner.nUsed == 7 && owner.nSize > 0, "The new owner holds the bytes and the capacity");
    CHECK(source.pData == NULL && source.nUsed == 0, "The source gave up its storage");

    CHECK(memcmp(owner.pData, "payload", 7) == 0, "The moved bytes are unchanged");
    XByteBuffer_Clear(&owner);

    /* Owning a caller allocation makes the buffer responsible for it. */
    uint8_t *pRaw = (uint8_t*)malloc(16);
    CHECK(pRaw != NULL, "A caller allocation is made");
    memcpy(pRaw, "owned", 5);

    xbyte_buffer_t adopted;
    XByteBuffer_Init(&adopted, 0, 0);
    CHECK(XByteBuffer_OwnData(&adopted, pRaw, 5) > 0, "The caller allocation is adopted");
    CHECK(adopted.pData == pRaw, "The adopted buffer points at the caller allocation");
    CHECK(memcmp(adopted.pData, "owned", 5) == 0, "The adopted bytes are readable");
    XByteBuffer_Clear(&adopted);

    /* Duplicating bytes produces an independent allocation. */
    const uint8_t original[] = {'d', 'u', 'p', 0x00, 0xff};
    uint8_t *pDup = XByteData_Dup(original, sizeof(original));
    CHECK(pDup != NULL && pDup != original, "The duplicate is a separate allocation");
    CHECK(memcmp(pDup, original, sizeof(original)) == 0, "The duplicate holds every byte including the NUL");
    free(pDup);

    CHECK(XByteData_Dup(NULL, 8) == NULL, "A missing source duplicates to nothing");
    CHECK(XByteData_Dup(original, 0) == NULL, "A zero length source duplicates to nothing");
    return 0;
}

static int XTest_heap_buffer(void)
{
    /* The heap buffer owns itself and clears the caller pointer. */
    xbyte_buffer_t *pBuffer = XByteBuffer_New(64, 0);
    CHECK(pBuffer != NULL, "A heap buffer is allocated");
    CHECK(pBuffer->nSize >= 64, "The heap buffer starts at the requested capacity");
    CHECK(XByteBuffer_Add(pBuffer, (const uint8_t*)"heap", 4) == 4, "The heap buffer accepts data");

    XByteBuffer_Free(&pBuffer);
    CHECK(pBuffer == NULL, "Freeing clears the caller pointer");

    XByteBuffer_Free(&pBuffer);
    XByteBuffer_Free(NULL);

    /* A zero sized heap buffer is still usable. */
    pBuffer = XByteBuffer_New(0, 0);
    CHECK(pBuffer != NULL, "A zero sized heap buffer is allocated");
    CHECK(XByteBuffer_Add(pBuffer, (const uint8_t*)"grown", 5) == 5, "The zero sized buffer grows on demand");
    XByteBuffer_Free(&pBuffer);
    return 0;
}

static int XTest_data_buffer(void)
{
    /* The pointer buffer stores what it is given without copying. */
    xdata_buffer_t buffer;
    CHECK(XDataBuffer_Init(&buffer, 4, 0) > 0, "The pointer buffer initializes");

    int values[6] = {10, 20, 30, 40, 50, 60};
    for (size_t i = 0; i < 3; i++)
        CHECK(XDataBuffer_Add(&buffer, &values[i]) == (int)i, "Each pointer is added at the next index");
    CHECK(buffer.nUsed == 3, "Every added pointer is counted");

    CHECK(XDataBuffer_Get(&buffer, 0) == &values[0], "The first pointer is returned as stored");
    CHECK(XDataBuffer_Get(&buffer, 2) == &values[2], "The last pointer is returned as stored");
    CHECK(XDataBuffer_Get(&buffer, 3) == NULL, "A pointer past the end is not there");
    CHECK(XDataBuffer_Get(&buffer, 999) == NULL, "A pointer far past the end is not there");

    /* Growing past the initial capacity keeps the earlier pointers. */
    for (size_t i = 3; i < 6; i++)
        CHECK(XDataBuffer_Add(&buffer, &values[i]) >= 0, "The buffer grows past its initial size");
    CHECK(buffer.nUsed == 6, "Every pointer survived the growth");
    CHECK(XDataBuffer_Get(&buffer, 0) == &values[0], "Growth preserves the first pointer");
    CHECK(XDataBuffer_Get(&buffer, 5) == &values[5], "Growth preserves the last pointer");

    /* Replacing returns what was there before. */
    CHECK(XDataBuffer_Set(&buffer, 0, &values[5]) == &values[0], "Replacing returns the previous pointer");
    CHECK(XDataBuffer_Get(&buffer, 0) == &values[5], "The replacement is now stored");

    /* Popping removes and returns. */
    void *pPopped = XDataBuffer_Pop(&buffer, 0);
    CHECK(pPopped == &values[5], "Popping returns the pointer at that index");
    CHECK(buffer.nUsed == 5, "Popping shortens the buffer");

    XDataBuffer_Clear(&buffer);
    CHECK(buffer.nUsed == 0, "Clearing empties the buffer");

    XDataBuffer_Destroy(&buffer);
    return 0;
}

static int XTest_ring_boundaries(void)
{
    /* The ring holds whole chunks in slots, not a stream of bytes: each
     * add occupies one slot and each pop consumes one, whatever fraction
     * of it the caller asked for. */
    xring_buffer_t ring;
    CHECK(XRingBuffer_Init(&ring, 4) > 0, "The ring initializes");

    CHECK(XRingBuffer_AddData(&ring, (const uint8_t*)"first", 5) > 0, "The first chunk is added");
    CHECK(XRingBuffer_AddData(&ring, (const uint8_t*)"second", 6) > 0, "The second chunk is added");
    CHECK(ring.nUsed == 2, "Each chunk occupies one slot");

    /* The view is of the front chunk only. */
    uint8_t *pData = NULL;
    size_t nSize = 0;
    CHECK(XRingBuffer_GetData(&ring, &pData, &nSize) > 0, "The ring hands back its front chunk");
    CHECK(nSize == 5 && memcmp(pData, "first", 5) == 0, "The front chunk is the one added first");

    /* Popping copies from the front chunk and then discards all of it. */
    uint8_t sPopped[16];
    memset(sPopped, 0, sizeof(sPopped));
    CHECK(XRingBuffer_Pop(&ring, sPopped, 3) == 3, "A partial pop reports what it copied");
    CHECK(memcmp(sPopped, "fir", 3) == 0, "The pop copies from the front of the chunk");
    CHECK(ring.nUsed == 1, "A pop consumes the whole chunk, not just what it copied");

    CHECK(XRingBuffer_GetData(&ring, &pData, &nSize) > 0, "The next chunk is now at the front");
    CHECK(nSize == 6 && memcmp(pData, "second", 6) == 0, "The next chunk is the one added second");

    /* Advancing discards the front chunk without copying it. */
    XRingBuffer_Advance(&ring);
    CHECK(ring.nUsed == 0, "Advancing consumed the last chunk");
    CHECK(XRingBuffer_GetData(&ring, &pData, &nSize) == 0, "An empty ring hands back nothing");
    CHECK(XRingBuffer_Pop(&ring, sPopped, sizeof(sPopped)) == 0, "An empty ring pops nothing");

    /* Filling every slot and then one more: the ring refuses rather than
     * silently dropping a chunk the caller believes it queued. */
    for (int i = 0; i < 4; i++)
        CHECK(XRingBuffer_AddData(&ring, (const uint8_t*)"x", 1) > 0, "Every slot accepts a chunk");
    CHECK(ring.nUsed == 4, "Every slot is occupied");
    CHECK(XRingBuffer_AddData(&ring, (const uint8_t*)"overflow", 8) <= 0, "A full ring refuses another chunk");
    CHECK(ring.nUsed == 4, "The refused chunk did not displace anything");

    /* Draining and refilling wraps around the slot array. */
    for (int i = 0; i < 4; i++) XRingBuffer_Advance(&ring);
    CHECK(ring.nUsed == 0, "The ring drains");

    for (int i = 0; i < 4; i++)
    {
        char sChunk[8];
        snprintf(sChunk, sizeof(sChunk), "w%d", i);
        CHECK(XRingBuffer_AddData(&ring, (const uint8_t*)sChunk, strlen(sChunk)) > 0,
            "The ring accepts chunks after wrapping");
    }

    for (int i = 0; i < 4; i++)
    {
        char sExpect[8];
        snprintf(sExpect, sizeof(sExpect), "w%d", i);
        CHECK(XRingBuffer_GetData(&ring, &pData, &nSize) > 0, "Every wrapped chunk is readable");
        CHECK(nSize == strlen(sExpect) && memcmp(pData, sExpect, nSize) == 0,
            "Wrapping preserves the order the chunks were added in");
        XRingBuffer_Advance(&ring);
    }

    /* Rejected arguments queue nothing. */
    CHECK(XRingBuffer_AddData(&ring, NULL, 4) <= 0, "A missing chunk is refused");
    CHECK(XRingBuffer_AddData(&ring, (const uint8_t*)"x", 0) <= 0, "An empty chunk is refused");
    CHECK(ring.nUsed == 0, "A refused chunk leaves the ring empty");

    XRingBuffer_Destroy(&ring);
    return 0;
}


static int XTest_string_and_stdin(void)
{
    /* A byte buffer can be filled from an xstring_t, which carries its own
     * length rather than relying on a terminator. That matters because the
     * two types are mixed on the HTTP and WebSocket paths, where a body may
     * legitimately contain a zero byte. */
    xstring_t str;
    XString_Init(&str, 0, 0);
    CHECK(XString_Append(&str, "%s", "head") > 0, "The string is built");

    xbyte_buffer_t buffer;
    XByteBuffer_Init(&buffer, 0, 0);

    CHECK(XByteBuffer_AddStr(&buffer, &str) > 0, "The string is appended");
    CHECK(buffer.nUsed == str.nLength, "The whole string went in");
    CHECK(memcmp(buffer.pData, "head", 4) == 0, "The bytes are the string's own");

    /* Appending again accumulates rather than replacing. */
    CHECK(XByteBuffer_AddStr(&buffer, &str) > 0, "The string is appended again");
    CHECK(buffer.nUsed == str.nLength * 2, "Both copies are in the buffer");

    /* A string holding a zero byte keeps its whole length: the buffer is
     * binary, so the terminator is data like any other. */
    xstring_t binary;
    XString_Init(&binary, 0, 0);
    CHECK(XString_Add(&binary, "a\0b", 3) > 0, "A string with an embedded zero is built");

    xbyte_buffer_t binBuffer;
    XByteBuffer_Init(&binBuffer, 0, 0);
    CHECK(XByteBuffer_AddStr(&binBuffer, &binary) > 0, "It is appended");
    CHECK(binBuffer.nUsed == 3, "All three bytes went in, the zero included");
    CHECK(memcmp(binBuffer.pData, "a\0b", 3) == 0, "The embedded zero survived");

    XByteBuffer_Clear(&binBuffer);
    XString_Clear(&binary);

    /* An empty string appends nothing and is not an error state. */
    xstring_t empty;
    XString_Init(&empty, 0, 0);
    size_t nBefore = buffer.nUsed;
    XByteBuffer_AddStr(&buffer, &empty);
    CHECK(buffer.nUsed == nBefore, "An empty string adds nothing");
    XString_Clear(&empty);

    XByteBuffer_Clear(&buffer);
    XString_Clear(&str);
    return 0;
}

static int XTest_read_stdin(void)
{
    /* Reading standard input to the end is how the command line tools take
     * piped data. It has to take the bytes whole, including zeroes, and it
     * has to stop at the end rather than spinning. */
    char sPath[128];
    snprintf(sPath, sizeof(sPath), "/tmp/xutils-stdin-%d.bin", (int)getpid());

    const uint8_t sPayload[] = {'p','i','p','e','d',0x00,0xff,'\n','t','a','i','l'};

    FILE *pFile = fopen(sPath, "wb");
    if (pFile == NULL)
    {
        printf("No temporary file, skipping\n");
        return 77;
    }

    size_t nWritten = fwrite(sPayload, 1, sizeof(sPayload), pFile);
    fclose(pFile);
    CHECK(nWritten == sizeof(sPayload), "The fixture is written");

    int nSaved = dup(STDIN_FILENO);
    CHECK(nSaved >= 0, "Standard input can be saved");

    int nFile = open(sPath, O_RDONLY);
    CHECK(nFile >= 0, "The fixture opens");

    xbyte_buffer_t buffer;
    XByteBuffer_Init(&buffer, 0, 0);

    int nRead = XSTDERR;
    if (dup2(nFile, STDIN_FILENO) >= 0)
    {
        clearerr(stdin);
        nRead = XByteBuffer_ReadStdin(&buffer);
    }

    close(nFile);
    dup2(nSaved, STDIN_FILENO);
    close(nSaved);
    clearerr(stdin);
    unlink(sPath);

    CHECK(nRead == (int)sizeof(sPayload), "Every byte of the input was read");
    CHECK(buffer.nUsed == sizeof(sPayload), "The buffer holds every byte");
    CHECK(memcmp(buffer.pData, sPayload, sizeof(sPayload)) == 0,
        "The bytes arrived unchanged, the zero and the high byte included");

    XByteBuffer_Clear(&buffer);

    CHECK(XByteBuffer_ReadStdin(NULL) == XSTDINV, "Reading into nothing is rejected");
    return 0;
}

static int XTest_borrowed_reset(void)
{
    /* A buffer set over someone else's data owns nothing: resetting it wrote
       a terminator into the first byte of that data. */
    char sData[] = "keep";
    xbyte_buffer_t buffer;
    XByteBuffer_Init(&buffer, 0, XFALSE);
    XByteBuffer_SetData(&buffer, (uint8_t*)sData, 4);
    XByteBuffer_Reset(&buffer);
    CHECK(buffer.nUsed == 0, "A reset borrowed buffer is empty");
    CHECK(strcmp(sData, "keep") == 0, "Resetting a borrowed buffer leaves the borrowed data alone");

    /* An owned buffer is still terminated at its start */
    XByteBuffer_Init(&buffer, 0, XFALSE);
    CHECK(XByteBuffer_Add(&buffer, (const uint8_t*)"abc", 3) == 3, "Fill an owned buffer");
    XByteBuffer_Reset(&buffer);
    CHECK(buffer.nUsed == 0 && buffer.pData[0] == '\0', "A reset owned buffer is an empty string");
    XByteBuffer_Clear(&buffer);

    /* Popping from the middle keeps the order of the rest */
    xdata_buffer_t data;
    CHECK(XDataBuffer_Init(&data, 8, XFALSE) > 0, "Create a data buffer");
    static int items[5] = { 0, 1, 2, 3, 4 };
    for (int i = 0; i < 5; i++) CHECK(XDataBuffer_Add(&data, &items[i]) == i, "Add an item");
    CHECK(XDataBuffer_Pop(&data, 1) == &items[1], "Pop an item from the middle");
    CHECK(data.nUsed == 4, "One item fewer is held");
    CHECK(XDataBuffer_Get(&data, 0) == &items[0] && XDataBuffer_Get(&data, 1) == &items[2] &&
        XDataBuffer_Get(&data, 3) == &items[4], "The remaining items keep their order");
    XDataBuffer_Destroy(&data);
    return 0;
}

static int XTest_api_guards(void)
{
    xbyte_buffer_t buffer, other;
    uint8_t sBorrowed[8] = "borrow";

    XByteBuffer_Clear(NULL);
    XByteBuffer_Reset(NULL);
    CHECK(XByteBuffer_SetData(NULL, sBorrowed, 6) == XSTDINV && XByteBuffer_Set(NULL, &buffer) == XSTDINV,
        "Nothing is set on no buffer");
    CHECK(XByteBuffer_OwnData(NULL, NULL, 0) == XSTDINV && XByteBuffer_Own(NULL, &buffer) == XSTDINV &&
        XByteBuffer_Own(&buffer, NULL) == XSTDINV, "Nothing is owned by no buffer");
    CHECK(!XByteBuffer_HasData(NULL), "No buffer holds no data");

    /* Resizing: to nothing, past what an int counts, and a borrowed buffer that already holds enough */
    CHECK(XByteBuffer_Init(&buffer, 16, XFALSE) > 0 && XByteBuffer_Add(&buffer, (uint8_t*)"0123456789", 10) == 10,
        "Fill a buffer");
    CHECK(XByteBuffer_Resize(&buffer, (size_t)INT_MAX + 1) == XSTDERR, "No size past an int");
    XByteBuffer_Clear(&buffer);
    CHECK(XByteBuffer_Init(&buffer, 16, XFALSE) > 0 && XByteBuffer_Add(&buffer, (uint8_t*)"0123456789", 10) == 10,
        "Fill it again");
    CHECK(XByteBuffer_Resize(&buffer, 4) == 4 && buffer.nUsed == 3 && !strcmp((char*)buffer.pData, "012"),
        "Shrinking keeps what fits with its terminator");
    CHECK(XByteBuffer_Resize(&buffer, 0) == XSTDNON && buffer.pData == NULL && !buffer.nSize, "Nothing is released");

    XByteBuffer_SetData(&buffer, sBorrowed, 6);
    CHECK(XByteBuffer_Resize(&buffer, 4) == 6, "A borrowed buffer is big enough for less than it holds");
    CHECK(XByteBuffer_NullTerm(&buffer) == XSTDERR, "But can not grow for a terminator");
    CHECK(XByteBuffer_Terminate(&buffer, 3) == XSTDOK && buffer.nUsed == 3 && !strcmp((char*)sBorrowed, "bor"),
        "It can be cut inside what it holds");

    buffer.pData = NULL;
    buffer.nUsed = 0;
    CHECK(XByteBuffer_Terminate(&buffer, 0) == XSTDERR, "An empty buffer is not terminated");

    /* What is added or inserted from the buffer itself must lie inside it */
    CHECK(XByteBuffer_Init(&buffer, 0, XFALSE) == XSTDNON && XByteBuffer_Add(&buffer, (uint8_t*)"abcdef", 6) == 6,
        "Fill a buffer");
    CHECK(XByteBuffer_Add(&buffer, buffer.pData + 4, 4) == XSTDERR, "No more of its own bytes than it holds are added");
    buffer.nStatus = XSTDOK;
    CHECK(XByteBuffer_Insert(&buffer, 1, buffer.pData + 4, 4) == XSTDERR, "Nor inserted");
    buffer.nStatus = XSTDOK;
    CHECK(XByteBuffer_Insert(&buffer, 1, buffer.pData + 4, 2) == 8 && !strcmp((char*)buffer.pData, "aefbcdef"),
        "Its own bytes that are there are inserted");
    CHECK(XByteBuffer_Insert(&buffer, 1, NULL, 2) == XSTDNON && XByteBuffer_Insert(&buffer, 1, (uint8_t*)"x", 0) == XSTDNON,
        "Nothing is inserted from nothing");
    CHECK(XByteBuffer_Insert(&buffer, 1, (uint8_t*)"x", INT_MAX) == XSTDERR, "Nor past an int");

    XByteBuffer_Init(&other, 0, XFALSE);
    CHECK(XByteBuffer_AddBuff(&buffer, &other) == XSTDERR, "An empty buffer adds nothing");
    xbyte_buffer_t *pOther = &other;
    XByteBuffer_Free(&pOther);
    CHECK(pOther == &other, "A buffer that was not allocated is cleared, not freed");
    XByteBuffer_Clear(&buffer);
    CHECK(!XByteBuffer_HasData(&buffer), "A cleared buffer holds nothing");

    /* Pointer buffers: their sizes, fixed or growing, holes, and shrinking once mostly empty */
    xdata_buffer_t data;
    CHECK(XDataBuffer_Init(&data, 0, XTRUE) == XSTDERR, "A fixed buffer needs a size");
    CHECK(XDataBuffer_Init(&data, (size_t)INT_MAX + 1, XFALSE) == XSTDERR, "Not one past an int");
    CHECK(XDataBuffer_Init(&data, 1, XTRUE) == 1 && XDataBuffer_Add(&data, sBorrowed) == 0, "A fixed buffer takes one");
    CHECK(XDataBuffer_Add(&data, sBorrowed) == XSTDERR, "And no more");
    CHECK(XDataBuffer_Set(&data, 5, sBorrowed) == NULL && data.nUsed == 1, "Nothing is set past its end");
    CHECK(XDataBuffer_Pop(&data, 3) == NULL && XDataBuffer_Pop(&data, 0) == sBorrowed && !data.nUsed,
        "Popping takes what is there");
    XDataBuffer_Destroy(&data);

    int nValues[64];
    CHECK(XDataBuffer_Init(&data, 0, XFALSE) == 1, "A growing buffer starts at one");
    for (int i = 0; i < 64; i++) CHECK(XDataBuffer_Add(&data, &nValues[i]) == i, "It grows");
    CHECK(data.nSize == 64, "To fit");
    CHECK(XDataBuffer_Set(&data, 63, NULL) == &nValues[63] && XDataBuffer_Pop(&data, 63) == NULL, "A hole pops nothing");
    for (int i = 0; i < 56; i++) CHECK(XDataBuffer_Pop(&data, 0) == &nValues[i], "Pop from the front");
    CHECK(data.nUsed == 8 && data.nSize < 64, "A mostly empty buffer shrinks");
    for (int i = 0; i < 7; i++) CHECK(XDataBuffer_Pop(&data, 0) == &nValues[56 + i], "And keeps its order");
    CHECK(XDataBuffer_Set(&data, 2, &nValues[0]) == NULL && data.nUsed == 3, "Setting past the end extends it");
    XDataBuffer_Destroy(&data);

    /* Ring buffers: sizes, empty and full, and slots never used */
    xring_buffer_t ring;
    uint8_t *pData = NULL;
    size_t nSize = 0;
    uint8_t sOut[8];
    CHECK(XRingBuffer_Init(&ring, 0) == 0 && XRingBuffer_Init(&ring, (size_t)INT_MAX + 1) == 0, "A ring needs a sane size");
    XRingBuffer_Update(&ring, 1);
    XRingBuffer_Advance(&ring);
    CHECK(XRingBuffer_Init(&ring, 2) == 2, "Make a ring of two");
    XRingBuffer_Update(&ring, 0);
    XRingBuffer_Advance(&ring);
    CHECK(ring.nUsed == 0, "Nothing is taken from an empty ring");
    CHECK(!XRingBuffer_GetData(&ring, &pData, &nSize) && !XRingBuffer_Pop(&ring, sOut, sizeof(sOut)), "Nor read");

    CHECK(XRingBuffer_AddData(&ring, (uint8_t*)"one", 3) == 3 && XRingBuffer_AddData(&ring, (uint8_t*)"two", 3) == 3,
        "Fill the ring");
    XRingBuffer_Update(&ring, 1);
    CHECK(ring.nUsed == 2 && XRingBuffer_AddData(&ring, (uint8_t*)"x", 1) == 0, "A full ring takes nothing");
    CHECK(!XRingBuffer_GetData(&ring, NULL, &nSize) && !XRingBuffer_GetData(&ring, &pData, NULL), "A read needs both outputs");
    CHECK(!XRingBuffer_Pop(&ring, NULL, 8) && !XRingBuffer_Pop(&ring, sOut, 0), "A pop needs room");
    CHECK(XRingBuffer_Pop(&ring, sOut, 2) == 2 && !memcmp(sOut, "on", 2), "A short pop takes what fits");

    size_t nSlot = ring.nFront;
    xbyte_buffer_t *pSlot = ring.pData[nSlot];
    ring.pData[nSlot] = NULL;
    CHECK(!XRingBuffer_GetData(&ring, &pData, &nSize), "A slot that is not there has no data");
    CHECK(XRingBuffer_Pop(&ring, sOut, sizeof(sOut)) == 0 && ring.nUsed == 0, "And pops as empty");
    ring.pData[nSlot] = pSlot;
    XRingBuffer_Destroy(&ring);
    return 0;
}

static int XTest_read_stdin_error(void)
{
    /* A read that fails is not the end of the input: both stopped fread(), and the failure used to come back as
       what had been read until then */
    int nSaved = dup(STDIN_FILENO);
    int nDir = open("/", O_RDONLY | O_DIRECTORY);
    CHECK(nSaved >= 0 && nDir >= 0, "Standard input can be saved, and a directory opened");

    xbyte_buffer_t buffer;
    XByteBuffer_Init(&buffer, 0, 0);
    CHECK(XByteBuffer_Add(&buffer, (const uint8_t*)"kept", 4) == 4, "The buffer holds something before");

    int nRead = 0;
    if (dup2(nDir, STDIN_FILENO) >= 0)
    {
        clearerr(stdin);
        nRead = XByteBuffer_ReadStdin(&buffer);
    }

    close(nDir);
    dup2(nSaved, STDIN_FILENO);
    close(nSaved);
    clearerr(stdin);

    CHECK(nRead == XSTDERR, "A failed read is reported");
    CHECK(buffer.nUsed == 4 && !memcmp(buffer.pData, "kept", 4), "And what the buffer held is left");
    XByteBuffer_Clear(&buffer);
    return 0;
}

static int XTest_set_ownership(void)
{
    /* A buffer that borrows storage gives up what it owned: it used to be dropped, and leaked */
    uint8_t sExternal[] = "external";
    xbyte_buffer_t buffer;
    XByteBuffer_Init(&buffer, 0, 0);
    CHECK(XByteBuffer_Add(&buffer, (const uint8_t*)"owned bytes", 11) == 11 && buffer.nSize > 0, "The buffer owns storage");
    CHECK(XByteBuffer_SetData(&buffer, sExternal, 8) == 8, "It borrows other storage");
    CHECK(buffer.pData == sExternal && buffer.nSize == 0 && buffer.nUsed == 8, "And holds that alone, owning nothing");

    xbyte_buffer_t source;
    XByteBuffer_Init(&source, 0, 0);
    CHECK(XByteBuffer_Add(&source, (const uint8_t*)"source", 6) == 6, "A source is filled");
    CHECK(XByteBuffer_Add(&buffer, (const uint8_t*)"x", 1) == XSTDERR, "A borrowing buffer does not grow");
    XByteBuffer_Init(&buffer, 0, 0);
    CHECK(XByteBuffer_Add(&buffer, (const uint8_t*)"owned again", 11) == 11, "It owns storage again");
    CHECK(XByteBuffer_Set(&buffer, &source) == 6 && buffer.pData == source.pData && !buffer.nSize,
        "Set gives up what it owned the same way");

    /* Borrowing part of its own storage frees nothing under the view: the storage stays the caller's to free */
    XByteBuffer_Init(&buffer, 0, 0);
    CHECK(XByteBuffer_Add(&buffer, (const uint8_t*)"0123456789", 10) == 10, "The buffer owns storage");
    uint8_t *pOwned = buffer.pData;
    CHECK(XByteBuffer_SetData(&buffer, pOwned + 2, 5) == 5 && !memcmp(buffer.pData, "23456", 5), "A view into it");
    CHECK(XByteBuffer_SetData(&buffer, pOwned, 10) == 10 && !memcmp(buffer.pData, "0123456789", 10), "Or all of it");
    free(pOwned);

    /* Neither borrowed nor owned storage, nothing given up */
    XByteBuffer_Init(&buffer, 0, 0);
    CHECK(XByteBuffer_SetData(&buffer, NULL, 0) == 0 && buffer.pData == NULL, "Borrowing nothing");
    XByteBuffer_Clear(&source);
    return 0;
}

static int XTest_own_self(void)
{
    /* A buffer handed itself keeps what it has: clearing it first lost the bytes it was to take */
    xbyte_buffer_t buffer;
    XByteBuffer_Init(&buffer, 0, 0);
    CHECK(XByteBuffer_Add(&buffer, (const uint8_t*)"mine", 4) == 4, "The buffer is filled");
    size_t nSize = buffer.nSize;
    uint8_t *pData = buffer.pData;
    CHECK(XByteBuffer_Own(&buffer, &buffer) == (int)nSize, "Owning itself reports what it has");
    CHECK(buffer.pData == pData && buffer.nUsed == 4 && !memcmp(buffer.pData, "mine", 4), "And keeps it");
    XByteBuffer_Clear(&buffer);
    return 0;
}

/* Whether a ring of two holds these, oldest first, looked at without taking anything out */
static int ring_holds(xring_buffer_t *pRing, const char *pFirst, const char *pSecond)
{
    if (pRing->nUsed != 2) return 0;
    const xbyte_buffer_t *pOldest = pRing->pData[pRing->nFront];
    const xbyte_buffer_t *pNewest = pRing->pData[(pRing->nFront + 1) % pRing->nSize];

    return pOldest->nUsed == strlen(pFirst) && !memcmp(pOldest->pData, pFirst, pOldest->nUsed) &&
           pNewest->nUsed == strlen(pSecond) && !memcmp(pNewest->pData, pSecond, pNewest->nUsed);
}

static int XTest_ring_eviction(void)
{
    /* A full ring drops its oldest element for a new one only once the new one is there: nothing to add used to drop
       it all the same */
    xring_buffer_t ring;
    CHECK(XRingBuffer_Init(&ring, 2) == 2, "A ring of two");
    CHECK(XRingBuffer_AddDataAdv(&ring, (const uint8_t*)"a", 1) == 1, "Add one");
    CHECK(XRingBuffer_AddDataAdv(&ring, (const uint8_t*)"b", 1) == 1, "Fill it");

    CHECK(XRingBuffer_AddDataAdv(&ring, NULL, 0) == 0, "Nothing is not added");
    CHECK(XRingBuffer_AddDataAdv(&ring, (const uint8_t*)"x", 0) == 0, "No bytes are not added");
    CHECK(XRingBuffer_AddDataAdv(&ring, NULL, 4) == 0, "Bytes that are not there are not added");
    CHECK(ring_holds(&ring, "a", "b"), "And the ring holds what it held");

    CHECK(XRingBuffer_AddDataAdv(&ring, (const uint8_t*)"cc", 2) == 2, "A new element in a full ring is added");
    CHECK(ring_holds(&ring, "b", "cc"), "In place of the oldest");
    CHECK(XRingBuffer_AddDataAdv(&ring, (const uint8_t*)"ddd", 3) == 3, "Again");
    CHECK(ring_holds(&ring, "cc", "ddd"), "Round the ring");

    /* A ring with no room at all takes nothing */
    xring_buffer_t empty;
    CHECK(XRingBuffer_Init(&empty, 0) == 0, "A ring of none");
    CHECK(XRingBuffer_AddDataAdv(&empty, (const uint8_t*)"z", 1) == 0, "Takes nothing");

    XRingBuffer_Destroy(&ring);
    XRingBuffer_Destroy(&empty);
    return 0;
}

static int XTest_spare_storage(void)
{
    /* The buffer's storage past its content is where an add copies to and where an insert moves the content after
       its point, so bytes there are refused and the buffer is left as it was. The terminator byte is such storage,
       and the buffer has to grow for it: the bytes used to be taken for bytes from elsewhere, and read from the
       storage the growth freed. */
    for (int nFast = 0; nFast < 2; nFast++)
    {
        xbyte_buffer_t buffer;
        XByteBuffer_Init(&buffer, 0, nFast);
        CHECK(XByteBuffer_Add(&buffer, (const uint8_t*)"0123456789", 10) == 10 && buffer.nSize > buffer.nUsed,
            "Fill a buffer");
        CHECK(XByteBuffer_Resize(&buffer, 11) == 11, "With no storage past its terminator byte");

        CHECK(XByteBuffer_Add(&buffer, buffer.pData + buffer.nUsed, 1) == XSTDERR, "Its terminator byte is not added");
        buffer.nStatus = XSTDOK;
        CHECK(XByteBuffer_Insert(&buffer, 1, buffer.pData + buffer.nUsed, 1) == XSTDERR, "Nor inserted");
        buffer.nStatus = XSTDOK;

        /* Nor is storage reserved further on, where the buffer would not even have to grow */
        CHECK(XByteBuffer_Reserve(&buffer, 64) > 0 && buffer.nSize >= buffer.nUsed + 64, "Reserve storage");
        CHECK(XByteBuffer_Add(&buffer, buffer.pData + buffer.nUsed + 16, 8) == XSTDERR, "Reserved storage is not added");
        buffer.nStatus = XSTDOK;
        CHECK(XByteBuffer_Add(&buffer, buffer.pData + buffer.nSize - 1, 1) == XSTDERR, "Nor its last byte");
        buffer.nStatus = XSTDOK;
        CHECK(XByteBuffer_Insert(&buffer, 1, buffer.pData + buffer.nUsed + 8, 4) == XSTDERR, "Nor inserted");
        buffer.nStatus = XSTDOK;
        CHECK(buffer.nUsed == 10 && !memcmp(buffer.pData, "0123456789", 11), "The buffer is left as it was");

        /* Its content still is added and inserted */
        CHECK(XByteBuffer_Add(&buffer, buffer.pData + 8, 2) == 12 && !memcmp(buffer.pData, "012345678989", 13),
            "Its own content is added");
        CHECK(XByteBuffer_Insert(&buffer, 0, buffer.pData + 10, 2) == 14 && !memcmp(buffer.pData, "89012345678989", 15),
            "And inserted");

        /* Validate arguments */
        CHECK(XByteBuffer_Insert(NULL, 4, (const uint8_t*)"test", 4) == XSTDINV, "Inserting data to NULL is rejected");
        CHECK(XByteBuffer_Insert(&buffer, 4, NULL, 4) == XSTDNON, "Inserting NULL to buffer is ignored");
        CHECK(XByteBuffer_Add(NULL, (const uint8_t*)"test", 4) == XSTDINV, "Adding data to NULL is rejected");
        CHECK(XByteBuffer_Add(&buffer, NULL, 4) == XSTDNON, "Adding NULL to buffer is ignored");

        XByteBuffer_Clear(&buffer);
    }
    return 0;
}

static int XTest_null_buffers(void)
{
    /* Every function refuses a missing buffer, and reads nothing else it was given */
    uint8_t sData[4] = "abc";
    uint8_t *pData = NULL;
    size_t nSize = 0;

    xbyte_buffer_t buffer;
    xstring_t str;
    XByteBuffer_Init(&buffer, 0, XFALSE);
    CHECK(XString_Init(&str, 0, 0) >= 0, "Make a string");

    CHECK(XByteBuffer_Init(NULL, 8, XFALSE) == XSTDINV, "No buffer is initialized");
    CHECK(XByteBuffer_Set(&buffer, NULL) == XSTDINV && buffer.pData == NULL, "No source is set on a buffer");

    /* Room past what an int counts is refused before anything is allocated */
    xbyte_buffer_t big;
    CHECK(XByteBuffer_Init(&big, 0, XFALSE) >= 0 && XByteBuffer_AddByte(&big, 'x') == 1, "A buffer of one byte");
    CHECK(XByteBuffer_Reserve(&big, (size_t)INT_MAX) == XSTDERR && big.nStatus == XSTDERR, "Is not grown past an int");
    XByteBuffer_Clear(&big);
    const char *pNoFormat = NULL;
    CHECK(XByteBuffer_AddFmt(&buffer, pNoFormat) == XSTDINV && !buffer.nUsed, "Nothing is formatted without a format");
    CHECK(XByteBuffer_Resize(NULL, 8) == XSTDINV && XByteBuffer_Reserve(NULL, 8) == XSTDINV, "Nor grown");
    CHECK(XByteBuffer_Terminate(NULL, 0) == XSTDINV, "Nor terminated");
    CHECK(XByteBuffer_Add(NULL, sData, 3) == XSTDINV && XByteBuffer_AddFmt(NULL, "%s", "abc") == XSTDINV,
        "Nothing is added to it");
    CHECK(XByteBuffer_AddStr(NULL, &str) == XSTDINV && XByteBuffer_AddBuff(NULL, &buffer) == XSTDINV,
        "Not from a string or a buffer either");
    CHECK(XByteBuffer_ReadStdin(NULL) == XSTDINV, "Nor read from the input");
    CHECK(XByteBuffer_Insert(NULL, 0, sData, 3) == XSTDINV, "Nor inserted");
    CHECK(XByteBuffer_Remove(NULL, 0, 1) == XSTDINV && XByteBuffer_Delete(NULL, 0, 1) == XSTDINV &&
        XByteBuffer_Advance(NULL, 1) == XSTDINV, "Nothing is taken from it");

    /* These have no check of their own: the growth they ask for refuses it */
    CHECK(XByteBuffer_AddByte(NULL, 'x') == XSTDERR && XByteBuffer_NullTerm(NULL) == XSTDERR,
        "No byte or terminator is put in it");
    (void)XByteBuffer_GetByte(NULL, 0);

    /* A missing source is refused as well, and leaves the buffer as it was */
    CHECK(XByteBuffer_Add(&buffer, sData, 3) == 3, "Fill a buffer");
    CHECK(XByteBuffer_AddStr(&buffer, NULL) == XSTDINV && XByteBuffer_AddBuff(&buffer, NULL) == XSTDINV,
        "No string or buffer that is not there is added");
    CHECK(buffer.nUsed == 3 && !memcmp(buffer.pData, "abc", 4) && buffer.nStatus >= 0, "The buffer is as it was");

    /* A buffer set to itself keeps what it has, its own storage included */
    const uint8_t *pStorage = buffer.pData;
    size_t nStorage = buffer.nSize;
    CHECK(XByteBuffer_Set(&buffer, &buffer) == 3, "Setting a buffer to itself reports what it holds");
    CHECK(buffer.pData == pStorage && buffer.nSize == nStorage && !memcmp(buffer.pData, "abc", 4),
        "And keeps owning it");
    XByteBuffer_Clear(&buffer);
    XString_Clear(&str);

    xbyte_buffer_t *pNone = NULL;
    XByteBuffer_Free(NULL);
    XByteBuffer_Free(&pNone);

    /* Pointer buffers */
    CHECK(XDataBuffer_Init(NULL, 4, XFALSE) == XSTDINV, "No pointer buffer is initialized");
    CHECK(XDataBuffer_Add(NULL, sData) == XSTDINV && XDataBuffer_Set(NULL, 0, sData) == NULL,
        "Nothing is put in one");
    CHECK(XDataBuffer_Get(NULL, 0) == NULL && XDataBuffer_Pop(NULL, 0) == NULL, "Nor taken from one");
    XDataBuffer_Clear(NULL);
    XDataBuffer_Destroy(NULL);

    /* Ring buffers */
    CHECK(XRingBuffer_Init(NULL, 4) == XSTDINV, "No ring is initialized");
    CHECK(XRingBuffer_AddData(NULL, sData, 3) == XSTDINV && XRingBuffer_AddDataAdv(NULL, sData, 3) == XSTDINV,
        "Nothing is added to one");
    CHECK(XRingBuffer_GetData(NULL, &pData, &nSize) == XSTDINV && pData == NULL && !nSize,
        "Nothing is handed out of one");
    CHECK(XRingBuffer_Pop(NULL, sData, sizeof(sData)) == XSTDINV && !memcmp(sData, "abc", 4), "Nor popped");
    XRingBuffer_Update(NULL, XTRUE);
    XRingBuffer_Advance(NULL);
    XRingBuffer_Reset(NULL);
    XRingBuffer_Destroy(NULL);
    return 0;
}

XTEST_MAIN(
    XTEST_CASE(api_guards),
    XTEST_CASE(null_buffers),
    XTEST_CASE(aliasing),
    XTEST_CASE(spare_storage),
    XTEST_CASE(borrowed_reset),
    XTEST_CASE(borrowed),
    XTEST_CASE(pointer_ownership),
    XTEST_CASE(ring_model),
    XTEST_CASE(growth),
    XTEST_CASE(edit),
    XTEST_CASE(accessors),
    XTEST_CASE(ownership_transfer),
    XTEST_CASE(heap_buffer),
    XTEST_CASE(data_buffer),
    XTEST_CASE(ring_boundaries),
    XTEST_CASE(string_and_stdin),
    XTEST_CASE(read_stdin),
    XTEST_CASE(read_stdin_error),
    XTEST_CASE(set_ownership),
    XTEST_CASE(own_self),
    XTEST_CASE(ring_eviction)
)
