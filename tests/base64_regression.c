#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base64.h"

#include "test.h"

static int check_roundtrip(const uint8_t *pData, size_t nDataLen, const char *pLabel)
{
    size_t nEncLen = nDataLen;
    char *pEncoded = XBase64_Encrypt(pData, &nEncLen);
    CHECK(pEncoded != NULL, "base64 encode failed");

    size_t nDecLen = nEncLen;
    char *pDecoded = XBase64_Decrypt((const uint8_t*)pEncoded, &nDecLen);
    CHECK(pDecoded != NULL, "base64 decode failed");
    if (nDecLen != nDataLen)
    {
        fprintf(
            stderr, "base64_regression: %s length mismatch: got(%zu), want(%zu), enc(%s)\n", pLabel, nDecLen, nDataLen, pEncoded);
        return 1;
    }
    if (memcmp(pDecoded, pData, nDataLen) != 0)
    {
        fprintf(stderr, "base64_regression: %s bytes mismatch, enc(%s)\n", pLabel, pEncoded);
        return 1;
    }

    free(pDecoded);
    free(pEncoded);
    return 0;
}

static int check_url_roundtrip(const uint8_t *pData, size_t nDataLen, const char *pLabel)
{
    size_t nEncLen = nDataLen;
    char *pEncoded = XBase64_UrlEncrypt(pData, &nEncLen);
    CHECK(pEncoded != NULL, "base64url encode failed");

    size_t nDecLen = nEncLen;
    char *pDecoded = XBase64_UrlDecrypt((const uint8_t*)pEncoded, &nDecLen);
    CHECK(pDecoded != NULL, "base64url decode failed");
    if (nDecLen != nDataLen)
    {
        fprintf(
            stderr, "base64_regression: %s length mismatch: got(%zu), want(%zu), enc(%s)\n", pLabel, nDecLen, nDataLen, pEncoded);
        return 1;
    }
    if (memcmp(pDecoded, pData, nDataLen) != 0)
    {
        fprintf(stderr, "base64_regression: %s bytes mismatch, enc(%s)\n", pLabel, pEncoded);
        return 1;
    }

    free(pDecoded);
    free(pEncoded);
    return 0;
}

static int check_vector(const char *pPlain, const char *pExpected)
{
    size_t nLength = strlen(pPlain);
    char *pEncoded = XBase64_Encrypt((const uint8_t*)pPlain, &nLength);
    CHECK(pEncoded != NULL, "vector encode");
    CHECK(strcmp(pEncoded, pExpected) == 0, "vector encoded bytes");
    CHECK(nLength == strlen(pExpected), "vector encoded length");

    char *pDecoded = XBase64_Decrypt((const uint8_t*)pEncoded, &nLength);
    CHECK(pDecoded != NULL, "vector decode");
    CHECK(nLength == strlen(pPlain) && memcmp(pDecoded, pPlain, nLength) == 0, "vector decoded bytes");
    free(pDecoded);
    free(pEncoded);
    return 0;
}

static int reject_decode(const char *pEncoded)
{
    size_t nLength = strlen(pEncoded);
    char *pDecoded = XBase64_Decrypt((const uint8_t*)pEncoded, &nLength);
    if (pDecoded != NULL) free(pDecoded);
    return pDecoded == NULL;
}

