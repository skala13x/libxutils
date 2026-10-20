/*!
 *  @file libxutils/tools/xhttp.c
 *
 *  This source is part of the "libxutils" project
 *  2015-2020  Sandro Kalatozishvili (s.kalatoz@gmail.com)
 *
 * @brief Example file for working with the HTTP request/responses.
 * Send custom HTTP request, analyze headers, download content, etc.
 */

#include "xstd.h"
#include "xtime.h"
#include "xver.h"
#include "xfs.h"
#include "sock.h"
#include "http.h"
#include "log.h"
#include "str.h"
#include "cli.h"

extern char *optarg;

#define XHTTP_VERSION_MAJ   0
#define XHTTP_VERSION_MIN   7

#define XHTTP_INTERVAL_SEC  1
#define XHTTP_REDIRECT_MAX  20

#ifdef _WIN32
#define XHTTP_STDOUT_TTY    _isatty(_fileno(stdout))
#else
#define XHTTP_STDOUT_TTY    isatty(fileno(stdout))
#endif

typedef struct xhttp_args_ {
    xhttp_method_t eMethod;
    xcli_bar_t progressBar;
    xfile_t *pOutputFile;

    xtime_t lastTime;
    xbool_t nAutoFollow;
    xbool_t nForceWrite;
    xbool_t nDownload;
    xbool_t nVerbose;
    xbool_t nSSL;
    size_t nRedirects;
    size_t nTimeout;
    size_t nBytes;
    size_t nDone;

    xbyte_buffer_t content;
    char sUnixAddr[XPATH_MAX];
    char sAddress[XHTTP_URL_MAX];
    char sHeaders[XLINE_MAX];
    char sContent[XPATH_MAX];
    char sOutput[XPATH_MAX];
    char sSpeed[XSTR_MIN];
} xhttp_args_t;

static char *XHTTPApp_WhiteSpace(const int nLength)
{
    static char sRetVal[XHTTP_FIELD_MAX];
    xstrnul(sRetVal);
    int i = 0;

    int nLen = XSTD_MIN(nLength, sizeof(sRetVal) - 1);
    for (i = 0; i < nLen; i++) sRetVal[i] = ' ';

    sRetVal[i] = '\0';
    return sRetVal;
}

void XHTTPApp_DisplayUsage(const char *pName)
{
    int nLength = strlen(pName) + 6;

    printf("===================================================================\n");
    printf(" XHTTP v%d.%d - (c) 2022 Sandro Kalatozishvili (s.kalatoz@gmail.com)\n",
        XHTTP_VERSION_MAJ, XHTTP_VERSION_MIN);
    printf("===================================================================\n");

    printf("Usage: %s [-l <address>] [-u <address>] [-c <content>]\n", pName);
    printf(" %s [-t <seconds>] [-x <headers>] [-o <output>]\n", XHTTPApp_WhiteSpace(nLength));
    printf(" %s [-m <method>] [-d] [-f] [-s] [-v] [-w] [-h]\n", XHTTPApp_WhiteSpace(nLength));

    printf("Options are:\n");
    printf("  -l <address>          # HTTP/S address/link (%s*%s)\n", XSTR_CLR_RED, XSTR_FMT_RESET);
    printf("  -u <address>          # Unix socket address\n");
    printf("  -c <content>          # Content file path\n");
    printf("  -m <method>           # HTTP request method\n");
    printf("  -o <output>           # Output file path\n");
    printf("  -t <seconds>          # Receive timeout (sec)\n");
    printf("  -x <headers>          # Custom HTTP headers\n");
    printf("  -d                    # Download output as a file\n");
    printf("  -f                    # Follow redirected locations\n");
    printf("  -s                    # Force SSL connection\n");
    printf("  -v                    # Enable verbose logging\n");
    printf("  -w                    # Force overwrite output\n");
    printf("  -h                    # Print version and usage\n\n");
    printf("Examples:\n");
    printf("1) %s -l https://endpoint.com/ -c body.json -m POST\n", pName);
    printf("2) %s -l endpoint.com/test -t 20 -wo output.txt -s -v\n", pName);
    printf("2) %s -l endpoint.com -x 'X-Is-Custom: True; X-My-Header: Test'\n", pName);
    printf("2) %s -l http://localhost/endpoint -u /var/run/xutils/unix.sock\n", pName);
}

