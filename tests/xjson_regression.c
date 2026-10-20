/* libxutils: the xjson tool, run through its main as the shell runs it: what each of -l, -m and -p writes, how
 * every input it cannot use fails, and what it does with every allocation it makes failing in turn. The tool is
 * compiled in with its main renamed. */
#include "alloc_inject.h"
#include <fcntl.h>
#include <getopt.h>
#include <sys/stat.h>
#include <unistd.h>

#define main xjson_main
#include "../tools/xjson.c"
#undef main

/* ---------------- a failing read ---------------- */

/* fread() ends the same way at the end of the input and at a read that failed, ferror() tells them apart: made
   to say the read failed, it stands for a failure after some of the input had come */
static int g_bReadFails = 0;
int __real_ferror(FILE *pStream);

int __wrap_ferror(FILE *pStream)
{
    if (g_bReadFails && pStream == stdin) return 1;
    return __real_ferror(pStream);
}

/* ---------------- fixtures ---------------- */

static char g_sDir[128];
static char g_sOut[65536];
static size_t g_nOut;

static const char *g_pInput = "{\"a\":[1,2,{\"b\":null}],\"c\":\"x\"}";

static int dir_begin(void)
{
    strcpy(g_sDir, "/tmp/xutils-xjson-XXXXXX");
    CHECK(mkdtemp(g_sDir) != NULL, "Create a directory of its own");
    return 0;
}

static void dir_end(void)
{
    XDir_Remove(g_sDir);
}

static void path_to(char *pPath, size_t nSize, const char *pName)
{
    snprintf(pPath, nSize, "%s/%s", g_sDir, pName);
}