static int XTest_boundaries(void)
{
    uint8_t key[32];
    uint8_t sig[64];

    for (size_t i = 0; i < sizeof(key); i++) key[i] = (uint8_t)(i + 1);
    key[sizeof(key) - 1] = 0;

    for (size_t i = 0; i < sizeof(sig); i++) sig[i] = (uint8_t)(0xa0 + i);
    sig[sizeof(sig) - 1] = 0;

    CHECK(check_roundtrip(key, sizeof(key), "32-byte trailing-zero roundtrip") == 0, "32-byte trailing-zero roundtrip");
    CHECK(check_roundtrip(sig, sizeof(sig), "64-byte trailing-zero roundtrip") == 0, "64-byte trailing-zero roundtrip");
    CHECK(check_url_roundtrip(sig, sizeof(sig), "url trailing-zero roundtrip") == 0, "url trailing-zero roundtrip");

    CHECK(check_vector("f", "Zg==") == 0, "RFC4648 f");
    CHECK(check_vector("fo", "Zm8=") == 0, "RFC4648 fo");
    CHECK(check_vector("foo", "Zm9v") == 0, "RFC4648 foo");
    CHECK(check_vector("foob", "Zm9vYg==") == 0, "RFC4648 foob");
    CHECK(check_vector("fooba", "Zm9vYmE=") == 0, "RFC4648 fooba");
    CHECK(check_vector("foobar", "Zm9vYmFy") == 0, "RFC4648 foobar");

    size_t nZero = 0;
    CHECK(XBase64_Encrypt((const uint8_t*)"x", &nZero) == NULL, "reject zero encode");
    CHECK(XBase64_Decrypt((const uint8_t*)"x", &nZero) == NULL, "reject zero decode");
    CHECK(reject_decode("A"), "reject remainder one");
    CHECK(reject_decode("===="), "reject excessive padding");
    CHECK(reject_decode("Z==="), "reject three padding bytes");
    CHECK(reject_decode("Zg="), "reject padded non-multiple of four");
    CHECK(reject_decode("Z=g="), "reject interior padding");
    CHECK(reject_decode("Zm$v"), "reject invalid alphabet");
    CHECK(reject_decode("Zm9v\n"), "reject whitespace");

    size_t nUrlLen = 3;
    const uint8_t sUrlBytes[] = {0xfb, 0xff, 0xef};
    char *pUrl = XBase64_UrlEncrypt(sUrlBytes, &nUrlLen);
    CHECK(pUrl != NULL && strcmp(pUrl, "-__v") == 0 && nUrlLen == 4, "base64url alphabet");
    free(pUrl);

    printf("base64_regression: OK\n");
    return 0;
}

static int XTest_length_overflow(void)
{
    /* A length whose encoding does not fit size_t is refused before anything is read. Four
       characters per three bytes wrapped round, and the encoding was written into a few bytes. */
    const uint8_t sInput[4] = "abc";
    size_t nLength = SIZE_MAX;
    CHECK(XBase64_Encrypt(sInput, &nLength) == NULL, "A length past what an encoding can hold is refused");
    nLength = SIZE_MAX / 4 * 3;
    CHECK(XBase64_UrlEncrypt(sInput, &nLength) == NULL, "By the URL form too");

    nLength = 3;
    char *pEncoded = XBase64_Encrypt(sInput, &nLength);
    CHECK(pEncoded != NULL && !strcmp(pEncoded, "YWJj"), "A real length still encodes");
    free(pEncoded);
    return 0;
}

/* What XBase64_Decrypt makes of pInput, against what XBase64_DecryptTo writes into a buffer of nSize bytes */
static int same_decoding(const uint8_t *pInput, size_t nLength, size_t nSize)
{
    uint8_t out[128];
    memset(out, 0xA5, sizeof(out));

    size_t nHeapLen = nLength;
    char *pHeap = XBase64_Decrypt(pInput, &nHeapLen);
    size_t nToLen = 0;
    xbool_t bTo = XBase64_DecryptTo(pInput, nLength, out, nSize, &nToLen);

    int nSame;
    if (pHeap == NULL) nSame = !bTo;
    else if (nHeapLen > nSize) nSame = !bTo;
    else nSame = bTo && nToLen == nHeapLen && !memcmp(out, pHeap, nHeapLen);

    /* What it did not write it left alone: nothing at all when it refused, nothing past the decoding when it did not */
    for (size_t i = bTo ? nToLen : 0; i < sizeof(out) && nSame; i++) nSame = out[i] == 0xA5;

    free(pHeap);
    return nSame;
}