int XHTTPApp_ParseArgs(xhttp_args_t *pArgs, int argc, char *argv[])
{
    XByteBuffer_Init(&pArgs->content, 0, 0);
    XTime_Init(&pArgs->lastTime);

    pArgs->pOutputFile = NULL;
    pArgs->eMethod = XHTTP_GET;
    pArgs->nAutoFollow = XFALSE;
    pArgs->nForceWrite = XFALSE;
    pArgs->nDownload = XFALSE;
    pArgs->nVerbose = XFALSE;
    pArgs->nSSL = XFALSE;
    pArgs->nRedirects = 0;
    pArgs->nTimeout = 0;
    pArgs->nBytes = 0;
    pArgs->nDone = 0;

    xstrncpy(pArgs->sSpeed, sizeof(pArgs->sSpeed), "N/A");
    xstrnul(pArgs->sUnixAddr);
    xstrnul(pArgs->sAddress);
    xstrnul(pArgs->sHeaders);
    xstrnul(pArgs->sContent);
    xstrnul(pArgs->sOutput);
    int nChar = 0;

    while ((nChar = getopt(argc, argv, "u:l:c:m:o:t:x:d1:f1:s1:v1:w1:h1")) != -1)
    {
        switch (nChar)
        {
            case 'u':
                xstrncpy(pArgs->sUnixAddr, sizeof(pArgs->sUnixAddr), optarg);
                break;
            case 'l':
                xstrncpy(pArgs->sAddress, sizeof(pArgs->sAddress), optarg);
                break;
            case 'c':
                xstrncpy(pArgs->sContent, sizeof(pArgs->sContent), optarg);
                break;
            case 'o':
                xstrncpy(pArgs->sOutput, sizeof(pArgs->sOutput), optarg);
                break;
            case 'x':
                xstrncpy(pArgs->sHeaders, sizeof(pArgs->sHeaders), optarg);
                break;
            case 'm':
                /* A method the library does not know went out as DUMMY */
                pArgs->eMethod = XHTTP_GetMethodType(optarg);
                if (pArgs->eMethod == XHTTP_DUMMY)
                {
                    xloge("Unsupported HTTP method: %s", optarg);
                    return XFALSE;
                }
                break;
            case 't':
                pArgs->nTimeout = atoi(optarg);
                break;
            case 'd':
                pArgs->nDownload = XTRUE;
                break;
            case 'f':
                pArgs->nAutoFollow = XTRUE;
                break;
            case 's':
                pArgs->nSSL = XTRUE;
                break;
            case 'v':
                pArgs->nVerbose = XTRUE;
                break;
            case 'w':
                pArgs->nForceWrite = XTRUE;
                break;
            case 'h':
            default:
                return 0;
        }
    }

    if (!xstrused(pArgs->sAddress)) return XFALSE;
    xcli_bar_t *pBar = &pArgs->progressBar;

    if (xstrused(pArgs->sContent) && XPath_LoadBuffer(pArgs->sContent, &pArgs->content) <= 0)
    {
        xloge("Failed to load content from file: %s (%d)", pArgs->sContent, errno);
        return XFALSE;
    }

    if (xstrused(pArgs->sOutput) || pArgs->nDownload)
    {
        XProgBar_GetDefaults(pBar);
        pArgs->nAutoFollow = XTRUE;
        pArgs->nDownload = XTRUE;
        pBar->bInPercent = XTRUE;
    }

    if (pArgs->nVerbose)
    {
        xlog_timing(XLOG_TIME);
        xlog_enable(XLOG_ALL);
    }

    return XTRUE;
}

/* The text without the spaces around it, cut in place */
static char *XHTTPApp_Trim(char *pText)
{
    while (*pText == XSTR_SPACE_CHAR) pText++;
    size_t nLength = strlen(pText);
    while (nLength && pText[nLength - 1] == XSTR_SPACE_CHAR) pText[--nLength] = XSTR_NUL;
    return pText;
}

