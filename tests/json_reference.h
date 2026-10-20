/* libxutils tests: how JSON strings decode, spelled out apart from the library, for the tests that check it */
#ifndef XUTILS_TEST_JSON_REFERENCE_H
#define XUTILS_TEST_JSON_REFERENCE_H

#include <stdlib.h>
#include <string.h>

/* What a string the lexer accepted says, spelled out on its own: U+0000 is kept as C0 80 and a surrogate without
   its pair as the three bytes it would take, the way the library keeps them. pOut has room for nIn bytes. */
static size_t reference_unescape(char *pOut, const char *pIn, size_t nIn)
{
    size_t nOut = 0;
    for (size_t i = 0; i < nIn;)
    {
        if (pIn[i] != '\\')
        {
            pOut[nOut++] = pIn[i++];
            continue;
        }

        char c = pIn[i + 1];
        i += 2;
        if (c != 'u')
        {
            pOut[nOut++] = c == 'b' ? '\b' : c == 'f' ? '\f' : c == 'n' ? '\n' : c == 'r' ? '\r' : c == 't' ? '\t' : c;
            continue;
        }

        char sHex[5] = {pIn[i], pIn[i + 1], pIn[i + 2], pIn[i + 3], 0};
        unsigned long nCode = strtoul(sHex, NULL, 16);
        i += 4;

        if (nCode >= 0xD800 && nCode <= 0xDBFF && i + 6 <= nIn && pIn[i] == '\\' && pIn[i + 1] == 'u')
        {
            char sLow[5] = {pIn[i + 2], pIn[i + 3], pIn[i + 4], pIn[i + 5], 0};
            unsigned long nLow = strtoul(sLow, NULL, 16);
            if (nLow >= 0xDC00 && nLow <= 0xDFFF)
            {
                nCode = 0x10000 + ((nCode - 0xD800) << 10) + (nLow - 0xDC00);
                i += 6;
            }
        }

        unsigned char *pByte = (unsigned char*)&pOut[nOut];
        if (!nCode) { pByte[0] = 0xC0; pByte[1] = 0x80; nOut += 2; }
        else if (nCode < 0x80) { pByte[0] = (unsigned char)nCode; nOut += 1; }
        else if (nCode < 0x800) { pByte[0] = (unsigned char)(0xC0 | (nCode >> 6)); pByte[1] = (unsigned char)(0x80 | (nCode & 0x3F)); nOut += 2; }
        else if (nCode < 0x10000)
        {
            pByte[0] = (unsigned char)(0xE0 | (nCode >> 12));
            pByte[1] = (unsigned char)(0x80 | ((nCode >> 6) & 0x3F));
            pByte[2] = (unsigned char)(0x80 | (nCode & 0x3F));
            nOut += 3;
        }
        else
        {
            pByte[0] = (unsigned char)(0xF0 | (nCode >> 18));
            pByte[1] = (unsigned char)(0x80 | ((nCode >> 12) & 0x3F));
            pByte[2] = (unsigned char)(0x80 | ((nCode >> 6) & 0x3F));
            pByte[3] = (unsigned char)(0x80 | (nCode & 0x3F));
            nOut += 4;
        }
    }

    return nOut;
}

#endif /* XUTILS_TEST_JSON_REFERENCE_H */
