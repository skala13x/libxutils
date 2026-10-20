/*!
 *  @file libxutils/src/data/str.h
 *
 *  This source is part of the "libxutils" project
 *  2015-2020  Sandro Kalatozishvili (s.kalatoz@gmail.com)
 *
 * @brief This source includes string operations.
 */

#ifndef __XUTILS_XSTR_H__
#define __XUTILS_XSTR_H__

#include "xstd.h"
#include "array.h"
#include "pool.h"

/* Supported colors */
#define XSTR_CLR_NONE               "\x1B[0m"
#define XSTR_CLR_RED                "\x1B[31m"
#define XSTR_CLR_GREEN              "\x1B[32m"
#define XSTR_CLR_YELLOW             "\x1B[33m"
#define XSTR_CLR_BLUE               "\x1B[34m"
#define XSTR_CLR_MAGENTA            "\x1B[35m"
#define XSTR_CLR_CYAN               "\x1B[36m"
#define XSTR_CLR_WHITE              "\x1B[37m"

#define XSTR_CLR_LIGHT_RED          "\x1B[31;1m"
#define XSTR_CLR_LIGHT_GREEN        "\x1B[32;1m"
#define XSTR_CLR_LIGHT_YELLOW       "\x1B[33;1m"
#define XSTR_CLR_LIGHT_BLUE         "\x1B[34;1m"
#define XSTR_CLR_LIGHT_MAGENTA      "\x1B[35;1m"
#define XSTR_CLR_LIGHT_CYAN         "\x1B[36;1m"
#define XSTR_CLR_LIGHT_WHITE        "\x1B[37;1m"

/* Supported background colors */
#define XSTR_BACK_BLACK             "\x1B[40m"
#define XSTR_BACK_RED               "\x1B[41m"
#define XSTR_BACK_GREEN             "\x1B[42m"
#define XSTR_BACK_YELLOW            "\x1B[43m"
#define XSTR_BACK_BLUE              "\x1B[44m"
#define XSTR_BACK_MAGENTA           "\x1B[45m"
#define XSTR_BACK_CYAN              "\x1B[46m"
#define XSTR_BACK_WHITE             "\x1B[47m"

/* Supported string formats */
#define XSTR_FMT_BOLD               "\x1B[1m"
#define XSTR_FMT_DIM                "\x1B[2m"
#define XSTR_FMT_ITALIC             "\x1B[3m"
#define XSTR_FMT_ULINE              "\x1B[4m"
#define XSTR_FMT_FLICK              "\x1B[5m"
#define XSTR_FMT_BLINK              "\x1B[6m"
#define XSTR_FMT_HIGHLITE           "\x1B[7m"
#define XSTR_FMT_HIDE               "\x1B[8m"
#define XSTR_FMT_CROSS              "\x1B[9m"
#define XSTR_FMT_RESET              XSTR_CLR_NONE
#define XSTR_SCREEN_CLEAR           "\033[H\033[J"

#define XSTR_DEGREE_SYMBOL          "\xC2\xB0"

#define XSTR_DOUBLE_ARG_MAX         309
#define XSTR_INT_ARG_MAX            32
#define XSTR_LETTERS                52

#define XSTR_MAX                    8192
#define XSTR_BIG                    4096
#define XSTR_MID                    2048
#define XSTR_SUB                    1024
#define XSTR_MIN                    512
#define XSTR_TINY                   256
#define XSTR_MICRO                  32
#define XSTR_PICO                   16
#define XSTR_NPOS                   UINT_MAX
#define XSTR_STACK                  XSTR_BIG

#define XSTR_SPACE_CHAR             ' '
#define XSTR_NEW_LINE               "\n"
#define XSTR_SPACE                  " "
#define XSTR_EMPTY                  ""
#define XSTR_NUL                    '\0'
#define XSTR_INIT                   { XSTR_NUL }

#define XSTR_AVAIL(arr)((int)sizeof(arr)-(int)strlen(arr))
#define XSTR_NAVAIL(arr,n)((int)n-(int)strlen(arr))

#define XSTRFY_RAW(x)               #x
#define XSTRFY(x)                   XSTRFY_RAW(x)

#define XSTRCPYFMT(dst, fmt, len)                   \
    do {                                            \
        va_list args;                               \
        va_start(args, fmt);                        \
        dst = xstracpyargs(fmt, args, len);         \
        va_end(args);                               \
    }                                               \
    while (XSTDNON)