int XHTTPApp_AppendArgHeaders(xhttp_t *pHandle, xhttp_args_t *pArgs)
{
    xarray_t *pArr = xstrsplit(pArgs->sHeaders, ";");
    if (pArr == NULL) return XSTDERR;

    pHandle->nAllowUpdate = XTRUE;
    size_t i, nUsed = pArr->nUsed;

    for (i = 0; i < nUsed; i++)
    {
        char *pHeader = (char*)XArray_GetData(pArr, i);
        if (pHeader == NULL) continue;

        /* The name ends at the first colon and the value is all the rest, colons in it included: a value
           such as a time or a URL was cut at its own colon */
        char *pColon = strchr(pHeader, ':');
        if (pColon == NULL)
        {
            XArray_Destroy(pArr);
            return XSTDERR;
        }

        *pColon = XSTR_NUL;
        char *pOptionPtr = XHTTPApp_Trim(pHeader);
        char *pFieldPtr = XHTTPApp_Trim(pColon + 1);

        if (!xstrused(pOptionPtr) || !xstrused(pFieldPtr) ||
            XHTTP_AddHeader(pHandle, pOptionPtr, "%s", pFieldPtr) < 0)
        {
            XArray_Destroy(pArr);
            return XSTDERR;
        }

        xlogd("Adding header: %s: %s", pOptionPtr, pFieldPtr);
    }

    XArray_Destroy(pArr);
    return XSTDOK;
}

int XHTTPApp_DisplayRequest(xhttp_t *pHandle)
{
    xhttp_args_t *pArgs = (xhttp_args_t*)pHandle->pUserCtx;
    XTime_Get(&pArgs->lastTime);
    if (!pArgs->nVerbose) return XSTDOK;

    xlogd("Sending %s request: %zu bytes",
        XHTTP_GetMethodStr(pHandle->eMethod),
        pHandle->rawData.nUsed);

    fwrite(pHandle->rawData.pData, 1, pHandle->rawData.nUsed, stdout);
    if (XHTTP_GetBodySize(pHandle)) printf("\n");

    return XSTDOK;
}

void XHTTPApp_UpdateProgress(xhttp_t *pHandle)
{
    xhttp_args_t *pArgs = (xhttp_args_t*)pHandle->pUserCtx;
    xcli_bar_t *pBar = &pArgs->progressBar;
    char sReceivedSize[XHTTP_FIELD_MAX];

    if (!pHandle->nContentLength) pBar->fPercent = -1.;
    else pBar->fPercent = (double)100 / pHandle->nContentLength * pArgs->nDone;

    XBytesToUnit(sReceivedSize, sizeof(sReceivedSize), pArgs->nDone, XFALSE);
    xstrncpyf(pBar->sPrefix, sizeof(pBar->sPrefix), "Downloading... %s ", pArgs->sSpeed);
    xstrncpyf(pBar->sSuffix, sizeof(pBar->sSuffix), " %s", sReceivedSize);
    XProgBar_Update(pBar);
}

/* The file a download goes to until all of it is there. Returns XSTDERR when the name does not fit: cut, it
   could be the output's own name. */
static XSTATUS XHTTPApp_PartPath(xhttp_args_t *pArgs, char *pPath, size_t nSize)
{
    int nLength = snprintf(pPath, nSize, "%s.part", pArgs->sOutput);
    if (nLength >= 0 && (size_t)nLength < nSize) return XSTDOK;

    xloge("Output path is too long: %s", pArgs->sOutput);
    return XSTDERR;
}

static XSTATUS XHTTPApp_OpenOutput(xhttp_args_t *pArgs)
{
    char sTmpPath[XPATH_MAX];
    if (XHTTPApp_PartPath(pArgs, sTmpPath, sizeof(sTmpPath)) < 0) return XSTDERR;

    pArgs->pOutputFile = XFile_Alloc(sTmpPath, "cwt", NULL);
    if (pArgs->pOutputFile == NULL)
    {
        xloge("Failed to open output file: %s (%d)", sTmpPath, errno);
        return XSTDERR;
    }

    return XSTDOK;
}