static int file_put(const char *pName, const char *pData)
{
    char sPath[256];
    path_to(sPath, sizeof(sPath), pName);
    int nFD = open(sPath, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (nFD < 0) return XSTDERR;
    ssize_t nWritten = write(nFD, pData, strlen(pData));
    close(nFD);
    return nWritten == (ssize_t)strlen(pData) ? XSTDOK : XSTDERR;
}

/* The output without its color sequences */
static void plain_text(const char *pText, char *pPlain, size_t nSize)
{
    size_t nUsed = 0;
    while (*pText && nUsed + 1 < nSize)
    {
        if (*pText == '\033')
        {
            while (*pText && *pText != 'm') pText++;
            if (*pText) pText++;
            continue;
        }

        pPlain[nUsed++] = *pText++;
    }

    pPlain[nUsed] = XSTR_NUL;
}

/* Runs the tool with these arguments after its name, standard input from pStdin when it is given. What it
   writes to standard output, its errors with the rest, is kept in g_sOut. Returns its exit status. */
static int xjson_run(const char **pArgs, const char *pStdin)
{
    const char *argv[16] = { "xjson" };
    int argc = 1;
    while (pArgs[argc - 1] != NULL && argc < 15) { argv[argc] = pArgs[argc - 1]; argc++; }
    argv[argc] = NULL;

    char sCapture[256], sInput[256];
    path_to(sCapture, sizeof(sCapture), "stdout");
    path_to(sInput, sizeof(sInput), "stdin");

    int nSavedIn = -1;
    if (pStdin != NULL)
    {
        if (file_put("stdin", pStdin) < 0) return 99;
        int nInput = open(sInput, O_RDONLY);
        if (nInput < 0) return 99;

        nSavedIn = dup(STDIN_FILENO);
        dup2(nInput, STDIN_FILENO);
        close(nInput);
        clearerr(stdin);
    }

    fflush(stdout);
    int nSaved = dup(STDOUT_FILENO);
    int nCapture = open(sCapture, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (nSaved < 0 || nCapture < 0) return 99;
    dup2(nCapture, STDOUT_FILENO);

    /* A process starts with the default log flags, which xlog_defaults() does not set once the log is set up */
    optind = 0;
    xlog_setfl(XLOG_DEFAULT);
    int nStatus = xjson_main(argc, (char**)argv);
    xlog_setfl(XLOG_NONE);

    fflush(stdout);
    dup2(nSaved, STDOUT_FILENO);
    close(nSaved);

    if (nSavedIn >= 0)
    {
        dup2(nSavedIn, STDIN_FILENO);
        close(nSavedIn);
        clearerr(stdin);
    }

    lseek(nCapture, 0, SEEK_SET);
    ssize_t nRead = read(nCapture, g_sOut, sizeof(g_sOut) - 1);
    g_nOut = nRead > 0 ? (size_t)nRead : 0;
    g_sOut[g_nOut] = XSTR_NUL;
    close(nCapture);
    return nStatus;
}

static const char *g_pIndented4 =
    "{\n    \"a\": [\n        1,\n        2,\n        {\n            \"b\": null\n        }\n    ],\n    \"c\": \"x\"\n}\n";
static const char *g_pIndented2 =
    "{\n  \"a\": [\n    1,\n    2,\n    {\n      \"b\": null\n    }\n  ],\n  \"c\": \"x\"\n}\n";
static const char *g_pMinified = "{\"a\":[1,2,{\"b\":null}],\"c\":\"x\"}\n";

/* ---------------- the cases ---------------- */

static int XTest_output_modes(void)
{
    CHECK(dir_begin() == 0 && file_put("in.json", g_pInput) == XSTDOK, "Write the input");
    char sInput[256], sPlain[8192];
    path_to(sInput, sizeof(sInput), "in.json");

    const char *pDefault[] = { "-i", sInput, NULL };
    CHECK(xjson_run(pDefault, NULL) == 0 && !strcmp(g_sOut, g_pIndented4), "By default, indented by four");

    const char *pTab[] = { "-i", sInput, "-l", "2", NULL };
    CHECK(xjson_run(pTab, NULL) == 0 && !strcmp(g_sOut, g_pIndented2), "-l sets the indent");

    const char *pNone[] = { "-i", sInput, "-l", "0", NULL };
    CHECK(xjson_run(pNone, NULL) == 0 && !strcmp(g_sOut, g_pMinified), "-l 0 indents nothing");

    const char *pMinify[] = { "-i", sInput, "-m", "-l", "2", NULL };
    CHECK(xjson_run(pMinify, NULL) == 0 && !strcmp(g_sOut, g_pMinified), "-m writes one line, whatever -l says");

    const char *pMinifyLast[] = { "-i", sInput, "-l", "8", "-m", NULL };
    CHECK(xjson_run(pMinifyLast, NULL) == 0 && !strcmp(g_sOut, g_pMinified), "Before it or after it");

    /* -p colors the same text */
    const char *pPretty[] = { "-i", sInput, "-p", NULL };
    CHECK(xjson_run(pPretty, NULL) == 0 && strchr(g_sOut, '\033') != NULL, "-p colors the output");
    plain_text(g_sOut, sPlain, sizeof(sPlain));
    CHECK(!strcmp(sPlain, g_pIndented4), "Which is the same text in its colors");

    const char *pBoth[] = { "-i", sInput, "-m", "-p", NULL };
    CHECK(xjson_run(pBoth, NULL) == 0 && strchr(g_sOut, '\033') != NULL, "-m and -p together");
    plain_text(g_sOut, sPlain, sizeof(sPlain));
    CHECK(!strcmp(sPlain, g_pMinified), "Color the line -m writes");

    /* Standard input without -i */
    const char *pStdin[] = { "-m", NULL };
    CHECK(xjson_run(pStdin, g_pInput) == 0 && !strcmp(g_sOut, g_pMinified), "Without -i, standard input is read");

    dir_end();
    return 0;
}

static int XTest_input_failures(void)
{
    CHECK(dir_begin() == 0, "Set up the directory");
    char sInput[256];

    const char *pHelp[] = { "-h", NULL };
    CHECK(xjson_run(pHelp, NULL) == 1 && strstr(g_sOut, "Usage:") != NULL, "-h shows the usage");

    path_to(sInput, sizeof(sInput), "missing.json");
    const char *pMissing[] = { "-i", sInput, NULL };
    CHECK(xjson_run(pMissing, NULL) == 1, "A file that is not there fails");

    CHECK(file_put("empty.json", "") == XSTDOK, "An empty file");
    path_to(sInput, sizeof(sInput), "empty.json");
    const char *pEmpty[] = { "-i", sInput, NULL };
    CHECK(xjson_run(pEmpty, NULL) == 1 && strstr(g_sOut, "(empty)") != NULL, "Fails as empty, not with an error it has not had");

    CHECK(file_put("broken.json", "{\"a\":1,}") == XSTDOK, "A broken document");
    path_to(sInput, sizeof(sInput), "broken.json");
    const char *pBroken[] = { "-i", sInput, NULL };
    CHECK(xjson_run(pBroken, NULL) == 1 && strstr(g_sOut, "Failed to parse JSON") != NULL, "Fails to parse");

    const char *pStdin[] = { NULL };
    CHECK(xjson_run(pStdin, "") == 1 && strstr(g_sOut, "No input on stdin") != NULL, "Nothing on standard input fails");

    /* A read that failed after some of the input: what came is a whole number, and is not taken for the input */
    g_bReadFails = 1;
    int nStatus = xjson_run(pStdin, "12345");
    g_bReadFails = 0;
    CHECK(nStatus == 1 && strstr(g_sOut, "Failed to read from stdin") != NULL && strstr(g_sOut, "12345") == NULL,
        "A failed read fails, and nothing it read is written");

    dir_end();
    return 0;
}

/* ---------------- the allocation sweep ---------------- */

static const char *g_pSweepArgs[8];
static const char *g_pSweepExpected;

/* The tool, failing one allocation at a time: it says it succeeded only when what it wrote is all of it, and it
   frees what it took whichever allocation failed. It used to say it succeeded after it had failed to write. */
static int scn_xjson(void)
{
    int nStatus = xjson_run(g_pSweepArgs, NULL);
    if (nStatus != 0 && nStatus != 1) return 1;

    if (!nStatus && g_pSweepExpected != NULL && strcmp(g_sOut, g_pSweepExpected)) return 1;
    if (!nStatus && g_pSweepExpected == NULL && strchr(g_sOut, '\033') == NULL) return 1;
    return 0;
}

static int XTest_alloc_sweep(void)
{
    CHECK(dir_begin() == 0 && file_put("in.json", g_pInput) == XSTDOK, "Write the input");
    char sInput[256];
    path_to(sInput, sizeof(sInput), "in.json");

    g_pSweepArgs[0] = "-i";
    g_pSweepArgs[1] = sInput;
    g_pSweepArgs[2] = "-l";
    g_pSweepArgs[3] = "2";
    g_pSweepArgs[4] = NULL;
    g_pSweepExpected = g_pIndented2;

    const alloc_case_t indented = { "xjson-indented", scn_xjson };
    CHECK(alloc_sweep(&indented) == 0, "Indented, it fails whole at every allocation, or succeeds whole");

    /* The colors take allocations of their own */
    g_pSweepArgs[2] = "-p";
    g_pSweepArgs[3] = NULL;
    g_pSweepExpected = NULL;

    const alloc_case_t pretty = { "xjson-pretty", scn_xjson };
    CHECK(alloc_sweep(&pretty) == 0, "Colored too");

    /* A document bigger than a block of the pool: the writer has to grow past it, which can fail too */
    static char sLarge[3000 * 13 + 3];
    static char sExpected[3000 * 15 + 8];
    size_t nLarge = 0, nExpected = 0;

    sLarge[nLarge++] = '[';
    nExpected += (size_t)snprintf(sExpected, sizeof(sExpected), "[\n");
    for (int i = 0; i < 3000; i++)
    {
        nLarge += (size_t)snprintf(sLarge + nLarge, sizeof(sLarge) - nLarge, "%s\"%09d\"", i ? "," : "", i);
        nExpected += (size_t)snprintf(sExpected + nExpected, sizeof(sExpected) - nExpected, "  \"%09d\"%s\n",
            i, i + 1 < 3000 ? "," : "");
    }
    sLarge[nLarge++] = ']';
    sLarge[nLarge] = XSTR_NUL;
    snprintf(sExpected + nExpected, sizeof(sExpected) - nExpected, "]\n");

    CHECK(file_put("large.json", sLarge) == XSTDOK, "Write the large input");
    path_to(sInput, sizeof(sInput), "large.json");
    g_pSweepArgs[1] = sInput;
    g_pSweepArgs[2] = "-l";
    g_pSweepArgs[3] = "2";
    g_pSweepArgs[4] = NULL;
    g_pSweepExpected = sExpected;

    CHECK(xjson_run(g_pSweepArgs, NULL) == 0 && !strcmp(g_sOut, sExpected), "Written whole when nothing fails");
    const alloc_case_t large = { "xjson-large", scn_xjson };
    CHECK(alloc_sweep(&large) == 0, "And when the writer cannot grow, it fails, and says so");

    dir_end();
    return 0;
}

XTEST_MAIN(
    XTEST_CASE(output_modes),
    XTEST_CASE(input_failures),
    XTEST_CASE(alloc_sweep)
)