static int XTest_decrypt_to(void)
{
    uint8_t out[8];
    size_t nOutLen = 0;

    CHECK(XBase64_DecryptTo((const uint8_t*)"Zm9vYmFy", 8, out, 6, &nOutLen) && nOutLen == 6 && !memcmp(out, "foobar", 6),
        "decodes into a buffer of exactly its size");
    CHECK(XBase64_DecryptTo((const uint8_t*)"Zm9vYg==", 8, out, sizeof(out), &nOutLen) && nOutLen == 4 &&
        !memcmp(out, "foob", 4), "padding is taken as XBase64_Decrypt takes it");
    CHECK(XBase64_DecryptTo((const uint8_t*)"Zm9vYg", 6, out, sizeof(out), &nOutLen) && nOutLen == 4 &&
        !memcmp(out, "foob", 4), "and so is padding left out");

    memset(out, 0x5A, sizeof(out));
    nOutLen = 99;
    CHECK(!XBase64_DecryptTo((const uint8_t*)"Zm9vYmFy", 8, out, 5, &nOutLen) && nOutLen == 99 && out[0] == 0x5A &&
        out[4] == 0x5A, "one byte short is refused, with nothing written");

    CHECK(!XBase64_DecryptTo(NULL, 4, out, sizeof(out), &nOutLen), "no input is refused");
    CHECK(!XBase64_DecryptTo((const uint8_t*)"Zm9v", 0, out, sizeof(out), &nOutLen), "an empty input is refused");
    CHECK(!XBase64_DecryptTo((const uint8_t*)"Zm9v", 4, NULL, sizeof(out), &nOutLen), "no buffer is refused");
    CHECK(!XBase64_DecryptTo((const uint8_t*)"Zm9v", 4, out, sizeof(out), NULL), "no length out is refused");

    const char *pRefused[] = { "A", "====", "Z===", "Zg=", "Z=g=", "Zm$v", "Zm9v\n", "=Zm9", "Zm9v====", "Zm 9" };
    for (size_t i = 0; i < sizeof(pRefused) / sizeof(pRefused[0]); i++)
    {
        CHECK(reject_decode(pRefused[i]), "XBase64_Decrypt refuses it");
        CHECK(!XBase64_DecryptTo((const uint8_t*)pRefused[i], strlen(pRefused[i]), out, sizeof(out), &nOutLen),
            "what XBase64_Decrypt refuses XBase64_DecryptTo refuses too");
    }

    /* Both decoders agree on everything: random text over the alphabet, '=' and a byte outside it, and real encodings
       with one character changed, decoded into buffers of every size around what they decode to */
    const char sChars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/==-_\n";
    uint64_t nState = 0x9E3779B97F4A7C15ULL;

    for (size_t nRound = 0; nRound < 200000; nRound++)
    {
        nState ^= nState << 13; nState ^= nState >> 7; nState ^= nState << 17;
        uint8_t sText[96];
        size_t nLength = 1 + (size_t)(nState % 64);

        if (nRound % 2)
        {
            for (size_t i = 0; i < nLength; i++)
            {
                nState ^= nState << 13; nState ^= nState >> 7; nState ^= nState << 17;
                sText[i] = (uint8_t)sChars[nState % (sizeof(sChars) - 1)];
            }
        }
        else
        {
            uint8_t raw[48];
            size_t nRaw = 1 + (size_t)(nState % 40);
            for (size_t i = 0; i < nRaw; i++) raw[i] = (uint8_t)(nState >> (i % 8 * 8)) ^ (uint8_t)i;

            size_t nEncLen = nRaw;
            char *pEncoded = XBase64_Encrypt(raw, &nEncLen);
            CHECK(pEncoded != NULL && nEncLen < sizeof(sText), "encode a value to change");
            memcpy(sText, pEncoded, nEncLen);
            free(pEncoded);
            nLength = nEncLen;

            /* Most of them changed in one place, some cut short, the rest left as they are */
            nState ^= nState << 13; nState ^= nState >> 7; nState ^= nState << 17;
            if (nState % 4 == 0) nLength -= nState % nLength;
            else if (nState % 4 != 1) sText[nState % nLength] = (uint8_t)sChars[(nState >> 8) % (sizeof(sChars) - 1)];
        }

        size_t nHeapLen = nLength;
        char *pHeap = XBase64_Decrypt(sText, &nHeapLen);
        size_t nExpect = pHeap != NULL ? nHeapLen : 48;
        free(pHeap);

        size_t nSizes[] = { nExpect, nExpect + 1, nExpect ? nExpect - 1 : 0, 128 };
        for (size_t i = 0; i < sizeof(nSizes) / sizeof(nSizes[0]); i++)
            CHECK(same_decoding(sText, nLength, nSizes[i]), "XBase64_DecryptTo decodes exactly what XBase64_Decrypt does");
    }

    return 0;
}

XTEST_MAIN(XTEST_CASE(boundaries), XTEST_CASE(length_overflow), XTEST_CASE(decrypt_to))