/* Closes the .part file. A write the file system had not finished can fail at the close, so that is checked too. */
static XSTATUS XHTTPApp_CloseOutput(xhttp_args_t *pArgs)
{
    if (pArgs->pOutputFile == NULL) return XSTDOK;
    int nFD = pArgs->pOutputFile->nFD;

    pArgs->pOutputFile->nFD = XSTDERR;
    XFile_Free(&pArgs->pOutputFile);
    return (nFD < 0 || xclose(nFD) == 0) ? XSTDOK : XSTDERR;
}

/* Ends a download that is all there: the .part file is closed, and only then takes the output's name. One that
   has no body is an empty file, which is made too. */
static XSTATUS XHTTPApp_FinishOutput(xhttp_args_t *pArgs)
{
    char sTmpPath[XPATH_MAX];
    if (XHTTPApp_PartPath(pArgs, sTmpPath, sizeof(sTmpPath)) < 0) return XSTDERR;
    if (pArgs->pOutputFile == NULL && XHTTPApp_OpenOutput(pArgs) < 0) return XSTDERR;

    if (XHTTPApp_CloseOutput(pArgs) < 0)
    {
        xloge("Failed to write output file: %s (%s)", sTmpPath, XSTRERR);
        return XSTDERR;
    }

    if (rename(sTmpPath, pArgs->sOutput) < 0)
    {
        xloge("Failed to rename file: %s -> %s (%s)", sTmpPath, pArgs->sOutput, XSTRERR);
        return XSTDERR;
    }

    return XSTDOK;
}

/* Writes all of it: a write can take less than it was given */
static XSTATUS XHTTPApp_WriteAll(xfile_t *pFile, const uint8_t *pData, size_t nSize)
{
    while (nSize > 0)
    {
        int nWritten = XFile_Write(pFile, pData, nSize);
        if (nWritten < 0 && errno == EINTR) continue;
        if (nWritten <= 0) return XSTDERR;

        pData += nWritten;
        nSize -= (size_t)nWritten;
    }

    return XSTDOK;
}

int XHTTPApp_DumpResponse(xhttp_t *pHandle, xhttp_ctx_t *pCbCtx)
{
    xhttp_args_t *pArgs = (xhttp_args_t*)pHandle->pUserCtx;
    if (!pArgs->nDownload || !XHTTP_IsSuccessCode(pHandle)) return XSTDUSR;
    if (pArgs->pOutputFile == NULL && XHTTPApp_OpenOutput(pArgs) < 0) return XSTDERR;

    xtime_t currTime;
    XTime_Get(&currTime);
    pArgs->nBytes += pCbCtx->nLength;

    int nDiff = XTime_DiffSec(&currTime, &pArgs->lastTime);
    if (nDiff > XHTTP_INTERVAL_SEC)
    {
        char sPerSec[XSTR_MIN], sSpeed[XSTR_MIN];
        size_t nPerSec = pArgs->nBytes / nDiff;
        XBytesToUnit(sPerSec, sizeof(sPerSec), nPerSec, XFALSE);
        xstrncpyfl(sSpeed, sizeof(sSpeed), 12, XSTR_SPACE_CHAR, "%s/s", sPerSec);
        xstrnclr(pArgs->sSpeed, sizeof(pArgs->sSpeed), XSTR_FMT_BOLD, "%s", sSpeed);
        XTime_Copy(&pArgs->lastTime, &currTime);
        pArgs->nBytes = 0;
    }

    pArgs->nDone += pCbCtx->nLength;
    XHTTPApp_UpdateProgress(pHandle);

    if (XHTTPApp_WriteAll(pArgs->pOutputFile, (const uint8_t*)pCbCtx->pData, pCbCtx->nLength) < 0)
    {
        xloge("Failed to write data to output file: %s (%d)", pArgs->sOutput, errno);
        return XSTDERR;
    }

    return XSTDOK;
}

