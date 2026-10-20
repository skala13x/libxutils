/*!
 *  @file libxutils/examples/strings.c
 *
 *  This source is part of the "libxutils" project
 *  2015-2026  Sandro Kalatozishvili (s.kalatoz@gmail.com)
 *
 * @brief Example of working with strings: a request line is checked and
 * split into its parts, its query is read, its path is matched against
 * patterns and rewritten, and the answer is formatted into fixed buffers
 * and built in a growing string. The results of several string functions
 * are easy to misread, the comments say how to read each one.
 */

#include "xstd.h"
#include "str.h"
#include "array.h"
#include "log.h"

static const char *g_pRequest = "GET /files/report.pdf?user=alice&lang=EN HTTP/1.1";

int main()
{
    xlog_defaults();
    xlog("Request: %s\n", g_pRequest);

//////////////////////////////////////////////////////////////////////////////////////////////
// Compare: these functions answer "equal", they are not strcmp()
//////////////////////////////////////////////////////////////////////////////////////////////

    /* xstrncmp() compares the first n characters, a prefix test: XTRUE is equal */
    if (!xstrncmp(g_pRequest, "GET ", 4))
    {
        xloge("Only GET requests are handled");
        return XSTDERR;
    }

//////////////////////////////////////////////////////////////////////////////////////////////
// Split the request line into method, target and version
//////////////////////////////////////////////////////////////////////////////////////////////

    char sParts[3][XSTR_MID];
    int nParts = 0, nNext = 0;

    do
    {
        char sToken[XSTR_MID];

        /* The position of the next token, 0 after the last one, XSTDERR when nothing is left */
        nNext = xstrntok(sToken, sizeof(sToken), g_pRequest, nNext, " ");
        if (nNext < 0) break;

        if (nParts < 3) xstrncpy(sParts[nParts], sizeof(sParts[nParts]), sToken);
        nParts++;
    }
    while (nNext > 0);

    if (nParts != 3)
    {
        xloge("A request line has 3 parts, this one has %d", nParts);
        return XSTDERR;
    }

    const char *pTarget = sParts[1];
    xlog("Method: %s, target: %s, version: %s", sParts[0], pTarget, sParts[2]);

    /* Ignoring the case of the letters */
    xlog("HTTP/1.1: %s\n", xstrncasecmp(sParts[2], "http/1.1", 8) ? "yes" : "no");

//////////////////////////////////////////////////////////////////////////////////////////////
// Find the query: a search returns a position, 0 is a match at the start
//////////////////////////////////////////////////////////////////////////////////////////////

    /* XSTDERR (-1) means not found, so the result is tested with >= 0, never as a boolean */
    xlog("The target starts with '/': %s", xstrsrc(pTarget, "/") == 0 ? "yes" : "no");

    char sPath[XSTR_MID], sQuery[XSTR_MID];
    int nQuery = xstrsrc(pTarget, "?");

    if (nQuery >= 0)
    {
        xstrncpys(sPath, sizeof(sPath), pTarget, (size_t)nQuery);
        xstrncpy(sQuery, sizeof(sQuery), &pTarget[nQuery + 1]);
    }
    else
    {
        xstrncpy(sPath, sizeof(sPath), pTarget);
        sQuery[0] = XSTR_NUL;
    }

    xlog("Path: %s, query: %s", sPath, sQuery);

//////////////////////////////////////////////////////////////////////////////////////////////
// Read the query parameters
//////////////////////////////////////////////////////////////////////////////////////////////

    char sUser[XSTR_MICRO] = XSTR_INIT;
    char sLang[XSTR_MICRO] = XSTR_INIT;

    /* NULL when there is no parameter at all, an empty array is never returned */
    xarray_t *pParams = xstrsplit(sQuery, "&");
    size_t i;

    for (i = 0; i < XArray_Used(pParams); i++)
    {
        const char *pParam = (const char*)XArray_GetData(pParams, i);
        char sKey[XSTR_MICRO], sValue[XSTR_MICRO];

        /* No start marker cuts from the beginning, no end marker to the end */
        xstrncuts(sKey, sizeof(sKey), pParam, NULL, "=");
        xstrncuts(sValue, sizeof(sValue), pParam, "=", NULL);

        if (xstrcmp(sKey, "user")) xstrncpy(sUser, sizeof(sUser), sValue);
        else if (xstrcmp(sKey, "lang")) xstrncpy(sLang, sizeof(sLang), sValue);
    }

    XArray_Destroy(pParams);
    xstrcase(sLang, XSTR_LOWER);
    xlog("User: %s, language: %s\n", sUser, sLang);

    if (!xstrcmp(sUser, "alice"))
    {
        xloge("The user parameter was not found");
        return XSTDERR;
    }

//////////////////////////////////////////////////////////////////////////////////////////////
// Match the path against patterns and rewrite it
//////////////////////////////////////////////////////////////////////////////////////////////

    /* '*' is any run of characters and '?' one character, a list is separated by ';' */
    size_t nPathLen = strlen(sPath);
    xlog("Under /files: %s", xstrmatch(sPath, nPathLen, "/files/*") ? "yes" : "no");
    xlog("A document: %s", xstrmatchm(sPath, nPathLen, "*.pdf;*.doc;*.txt", NULL) ? "yes" : "no");
    xlog("A report: %s", xstrmatch(sPath, nPathLen, "*/report.???") ? "yes" : "no");

    /* xstrrep() returns a new string, the caller frees it */
    char *pArchived = xstrrep(sPath, "/files/", "/archive/2026/");
    if (pArchived == NULL)
    {
        xloge("Failed to rewrite the path");
        return XSTDERR;
    }

    xlog("Moved to: %s\n", pArchived);

//////////////////////////////////////////////////////////////////////////////////////////////
// Format into fixed buffers: the text is cut to the buffer
//////////////////////////////////////////////////////////////////////////////////////////////

    char sHeader[XSTR_TINY];
    size_t nLength = xstrncpyf(sHeader, sizeof(sHeader), "%s 301 Moved", sParts[2]);

    /* xstrncat() returns the length it appended, not the new length of the string */
    nLength += xstrncat(sHeader, sizeof(sHeader), ", Location: %s", pArchived);
    xlog("%s (%zu characters)", sHeader, nLength);

    /* The length that comes back is what was written, a cut is not reported */
    char sShort[16];
    size_t nWritten = xstrncpyf(sShort, sizeof(sShort), "%s", pArchived);
    xlog("In 16 bytes: %s (%zu of %zu characters)\n", sShort, nWritten, strlen(pArchived));

//////////////////////////////////////////////////////////////////////////////////////////////
// Build the answer in a string that grows as needed
//////////////////////////////////////////////////////////////////////////////////////////////

    xstring_t body;
    if (XString_Init(&body, XSTR_MICRO, XFALSE) < 0)
    {
        xloge("Failed to create a string");
        free(pArchived);
        return XSTDERR;
    }

    /* Every XString function returns the new length or XSTDERR */
    XString_Append(&body, "\"user\": \"%s\", \"lang\": \"%s\"", sUser, sLang);
    XString_Append(&body, ", \"location\": \"%s\"", pArchived);
    XString_Insert(&body, 0, "{ ", 2);
    XString_Add(&body, " }", 2);
    XString_Replace(&body, "alice", "Alice");
    free(pArchived);

    /* Like xstrsrcp(), the position is counted from where the search started */
    int nFrom = XString_Search(&body, 0, "location");
    int nRelative = nFrom >= 0 ? XString_Search(&body, (size_t)nFrom, "/archive") : XSTDERR;
    xlog("%s (%zu characters)", body.pData, body.nLength);
    xlog("\"/archive\" is %d characters after \"location\", which is at %d", nRelative, nFrom);

    char sColored[XSTR_MIN];
    xstrnclr(sColored, sizeof(sColored), XSTR_CLR_GREEN, "%s", "Done");
    xlog("%s", sColored);

    XString_Clear(&body);
    return 0;
}