#define XSTRNCPYFMT(dst, size, fmt, len)            \
    do {                                            \
        va_list args;                               \
        va_start(args, fmt);                        \
        *len = xstrncpyarg(dst, size, fmt, args);   \
        va_end(args);                               \
    }                                               \
    while (XSTDNON)

#ifdef __cplusplus
extern "C" {
#endif

typedef char xstr_max_t[XSTR_MAX];
typedef char xstr_big_t[XSTR_BIG];
typedef char xstr_mid_t[XSTR_MID];
typedef char xstr_sub_t[XSTR_SUB];
typedef char xstr_min_t[XSTR_MIN];
typedef char xstr_tiny_t[XSTR_TINY];
typedef char xstr_micro_t[XSTR_MICRO];
typedef char xstr_pico_t[XSTR_PICO];

typedef enum {
    XSTR_LOWER = 0,
    XSTR_UPPER
} xstr_case_t;

/////////////////////////////////////////////////////////////////////////
// C string functions
//
// Compare and match functions return XTRUE for "equal" or "matches",
// unlike strcmp(). Search functions return an index where 0 is a match
// and XSTDERR (-1) is "not found". See each function for the details.
/////////////////////////////////////////////////////////////////////////

/*!
 * @brief Splits pString at the delimiter string pDlmt and appends the tokens to pTokens.
 * @return Number of entries pTokens has now, the ones it had before included, not the number added; 0 for invalid
 *         arguments or on failure (the added tokens are removed again).
 */
size_t xstrsplita(const char *pString, const char *pDlmt, xarray_t *pTokens, xbool_t bIncludeDlmt, xbool_t bIncludeEmpty);

/*!
 * @brief Splits pString at the delimiter string pDlmt into a new array, skipping empty tokens.
 * @return The array (XArray_Destroy() frees it), or NULL when there is no token at all.
 */
xarray_t* xstrsplit(const char *pString, const char *pDlmt);

/*!
 * @brief Like xstrsplit(), with every delimiter as a token of its own.
 * @return The array (XArray_Destroy() frees it), or NULL when there is no token at all.
 */
xarray_t* xstrsplitd(const char *pString, const char *pDlmt);

/*!
 * @brief Like xstrsplit(), keeping empty tokens as empty strings.
 * @return The array (XArray_Destroy() frees it), or NULL when there is no token at all.
 */
xarray_t* xstrsplite(const char *pString, const char *pDlmt);

/*!
 * @brief Replaces every pRep of pOrig with pWith into a new malloc() buffer to free.
 * @return The new string, or NULL for a NULL argument or on failure. An empty pRep replaces nothing.
 */
char* xstrrep(const char *pOrig, const char *pRep, const char *pWith);

/*!
 * @brief Replaces every pRep of pOrig with pWith into pDst.
 * @return XSTDOK when the whole result fit, XSTDNON (0) when it did not (pDst holds a cut result),
 *         XSTDINV for a NULL argument.
 */
int xstrnrep(char *pDst, size_t nSize, const char *pOrig, const char *pRep, const char *pWith);

char *xstrpdup(xpool_t *pPool, const char *pStr);
char* xstrdup(const char *pStr);

/*!
 * @brief Formats into a new malloc() buffer, the caller frees it.
 * @return The new string, or NULL on failure and also when the result is empty.
 */
char* xstracpy(const char *pFmt, ...);

/*!
 * @brief Formats into a new malloc() buffer, the caller frees it; *nSize gets its length.
 * @return The new string, or NULL (*nSize 0) on failure and also when the result is empty.
 */
char* xstracpyn(size_t *nSize, const char *pFmt, ...);

char* xstralloc(size_t nSize);

/*!
 * @brief vsnprintf() into pDest.
 * @return Length written, cut to nSize - 1, or XSTDERR (-1) for a NULL pDest or a zero nSize.
 */
int xstrncpyarg(char *pDest, size_t nSize, const char *pFmt, va_list args);

/*!
 * @brief Formats into a new pPool buffer (xfree() it); *nDstLength gets its length.
 * @return The new string, or NULL (*nDstLength 0) on failure and also when the result is empty.
 */
char* xstrpcpyargs(xpool_t *pPool, const char *pFmt, va_list args, size_t *nDstLength);

/*!
 * @brief Formats into a new malloc() buffer, the caller frees it.
 * @return The new string, or NULL on failure and also when the result is empty.
 */
char* xstracpyarg(const char *pFmt, va_list args);

/*!
 * @brief Formats into a new malloc() buffer, the caller frees it; *nDstLength gets its length.
 * @return The new string, or NULL (*nDstLength 0) on failure and also when the result is empty.
 */
char* xstracpyargs(const char *pFmt, va_list args, size_t *nDstLength);

size_t xstrarglen(const char *pFmt, va_list args);

/*!
 * @brief Formats into a new buffer at *pDst with vasprintf(), the caller frees it.
 * @return Length of the string, 0 on failure; without _XUTILS_USE_GNU 0 and *pDst is left unset.
 */
size_t xstrxcpyf(char **pDst, const char *pFmt, ...);

/*!
 * @brief Formats into a new buffer with vasprintf(), the caller frees it.
 * @return The new string, or NULL on failure, for an empty result and always without _XUTILS_USE_GNU.
 */
char* xstrxcpy(const char *pFmt, ...);

/*!
 * @brief Wildcard match against a list of patterns like "*.c;*.h", pDelimiter NULL means ";".
 * @return XTRUE when any of the patterns matches, XFALSE otherwise.
 * @note An empty string matches only an empty list. A list that starts with the delimiter is
 *       used as one pattern, the delimiter included.
 */
xbool_t xstrmatchm(const char *pStr, size_t nLength, const char *pPattern, const char *pDelimiter);

/*!
 * @brief Whole string wildcard match: '*' is any run of characters, '?' one character.
 * @return XTRUE when pStr[0, nLength) matches pPattern[0, nPatternLength), XFALSE otherwise.
 */
xbool_t xstrnmatch(const char *pStr, size_t nLength, const char *pPattern, size_t nPatternLength);

/*!
 * @brief Wildcard match like xstrnmatch() with a NUL terminated pattern.
 * @return XTRUE on a match, XFALSE otherwise.
 * @note An empty string (nLength 0) matches only an empty pattern, not even "*" (xstrnmatch() does).
 */
xbool_t xstrmatch(const char *pStr, size_t nLength, const char *pPattern);

/*!
 * @brief Simple '*' matcher, not a regular expression.
 * @return XTRUE on a match, XFALSE otherwise.
 * @note Each literal part matches at its first occurrence only:
 *       "*a" does not match "xaxa". xstrmatch() matches wildcards properly.
 */
xbool_t xstrregex(const char *pStr, size_t nLength, const char *pPattern);

/*!
 * @brief Like xstrncmp(), ignoring the case of ASCII letters.
 * @return XTRUE when the first nCmpLen characters are equal, XFALSE otherwise, also for nCmpLen 0 or NULL.
 */
xbool_t xstrncasecmp(const char *pStr, const char *pCmp, size_t nCmpLen);

/*!
 * @brief Equality test of two strings of the given lengths.
 * @return XTRUE when nStrLen == nCmpLen and the strings are equal over that length, XFALSE otherwise, also for a zero
 *         length or NULL.
 * @note Compares with strncmp(), which stops at a NUL byte: not binary safe.
 */
xbool_t xstrncmpn(const char *pStr, size_t nStrLen, const char *pCmp, size_t nCmpLen);

/*!
 * @brief Equality test of the first nCmpLen characters with strncmp() rules, so a prefix test.
 * @return XTRUE when they are equal, XFALSE when they differ, nCmpLen is 0 or a pointer is NULL.
 * @note Not strncmp(): XTRUE (1) means equal, xstrncmp("abcdef", "abc", 3) is XTRUE.
 */
xbool_t xstrncmp(const char *pStr, const char *pCmp, size_t nCmpLen);

/*!
 * @brief Equality test of two C strings, not an ordering like strcmp().
 * @return XTRUE when the strings are equal, XFALSE when they differ or either is NULL.
 */
xbool_t xstrcmp(const char *pStr, const char *pCmp);

/*!
 * @brief Copies pSrc into pDst, cut to nSize - 1 characters and always NUL terminated.
 * @return Number of characters copied (not strlen(pSrc) like strlcpy()),
 *         0 for a NULL or empty pSrc (pDst is emptied) or a NULL / zero sized pDst.
 */
size_t xstrncpy(char *pDst, size_t nSize, const char* pSrc);

size_t xstrncpys(char *pDst, size_t nDstSize, const char *pSrc, size_t nSrcLen);

/*!
 * @brief snprintf() into pDst.
 * @return Length written, cut to nSize - 1 (not the full length like snprintf(),
 *         so a cut can not be told from it), or (size_t)-1 for a NULL pDst or a zero nSize.
 */
size_t xstrncpyf(char *pDst, size_t nSize, const char *pFmt, ...);

/*!
 * @brief Appends formatted text after the string in pDst, a buffer of nSize bytes.
 * @return Number of characters appended, not the new length of pDst.
 */
size_t xstrncat(char *pDst, size_t nSize, const char *pFmt, ...);

/*!
 * @brief Appends formatted text after the string in pDst, nAvail being the space left in it.
 * @return The space left after appending, to pass to the next call; not a length.
 */
size_t xstrncatf(char *pDst, size_t nAvail, const char *pFmt, ...);

/*!
 * @brief Appends formatted text at pDst + (nSize - nAvail), not at strlen(pDst).
 * @return The space left after appending, to pass to the next call; not a length.
 */
size_t xstrncatsf(char *pDst, size_t nSize, size_t nAvail, const char *pFmt, ...);

/*!
 * @brief Random lower case letters, upper case too with bUpper, digits too with bNumbers.
 * @return Number of characters written, at most nSize - 1.
 * @note Uses rand(): not for secrets, keys or tokens.
 */
size_t xstrrand(char *pDst, size_t nSize, size_t nLength, xbool_t bUpper, xbool_t bNumbers);

/*!
 * @brief Counts the bytes of escape sequences in pStr that take no screen column.
 * @return Number of those bytes. *pChars gets the visible characters, *pPosit where the scan stopped:
 *         the end, or the byte after nMaxChars visible characters when nMaxChars is not 0.
 */
size_t xstrextra(const char *pStr, size_t nLength, size_t nMaxChars, size_t *pChars, size_t *pPosit);

/*!
 * @brief Formats into pDst and pads it on the right with cFChar up to nFLen characters.
 * @return Length of the result.
 */
size_t xstrncpyfl(char *pDst, size_t nSize, size_t nFLen, char cFChar, const char* pFmt, ...);

/*!
 * @brief Formats into pDst and pads it on the left with cFChar up to nFLen characters.
 * @return Length of the result.
 */
size_t xstrnlcpyf(char *pDst, size_t nSize, size_t nFLen, char cFChar, const char* pFmt, ...);

size_t xstrnfill(char *pDst, size_t nSize, size_t nLength, char cFill);

/*!
 * @brief Number of bytes at pOffset that take no screen column.
 * @return Length of a known color or format escape sequence, 1 for the first byte of
 *         the degree sign (its second byte is the visible character), 0 otherwise.
 */
size_t xstrisextra(const char *pOffset);

/*!
 * @brief A string of nLength cFill characters, at most XSTR_MAX - 1.
 * @return A static buffer shared by all calls: not thread safe, overwritten by the next call.
 */
char *xstrfill(size_t nLength, char cFill);

/*!
 * @brief Binary safe search for pSrc in the first nLen bytes of pStr, starting at nPos.
 * @return Offset of the match from nPos (not from the start of pStr), or XSTDERR (-1)
 *         when not found or nPos >= nLen. An empty pSrc matches at offset 0.
 */
int xstrnsrc(const char *pStr, size_t nLen, const char *pSrc, size_t nPos);

/*!
 * @brief Binary safe search for pSrc in the first nLength bytes of pStr.
 * @return Index of the match from the start of pStr, or XSTDERR (-1) when not found.
 * @note pSrc must not be NULL. An empty pSrc gives 0 on GNU builds and XSTDERR elsewhere.
 */
int xstrsrcb(const char *pStr, size_t nLength, const char *pSrc);

/*!
 * @brief Search for pSrc in pStr starting at nPos.
 * @return Offset of the match from nPos (not from the start of pStr), or XSTDERR (-1).
 */
int xstrsrcp(const char *pStr, const char *pSrc, size_t nPos);

/*!
 * @brief Position of the first occurrence of pSrc in pStr.
 * @return Index of the match, 0 included, or XSTDERR (-1) when not found or a string is NULL or empty.
 * @note 0 is a match at the start: test the result with >= 0, never as a boolean.
 */
int xstrsrc(const char *pStr, const char *pSrc);

/*!
 * @brief A new malloc() copy of pSrc in the given case, the caller frees it.
 * @return The copy, or NULL for an empty pSrc or on failure.
 * @note pSrc must not be NULL.
 */
char* xstracase(const char *pSrc, xstr_case_t eCase);

/*!
 * @brief A new malloc() copy of at most nLength characters of pSrc in the given case.
 * @return The copy, or NULL for an empty pSrc or on failure.
 * @note pSrc must not be NULL.
 */
char* xstracasen(const char *pSrc, xstr_case_t eCase, size_t nLength);

size_t xstrcase(char *pSrc, xstr_case_t eCase);
size_t xstrncase(char *pDst, size_t nSize, xstr_case_t eCase, const char *pSrc);
size_t xstrncases(char* pDst, size_t nSize, xstr_case_t eCase, const char *pSrc, size_t nSrcLen);

char* xstrtok(char* pString, const char* pDelimiter, char** pContext);

/*!
 * @brief Cuts the text after pStart (a string) up to the end mark pEnd in place.
 * @return Pointer into pData, or NULL when pStart is not found. A NULL pEnd gives
 *         the rest after pStart, a NULL pStart the first token of pData.
 * @note Modifies pData: strtok_r() puts a NUL at the end mark. pEnd is a set of
 *       characters, not a string: "</a>" ends at the first '<', '/', 'a' or '>'.
 */
char* xstrcut(char *pData, const char *pStart, const char *pEnd);

char* xstracut(const char *pSrc, size_t nPosit, size_t nSize);

/*!
 * @brief Copies nSize characters of pSrc from nPosit into pDst.
 * @return The smallest of nSize, nDstSize and the characters left after nPosit: one more than the characters copied
 *         when the copy is cut to nDstSize - 1. 0 when nPosit is past the end.
 */
size_t xstrncut(char *pDst, size_t nDstSize, const char *pData, size_t nPosit, size_t nSize);

/*!
 * @brief Copies the text between the strings pStart and pEnd of pSrc into pDst.
 * @return Length copied, 0 when pStart is not found and also for an empty text.
 *         A NULL pStart cuts from the beginning, a missing pEnd to the end.
 * @note pDst must not be NULL.
 */
size_t xstrncuts(char *pDst, size_t nSize, const char *pSrc, const char *pStart, const char *pEnd);

/*!
 * @brief Binary safe xstrntok() over nLen bytes with a delimiter of nDlmtLen bytes.
 * @return Next position, 0 for the last token, XSTDERR (-1) past the end, as xstrntok().
 */
int xstrntokat(char *pDst, size_t nSize, const char *pStr, size_t nLen, size_t nPosit, const char *pDlmt, size_t nDlmtLen);

/*!
 * @brief Copies the token from nPosit up to the delimiter string pDlmt into pDst.
 * @return Position where the next token starts; 0 when no delimiter follows,
 *         pDst then holds the last token; XSTDERR (-1) when nPosit is at or past the end of pStr.
 * @note A loop on `xstrntok(...) > 0` misses the last token: use the token on 0 as well.
 */
int xstrntok(char *pDst, size_t nSize, const char *pStr, size_t nPosit, const char *pDlmt);

size_t xstrnclr(char *pDst, size_t nSize, const char* pClr, const char* pStr, ...);

/*!
 * @brief Removes nSize characters of pStr at nPosit, in place.
 * @return New length of pStr, or 0 when nothing was removed and pStr is left as it was.
 * @note 0 is also the new length when everything was removed: compare it with the old length.
 */
size_t xstrnrm(char *pStr, size_t nPosit, size_t nSize);

/*!
 * @brief Sets nLength bytes of pString to NUL.
 * @note A zero nLength still writes one NUL byte. pString must not be NULL.
 */
void xstrnull(char *pString, size_t nLength);

void xstrnul(char *pString);

/*!
 * @brief Checks that a string has content.
 * @return XTRUE when pStr is not NULL and not empty, XFALSE otherwise.
 */
xbool_t xstrused(const char *pStr);

size_t xstrnrgb(char *pStr, size_t nSize, int nR, int nG, int nB);
size_t xstrnyuv(char *pStr, size_t nSize, int nY, int nU, int nV);
char *xstrrgb(int nR, int nG, int nB);
char *xstryuv(int nY, int nU, int nV);

/////////////////////////////////////////////////////////////////////////
// XString Implementation
/////////////////////////////////////////////////////////////////////////

typedef struct XString {
    char* pData;
    size_t nLength;
    size_t nSize;
    int16_t nStatus;
    xbool_t nAlloc;
    xbool_t nFast;
} xstring_t;

xstring_t *XString_New(size_t nSize, uint8_t nFastAlloc);
xstring_t *XString_From(const char *pData, size_t nLength);
xstring_t *XString_FromFmt(const char *pFmt, ...);
xstring_t *XString_FromStr(xstring_t *pString);

int XString_Init(xstring_t *pString, size_t nSize, uint8_t nFastAlloc);
int XString_InitFrom(xstring_t *pStr, const char *pFmt, ...);
int XString_Set(xstring_t *pString, char *pData, size_t nLength);
int XString_Increase(xstring_t *pString, size_t nSize);
int XString_Resize(xstring_t *pString, size_t nSize);
void XString_Clear(xstring_t *pString);

int XString_Add(xstring_t *pString, const char *pData, size_t nLength);
int XString_Copy(xstring_t *pString, xstring_t *pSrc);
int XString_Append(xstring_t *pString, const char *pFmt, ...);
int XString_AddString(xstring_t *pString, xstring_t *pSrc);

int XString_Insert(xstring_t *pString, size_t nPosit, const char *pData, size_t nLength);
int XString_InsertFmt(xstring_t *pString, size_t nPosit, const char *pFmt, ...);

int XString_Remove(xstring_t *pString, size_t nPosit, size_t nSize);
int XString_Delete(xstring_t *pString, size_t nPosit, size_t nSize);
int XString_Advance(xstring_t *pString, size_t nSize);

/*!
 * @brief Copies the token from nPosit up to the delimiter string pDlmt, like xstrntok().
 * @return Next position, 0 for the last token, XSTDERR (-1) past the end.
 * @note Unlike xstrntok(), an empty token (pDlmt right at nPosit) is none:
 *       pDst gets the rest of the string and 0 is returned.
 */
int XString_Tokenize(xstring_t *pString, char *pDst, size_t nSize, size_t nPosit, const char *pDlmt);

/*!
 * @brief Puts the token from nPosit up to the delimiter string pDlmt, like xstrntok().
 * @return Next position, 0 for the last token, XSTDERR (-1) past the end or on failure.
 * @note Unlike xstrntok(), an empty token (pDlmt right at nPosit) is none: pDst gets the
 *       rest of the string and 0 is returned.
 */
int XString_Token(xstring_t *pString, xstring_t *pDst, size_t nPosit, const char *pDlmt);

int XString_Case(xstring_t *pString, xstr_case_t eCase, size_t nPosit, size_t nSize);
int XString_Color(xstring_t *pString, const char* pClr, size_t nPosit, size_t nSize);
int XString_ChangeCase(xstring_t *pString, xstr_case_t eCase);
int XString_ChangeColor(xstring_t *pString, const char* pClr);
int XString_Replace(xstring_t *pString, const char *pRep, const char *pWith);

/*!
 * @brief Search for pSearch in pString starting at nPos.
 * @return Offset of the match from nPos (not from the start), or XSTDERR (-1); 0 is a match at nPos.
 */
int XString_Search(xstring_t *pString, size_t nPos, const char *pSearch);

xarray_t* XString_SplitStr(xstring_t *pString, const char *pDlmt);
xarray_t* XString_Split(const char *pCStr, const char *pDlmt);

int XString_Sub(xstring_t *pString, char *pDst, size_t nDstSize, size_t nPos, size_t nSize);
int XString_SubStr(xstring_t *pString, xstring_t *pSub, size_t nPos, size_t nSize);
xstring_t *XString_SubNew(xstring_t *pString, size_t nPos, size_t nSize);

int XString_Cut(xstring_t *pString, char *pDst, size_t nSize, const char *pFrom, const char *pTo);
int XString_CutSub(xstring_t *pString, xstring_t *pSub, const char *pFrom, const char *pTo);
xstring_t *XString_CutNew(xstring_t *pString, const char *pFrom, const char *pTo);

#ifdef __cplusplus
}
#endif

#endif /* __XUTILS_XSTR_H__ */