int XHTTPApp_DisplayResponseHdr(xhttp_t *pHandle)
{
    xhttp_args_t *pArgs = (xhttp_args_t*)pHandle->pUserCtx;
    if (!pArgs->nVerbose) return XSTDOK;

    const char *pStatus = XHTTP_GetCodeStr(pHandle->nStatusCode);
    const char *pCntType = XHTTP_GetHeader(pHandle, "Content-Type");

    char cSave = pHandle->rawData.pData[pHandle->nHeaderLength - 1];
    pHandle->rawData.pData[pHandle->nHeaderLength - 1] = XSTR_NUL;
    xlogd("Received response header: %s", pStatus);
    printf("%s\n", (char*)pHandle->rawData.pData);
    pHandle->rawData.pData[pHandle->nHeaderLength - 1] = cSave;

    xbool_t bFollowing = (pHandle->nStatusCode >= 300 && pHandle->nStatusCode < 400 &&
        pArgs->nAutoFollow && XHTTP_GetHeader(pHandle, "Location") != NULL) ? XTRUE : XFALSE;

    if ((pHandle->nContentLength > 0 || xstrused(pCntType)) && !bFollowing)
    {
        char sBytes[XSTR_TINY];
        if (!pHandle->nContentLength) xstrncpy(sBytes, sizeof(sBytes), "N/A");
        else xstrncpyf(sBytes, sizeof(sBytes), "%zu", pHandle->nContentLength);
        xlogd("Downloading body: %s bytes", sBytes);
    }

    return XSTDOK;
}

int XHTTPApp_Callback(xhttp_t *pHttp, xhttp_ctx_t *pCbCtx)
{
    switch (pCbCtx->eCbType)
    {
        case XHTTP_STATUS:
            if (pCbCtx->eStatus == XHTTP_PARSED)
                return XHTTPApp_DisplayResponseHdr(pHttp);
            xlogd("%s", (const char*)pCbCtx->pData);
            return XSTDOK;
        case XHTTP_READ_CNT:
            return XHTTPApp_DumpResponse(pHttp, pCbCtx);
        case XHTTP_WRITE:
            return XHTTPApp_DisplayRequest(pHttp);
        case XHTTP_ERROR:
            xloge("%s", (const char*)pCbCtx->pData);
            return XSTDERR;
        default:
            break;
    }

    return XSTDUSR;
}

int XHTTPApp_Prepare(xhttp_args_t *pArgs, xlink_t *pLink)
{
    if (XLink_Parse(pLink, pArgs->sAddress) == XSTDERR)
    {
        xloge("Unsupported link: %s", pArgs->sAddress);
        return XSTDERR;
    }

    if (pArgs->nVerbose)
    {
        xlogd("Parsed link: %s", pArgs->sAddress);
        printf("Protocol: %s\nHost: %s\nAddr: %s\nPort: %d\nUser: %s\nPass: %s\nFile: %s\nURL: %s\n\n",
              pLink->sProtocol, pLink->sHost, pLink->sAddr, pLink->nPort,
              pLink->sUser, pLink->sPass, pLink->sFile, pLink->sUri);
    }

    if (pArgs->nDownload && !xstrused(pArgs->sOutput))
    {
        const char *pFileName = xstrused(pLink->sFile) ? pLink->sFile : "xhttp.out";
        xstrncpy(pArgs->sOutput, sizeof(pArgs->sOutput), pFileName);
        size_t nFileCount = 1;

        while (XPath_Exists(pArgs->sOutput))
        {
            xstrncpyf(pArgs->sOutput, sizeof(pArgs->sOutput), "%s.%zu", pFileName, nFileCount);
            nFileCount++;
        }
    }

    if (XPath_Exists(pArgs->sOutput) && pArgs->nForceWrite == XFALSE)
    {
        xlogw("File already exists: %s", pArgs->sOutput);
        xlogi("Use option -w to force overwrite output");
        return XSTDERR;
    }

    if (pArgs->nSSL && strncmp(pLink->sProtocol, "https", 5))
    {
        xlogd("Upgrading to HTTPS: %s: %d -> %d", pLink->sAddr, pLink->nPort, XHTTP_SSL_PORT);
        xstrncpyf(pLink->sHost, sizeof(pLink->sHost), "%s:%d", pLink->sAddr, XHTTP_SSL_PORT);
        xstrncpy(pLink->sProtocol, sizeof(pLink->sProtocol), "https");
        pLink->nPort = XHTTP_SSL_PORT;
    }

    return XSTDOK;
}

