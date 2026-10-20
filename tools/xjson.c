/*!
 *  @file libxutils/examples/json.c
 *
 *  This source is part of the "libxutils" project
 *  2019-2021  Sandro Kalatozishvili (s.kalatoz@gmail.com)
 *
 * @brief Parse, lint and minify json using xjson library.
 */

#include "xstd.h"
#include "str.h"
#include "log.h"
#include "xfs.h"
#include "pool.h"
#include "json.h"
#include "buf.h"

extern char *optarg;

#define XJSON_LINT_VER_MAX  0
#define XJSON_LINT_VER_MIN  5

#define XJSON_POOL_SIZE     1024 * 64

typedef struct xjson_args_ {
    char sFile[XPATH_MAX];
    uint16_t nTabSize;
    uint8_t nMinify;
    uint8_t nPretty;
} xjson_args_t;

void XJSON_DisplayUsage(const char *pName)
{
    xlog("======================================================");
    xlog(" XJSON - Lint / Minify JSON file - v%d.%d (%s)",
        XJSON_LINT_VER_MAX, XJSON_LINT_VER_MIN, __DATE__);
    xlog("======================================================");

    xlog("Usage: %s [-i <path>] [-l <size>] [-m] [-p] [-h]\n", pName);
    xlog("Options are:");
    xlog("  -i <path>           # Input file path, standard input without it");
    xlog("  -l <size>           # Indent with <size> spaces, 4 by default, 0 for none");
    xlog("  -m                  # Minify: one line, no indent, whatever -l says");
    xlog("  -p                  # Colorize names and values for a terminal");
    xlog("  -h                  # Version and usage\n");
    xlog("Output is the JSON indented by 4 spaces unless -l or -m say otherwise.");
    xlog("With -p, it has terminal color sequences in it, and is no longer JSON.\n");
    xlog("Examples:");
    xlog("1) %s -i example.json -pl 4", pName);
    xlog("2) cat example.json | %s -p\n", pName);
}

int XJSON_ParseArgs(xjson_args_t *pArgs, int argc, char *argv[])
{
    xstrnul(pArgs->sFile);
    pArgs->nTabSize = 4;
    pArgs->nMinify = 0;
    pArgs->nPretty = 0;
    int nChar = 0;

    while ((nChar = getopt(argc, argv, "i:l:m1:p1:h1")) != -1)
    {
        switch (nChar)
        {
            case 'i':
                xstrncpy(pArgs->sFile, sizeof(pArgs->sFile), optarg);
                break;
            case 'l':
                pArgs->nTabSize = atoi(optarg);
                break;
            case 'm':
                pArgs->nMinify = 1;
                break;
            case 'p':
                pArgs->nPretty = 1;
                break;
            case 'h':
            default:
                return XFALSE;
        }
    }

    return XTRUE;
}

/* Returns the bytes read, or XSTDERR when the read failed: what came before a failed read is not all of the
   input, and parsed as if it were, a cut document could pass for a whole one */
int XJSon_ReadFromStdin(xbyte_buffer_t *pBuffer)
{
    XByteBuffer_Init(pBuffer, XSTR_MID, XFALSE);
    if (XByteBuffer_ReadStdin(pBuffer) < 0) return XSTDERR;
    return (int)pBuffer->nUsed;
}

int main(int argc, char *argv[])
{
    xlog_defaults();
    xbyte_buffer_t buffer;
    xjson_args_t args;
    xjson_t json;

    if (!XJSON_ParseArgs(&args, argc, argv))
    {
        XJSON_DisplayUsage(argv[0]);
        return 1;
    }

    if (xstrused(args.sFile))
    {
        errno = 0;
        if (XPath_LoadBuffer(args.sFile, &buffer) <= 0)
        {
            xloge("Failed to load file: %s (%s)", args.sFile, errno ? XSTRERR : "empty");
            return 1;
        }
    }
    else
    {
        errno = 0;
        int nRead = XJSon_ReadFromStdin(&buffer);

        if (nRead <= 0)
        {
            if (nRead < 0) xloge("Failed to read from stdin: %s", errno ? XSTRERR : "read error");
            else xloge("No input on stdin");

            XByteBuffer_Clear(&buffer);
            return 1;
        }
    }

    xpool_t pool;
    if (XPool_Init(&pool, XJSON_POOL_SIZE) != XSTDOK)
    {
        xloge("Failed to allocate memory pool: %s", XSTRERR);
        XByteBuffer_Clear(&buffer);
        XPool_Destroy(&pool);
        return 1;
    }

    if (!XJSON_Parse(&json, &pool, (const char*)buffer.pData, buffer.nUsed))
    {
        char sError[256];
        XJSON_GetErrorStr(&json, sError, sizeof(sError));
        xloge("Failed to parse JSON: %s", sError);

        XByteBuffer_Clear(&buffer);
        XPool_Destroy(&pool);
        XJSON_Destroy(&json);
        return 1;
    }

    xjson_writer_t writer;
    int nStatus = XJSON_InitWriter(&writer, &pool, NULL, buffer.nUsed);
    writer.nTabSize = !args.nMinify ? args.nTabSize : 0;
    writer.nPretty = args.nPretty;

    if (nStatus && XJSON_WriteObject(json.pRootObj, &writer)) printf("%s\n", writer.pData);
    else
    {
        xloge("Failed to serialize json: errno(%d) %s", errno, writer.pData ? writer.pData : "");
        nStatus = XJSON_FAILURE;
    }

    XJSON_DestroyWriter(&writer);
    XByteBuffer_Clear(&buffer);
    XJSON_Destroy(&json);
    XPool_Destroy(&pool);

    return nStatus ? 0 : 1;
}