/* Whether a reference starts with a scheme (RFC 3986 3.1): letters, then letters, digits, '+', '-' or '.',
   up to a colon that comes before any '/', '?' or '#' */
static xbool_t XHTTPApp_HasScheme(const char *pReference)
{
    if (!isalpha((unsigned char)*pReference)) return XFALSE;

    for (pReference++; *pReference; pReference++)
    {
        if (*pReference == ':') return XTRUE;
        if (!isalnum((unsigned char)*pReference) && *pReference != '+' &&
            *pReference != '-' && *pReference != '.') return XFALSE;
    }

    return XFALSE;
}

/* The address a Location sends the request to (RFC 9110 10.2.2): an absolute one as it is, any other merged with
   the request's own as RFC 3986 5.2 merges them, with its scheme. Taken as it was, a path lost the scheme, so a
   redirect to /login on an https server went to port 443 in plain text, and a relative path was taken for a host. */
void XHTTPApp_ResolveLocation(char *pAddress, size_t nSize, const xlink_t *pLink, const char *pLocation)
{
    const char *pScheme = xstrused(pLink->sProtocol) ? pLink->sProtocol : "http";

    if (XHTTPApp_HasScheme(pLocation))
    {
        xstrncpy(pAddress, nSize, pLocation);
        return;
    }

    if (pLocation[0] == '/' && pLocation[1] == '/')
    {
        xstrncpyf(pAddress, nSize, "%s:%s", pScheme, pLocation);
        return;
    }

    if (pLocation[0] == '/')
    {
        xstrncpyf(pAddress, nSize, "%s://%s%s", pScheme, pLink->sHost, pLocation);
        return;
    }

    /* A query alone replaces the request's query, and a relative path the last segment of its path. Dot
       segments are left for the server to resolve. */
    size_t nBase = strcspn(pLink->sUri, "?#");
    if (pLocation[0] == '?' || pLocation[0] == '#')
    {
        xstrncpyf(pAddress, nSize, "%s://%s%.*s%s", pScheme, pLink->sHost, (int)nBase, pLink->sUri, pLocation);
        return;
    }

    while (nBase > 0 && pLink->sUri[nBase - 1] != '/') nBase--;

    if (!nBase) xstrncpyf(pAddress, nSize, "%s://%s/%s", pScheme, pLink->sHost, pLocation);
    else xstrncpyf(pAddress, nSize, "%s://%s%.*s%s", pScheme, pLink->sHost, (int)nBase, pLink->sUri, pLocation);
}

int XHTTPApp_Perform(xhttp_args_t *pArgs, xlink_t *pLink)
{
    xhttp_t handle;
    XHTTP_InitRequest(&handle, pArgs->eMethod, pLink->sUri, NULL);
    XHTTP_AddHeader(&handle, "Host", "%s", pLink->sAddr);
    XHTTP_AddHeader(&handle, "User-Agent", "xutils/%s", XUtils_VersionShort());
    XHTTP_SetUnixAddr(&handle, pArgs->sUnixAddr);
    handle.nTimeout = pArgs->nTimeout;

    uint16_t nCallbacks = XHTTP_ERROR | XHTTP_READ_CNT | XHTTP_WRITE | XHTTP_STATUS;
    XHTTP_SetCallback(&handle, XHTTPApp_Callback, pArgs, nCallbacks);

    if (xstrused(pArgs->sHeaders) && XHTTPApp_AppendArgHeaders(&handle, pArgs) < 0)
    {
        xloge("Failed to appen custom headers: %s (%d)", pArgs->sHeaders, errno);
        XHTTP_Clear(&handle);
        return XSTDERR;
    }

    xbool_t bHaveOutput = xstrused(pArgs->sOutput);
    xhttp_status_t eStatus;

    /* Only a complete response is one. Whatever came of any other was cut short: it is not printed, and a
       download of it stays in its .part file. Some of a body used to be taken for all of it. */
    eStatus = XHTTP_LinkPerform(&handle, pLink, pArgs->content.pData, pArgs->content.nUsed);
    if (eStatus != XHTTP_COMPLETE)
    {
        if (eStatus == XHTTP_BIGCNT) xlogi("Too big content. Try to use output file (-o <file>)");

        XFile_Free(&pArgs->pOutputFile);
        XHTTP_Clear(&handle);
        return XSTDERR;
    }

    if (pArgs->nDone && !handle.nContentLength)
        XProgBar_Finish(&pArgs->progressBar);

    if (pArgs->nAutoFollow &&
        handle.nStatusCode < 400 &&
        handle.nStatusCode >= 300)
    {
        const char *pStatus = XHTTP_GetCodeStr(handle.nStatusCode);
        xlogd("HTTP redirect: %d (%s)", handle.nStatusCode, pStatus);

        const char *pLocation = XHTTP_GetHeader(&handle, "Location");
        if (pLocation != NULL)
        {
            /* Redirects that lead back to where they started would be followed for ever */
            if (pArgs->nRedirects >= XHTTP_REDIRECT_MAX)
            {
                xloge("Too many redirects: stopped after %d", XHTTP_REDIRECT_MAX);
                XFile_Free(&pArgs->pOutputFile);
                XHTTP_Clear(&handle);
                return XSTDERR;
            }

            pArgs->nRedirects++;
            XHTTPApp_ResolveLocation(pArgs->sAddress, sizeof(pArgs->sAddress), pLink, pLocation);
            xlogd("Following location: %s", pArgs->sAddress);

            XFile_Free(&pArgs->pOutputFile);
            XHTTP_Clear(&handle);
            return XSTDOK;
        }
    }

    xbool_t bSuccess = XHTTP_IsSuccessCode(&handle);
    if (!bSuccess)
    {
        const char *pStatus = XHTTP_GetCodeStr(handle.nStatusCode);
        xlogw("HTTP response: %d (%s)", handle.nStatusCode, pStatus);
    }

    /* A download is done only once its file is: a response that is not a success has nothing to save */
    if (bHaveOutput)
    {
        XSTATUS nStatus = bSuccess ? XHTTPApp_FinishOutput(pArgs) : XSTDERR;
        XFile_Free(&pArgs->pOutputFile);
        XHTTP_Clear(&handle);
        return nStatus < 0 ? XSTDERR : XSTDNON;
    }

    /* The body as it came, bytes after a NUL included. A terminal gets a newline after it. */
    const uint8_t *pBody = XHTTP_GetBody(&handle);
    size_t nBodySize = XHTTP_GetBodySize(&handle);

    if (pBody != NULL)
    {
        fwrite(pBody, 1, nBodySize, stdout);
        if (XHTTP_STDOUT_TTY) printf("\n");
        fflush(stdout);
    }

    XFile_Free(&pArgs->pOutputFile);
    XHTTP_Clear(&handle);
    return XSTDNON;
}

/* Performs the request, and the ones its redirects lead to: XHTTPApp_Perform() returns XSTDOK when it has
   another to make. */
int XHTTPApp_Run(xhttp_args_t *pArgs)
{
    int nStatus = XSTDOK;

    while (nStatus == XSTDOK)
    {
        xlink_t link;
        nStatus = XHTTPApp_Prepare(pArgs, &link);
        if (nStatus < 0) break;
        nStatus = XHTTPApp_Perform(pArgs, &link);
    }

    return nStatus;
}

int main(int argc, char* argv[])
{
    xlog_defaults();
    xlog_cfg_t xcfg;

    xlog_get(&xcfg);
    xcfg.eColorFormat = XLOG_COLORING_FULL;
    xcfg.bUseHeap = XTRUE;
    xcfg.bIndent = XTRUE;
    xcfg.nFlags |= XLOG_INFO;
    xlog_set(&xcfg);

    xhttp_args_t args;
    if (!XHTTPApp_ParseArgs(&args, argc, argv))
    {
        XHTTPApp_DisplayUsage(argv[0]);
        return XSTDERR;
    }

    int nStatus = XHTTPApp_Run(&args);
    XByteBuffer_Clear(&args.content);
    XSock_DeinitSSL();
    return nStatus;
}
