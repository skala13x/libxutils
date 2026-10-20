/* libxutils: the xsrc tool, the read cap its -b sets and the files its -c rewrites. The tool is compiled in with
 * its main renamed, and the calls that put a rewritten file in place are wrapped so that any one of them can fail. */
#include "test.h"
#include <dirent.h>
#include <fcntl.h>
#include <getopt.h>
#include <sys/stat.h>
#include <unistd.h>

#define main xsrc_main
#include "../tools/xsrc.c"
#undef main

/* ---------------- the failing calls ---------------- */

static int g_bArmed = 0;
static int g_nWrites = 0;
static int g_nFailWrite = 0;        /* The armed write that fails, counted from 1 */
static int g_nInterruptWrite = 0;   /* The armed write that a signal interrupts before it writes anything */
static size_t g_nShortWrite = 0;    /* The most an armed write takes, 0 for all of it */
static int g_bFailFsync = 0;
static int g_bFailFchmod = 0;
static int g_bFailClose = 0;
static int g_bFailRename = 0;
static int g_bFailMkstemp = 0;

ssize_t __real_write(int nFD, const void *pData, size_t nSize);
int __real_fsync(int nFD);
int __real_fchmod(int nFD, mode_t nMode);
int __real_close(int nFD);
int __real_rename(const char *pOld, const char *pNew);
int __real_mkstemp(char *pTemplate);

ssize_t __wrap_write(int nFD, const void *pData, size_t nSize)
{
    if (g_bArmed)
    {
        if (++g_nWrites == g_nFailWrite)
        {
            errno = ENOSPC;
            return -1;
        }

        if (g_nWrites == g_nInterruptWrite)
        {
            errno = EINTR;
            return -1;
        }

        if (g_nShortWrite && nSize > g_nShortWrite) nSize = g_nShortWrite;
    }

    return __real_write(nFD, pData, nSize);
}

int __wrap_fsync(int nFD)
{
    if (g_bArmed && g_bFailFsync) { errno = EIO; return -1; }
    return __real_fsync(nFD);
}

int __wrap_fchmod(int nFD, mode_t nMode)
{
    if (g_bArmed && g_bFailFchmod) { errno = EPERM; return -1; }
    return __real_fchmod(nFD, nMode);
}

/* A failed close has still closed the descriptor */
int __wrap_close(int nFD)
{
    int nStatus = __real_close(nFD);
    if (g_bArmed && g_bFailClose) { errno = EIO; return -1; }
    return nStatus;
}

int __wrap_rename(const char *pOld, const char *pNew)
{
    if (g_bArmed && g_bFailRename) { errno = EXDEV; return -1; }
    return __real_rename(pOld, pNew);
}

int __wrap_mkstemp(char *pTemplate)
{
    if (g_bArmed && g_bFailMkstemp) { errno = EACCES; return -1; }
    return __real_mkstemp(pTemplate);
}

static void faults_clear(void)
{
    g_bArmed = 0;
    g_nWrites = 0;
    g_nFailWrite = 0;
    g_nInterruptWrite = 0;
    g_nShortWrite = 0;
    g_bFailFsync = g_bFailFchmod = g_bFailClose = 0;
    g_bFailRename = g_bFailMkstemp = 0;
}

/* ---------------- fixtures ---------------- */

static char g_sDir[128];

static int dir_begin(void)
{
    strcpy(g_sDir, "/tmp/xutils-xsrc-XXXXXX");
    CHECK(mkdtemp(g_sDir) != NULL, "Create a directory of its own");
    return 0;
}

static void dir_end(void)
{
    XDir_Remove(g_sDir);
    g_sDir[0] = XSTR_NUL;
}

static int file_put(const char *pName, const char *pData, size_t nSize, mode_t nMode)
{
    char sPath[XPATH_MAX];
    snprintf(sPath, sizeof(sPath), "%s/%s", g_sDir, pName);
    int nFD = open(sPath, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    CHECK(nFD >= 0, "Create the file");
    CHECK(__real_write(nFD, pData, nSize) == (ssize_t)nSize, "Write all of it");
    CHECK(fchmod(nFD, nMode) == 0 && __real_close(nFD) == 0, "With its permissions");
    return 0;
}

/* The file's content, or NULL; free() it */
static char *file_get(const char *pName, size_t *pSize)
{
    char sPath[XPATH_MAX];
    snprintf(sPath, sizeof(sPath), "%s/%s", g_sDir, pName);
    xbyte_buffer_t buffer;
    if (XPath_LoadBuffer(sPath, &buffer) <= 0) return NULL;
    *pSize = buffer.nUsed;
    return (char*)buffer.pData;
}

static int file_is(const char *pName, const char *pData)
{
    size_t nSize = 0;
    char *pContent = file_get(pName, &nSize);
    int bSame = pContent != NULL && nSize == strlen(pData) && !memcmp(pContent, pData, nSize);
    free(pContent);
    return bSame;
}

static mode_t file_mode(const char *pName)
{
    char sPath[XPATH_MAX];
    snprintf(sPath, sizeof(sPath), "%s/%s", g_sDir, pName);
    struct stat st;
    return stat(sPath, &st) == 0 ? (st.st_mode & 07777) : (mode_t)-1;
}

static size_t dir_count(void)
{
    DIR *pDir = opendir(g_sDir);
    if (pDir == NULL) return 0;

    size_t nCount = 0;
    struct dirent *pEntry;
    while ((pEntry = readdir(pDir)) != NULL)
        if (strcmp(pEntry->d_name, ".") && strcmp(pEntry->d_name, "..")) nCount++;

    closedir(pDir);
    return nCount;
}

/* Strips a file in the directory as -c does, with the arguments given */
static XSTATUS strip_file(const char *pName, xbool_t bFinalNewline)
{
    xsearch_args_t args;
    memset(&args, 0, sizeof(args));
    args.bFinalNewline = bFinalNewline;

    char sPath[XPATH_MAX];
    snprintf(sPath, sizeof(sPath), "%s/%s", g_sDir, pName);
    xstat_t st;
    if (xstat(sPath, &st) < 0) return 99;

    char sDir[XPATH_MAX];
    snprintf(sDir, sizeof(sDir), "%s/", g_sDir);

    xsearch_entry_t entry;
    XSearch_CreateEntry(&entry, pName, sDir, &st);
    XSTATUS nStatus = XSearch_StripEmptySpaces(&args, &entry);
    free(entry.pRealPath);
    return nStatus;
}

static int parse_args(xsearch_args_t *pArgs, int argc, const char **argv)
{
    optind = 0;
    return XSearch_ParseArgs(pArgs, argc, (char**)argv);
}

/* ---------------- the cases ---------------- */

typedef struct {
    int nFound;
    char sNames[4][XNAME_MAX];
} found_t;

static int found_callback(xsearch_t *pSearch, xsearch_entry_t *pEntry, const char *pMsg)
{
    found_t *pFound = (found_t*)pSearch->pUserCtx;
    (void)pMsg;

    if (pEntry != NULL && pFound->nFound < 4)
        xstrncpy(pFound->sNames[pFound->nFound++], XNAME_MAX, pEntry->sName);

    return XSTDNON;
}

static int search_found(const char *pBuffer, found_t *pFound)
{
    char sDir[XPATH_MAX];
    snprintf(sDir, sizeof(sDir), "%s/", g_sDir);

    const char *argv[] = { "xsrc", "-g", "needle", "-b", pBuffer, NULL };
    int argc = pBuffer != NULL ? 5 : 3;

    xsearch_args_t args;
    CHECK(parse_args(&args, argc, argv), "The arguments parse");

    xsearch_t search;
    XSearch_SetupArgs(&search, &args);

    /* What the search finds is counted instead of printed */
    memset(pFound, 0, sizeof(*pFound));
    search.callback = found_callback;
    search.pUserCtx = pFound;

    /* XSearch_Destroy() raises the interrupt flag, which a search before this one has left up */
    XSYNC_ATOMIC_SET(&g_interrupted, 0);
    XSearch(&search, sDir);
    XSearch_Destroy(&search);
    return 0;
}

static int XTest_read_buffer(void)
{
    xsearch_args_t args;
    xsearch_t search;

    const char *argv[] = { "xsrc", "-b", "4k", "-g", "needle", NULL };
    CHECK(parse_args(&args, 5, argv) && args.nMaxRead == 4096, "-b 4k is 4096 bytes");
    XSearch_SetupArgs(&search, &args);
    CHECK(search.nBufferSize == 4096, "And it is the most the search reads of a file");
    CHECK(!strcmp(search.sText, "needle") && search.callback == XSearch_Callback && search.pUserCtx == &args &&
        search.pInterrupted == &g_interrupted, "With the rest of the arguments");
    XSearch_Destroy(&search);

    const char *defaults[] = { "xsrc", "-g", "needle", NULL };
    CHECK(parse_args(&args, 3, defaults), "No -b parses");
    XSearch_SetupArgs(&search, &args);
    CHECK(search.nBufferSize == XSEARCH_MAX_READ_SIZE, "Without it, a file is read up to 1G");
    XSearch_Destroy(&search);

    const char *invalid[] = { "xsrc", "-b", "big", NULL };
    CHECK(!parse_args(&args, 3, invalid), "A size that is not one is refused");

    /* The cap at work: text past it is not found, text within it is */
    CHECK(dir_begin() == 0, "Set up the directory");
    char sFar[9000];
    memset(sFar, 'a', sizeof(sFar));
    memcpy(sFar + 8000, "needle", 6);
    CHECK(file_put("far.txt", sFar, sizeof(sFar), 0644) == 0 && file_put("near.txt", "needle\n", 7, 0644) == 0,
        "One file with the text past 4k, one with it at the start");

    found_t found;
    CHECK(search_found("4k", &found) == 0 && found.nFound == 1 && !strcmp(found.sNames[0], "near.txt"),
        "With -b 4k, only the text within the first 4k is found");
    CHECK(search_found("16k", &found) == 0 && found.nFound == 2, "With -b 16k, both are");
    CHECK(search_found(NULL, &found) == 0 && found.nFound == 2, "And by default");

    dir_end();
    return 0;
}

static int XTest_strip_content(void)
{
    CHECK(dir_begin() == 0, "Set up the directory");

    const char *pDirty = "a  \nb\t\n   \nc   ";
    CHECK(file_put("dirty.txt", pDirty, strlen(pDirty), 0644) == 0, "A file with trailing spaces");
    CHECK(strip_file("dirty.txt", XFALSE) == XSTDOK, "It is rewritten");
    CHECK(file_is("dirty.txt", "a\nb\n\nc"), "Without them");

    CHECK(file_put("final.txt", pDirty, strlen(pDirty), 0644) == 0, "The same file again");
    CHECK(strip_file("final.txt", XTRUE) == XSTDOK && file_is("final.txt", "a\nb\n\nc\n"),
        "With -w, it ends in a newline");

    /* A clean file is left as it is, the same file */
    CHECK(file_put("clean.txt", "a\nb\n", 4, 0644) == 0, "A clean file");
    struct stat before, after;
    char sPath[XPATH_MAX];
    snprintf(sPath, sizeof(sPath), "%s/clean.txt", g_sDir);
    CHECK(stat(sPath, &before) == 0 && strip_file("clean.txt", XFALSE) == XSTDNON, "Nothing to strip");
    CHECK(stat(sPath, &after) == 0 && before.st_ino == after.st_ino && file_is("clean.txt", "a\nb\n"),
        "And it is not rewritten");

    CHECK(dir_count() == 3, "And nothing else is left in the directory");
    dir_end();
    return 0;
}

static int XTest_strip_atomic(void)
{
    CHECK(dir_begin() == 0, "Set up the directory");

    /* A file that has the name the old backup took is someone's file */
    const char *pDirty = "one  \ntwo \n";
    CHECK(file_put("f.txt", pDirty, strlen(pDirty), 0640) == 0, "A file to strip, with its own permissions");
    CHECK(file_put("f.txt.xsrc.bak", "keep me\n", 8, 0600) == 0, "And another one beside it");

    CHECK(strip_file("f.txt", XFALSE) == XSTDOK, "The file is rewritten");
    CHECK(file_is("f.txt", "one\ntwo\n") && file_mode("f.txt") == 0640, "With its permissions");
    CHECK(file_is("f.txt.xsrc.bak", "keep me\n") && file_mode("f.txt.xsrc.bak") == 0600,
        "The file beside it is untouched");
    CHECK(dir_count() == 2, "And nothing is left behind");

    /* Wherever the content goes in pieces, it goes in whole */
    CHECK(file_put("pieces.txt", pDirty, strlen(pDirty), 0604) == 0, "Another file to strip");
    g_bArmed = 1;
    g_nShortWrite = 2;
    XSTATUS nStatus = strip_file("pieces.txt", XFALSE);
    int nWrites = g_nWrites;
    faults_clear();
    CHECK(nStatus == XSTDOK && nWrites == 4, "Written two bytes at a time");
    CHECK(file_is("pieces.txt", "one\ntwo\n") && file_mode("pieces.txt") == 0604, "It is all there");

    /* And a write a signal interrupted is made again */
    CHECK(file_put("pieces.txt", pDirty, strlen(pDirty), 0604) == 0, "The file to strip again");
    g_bArmed = 1;
    g_nShortWrite = 2;
    g_nInterruptWrite = 2;
    nStatus = strip_file("pieces.txt", XFALSE);
    nWrites = g_nWrites;
    faults_clear();
    CHECK(nStatus == XSTDOK && nWrites == 5 && file_is("pieces.txt", "one\ntwo\n"),
        "An interrupted write is made again, and the file is all there");

    /* A name too long for the new file next to it is refused before anything is written */
    char sLong[XPATH_MAX + 16];
    memset(sLong, 'a', sizeof(sLong) - 1);
    sLong[sizeof(sLong) - 1] = XSTR_NUL;
    xbyte_buffer_t buffer;
    XByteBuffer_Init(&buffer, 0, XFALSE);
    xsearch_entry_t entry;
    XSearch_InitEntry(&entry);
    errno = 0;
    CHECK(XSearch_ReplaceFile(sLong, &buffer, &entry) == XSTDERR && errno == ENAMETOOLONG,
        "A path with no room for the new file's name is refused");

    CHECK(dir_count() == 3, "Nothing else is in the directory");
    dir_end();
    return 0;
}

static int XTest_strip_failures(void)
{
    const char *pDirty = "keep  \nthis \n";
    const char *pFailures[] = { "mkstemp", "fchmod", "write", "later write", "fsync", "close", "rename" };

    for (size_t i = 0; i < sizeof(pFailures) / sizeof(*pFailures); i++)
    {
        CHECK(dir_begin() == 0, "Set up the directory");
        CHECK(file_put("f.txt", pDirty, strlen(pDirty), 0640) == 0, "A file to strip");

        g_bArmed = 1;
        switch (i)
        {
            case 0: g_bFailMkstemp = 1; break;
            case 1: g_bFailFchmod = 1; break;
            case 2: g_nFailWrite = 1; break;
            case 3: g_nShortWrite = 3; g_nFailWrite = 2; break;
            case 4: g_bFailFsync = 1; break;
            case 5: g_bFailClose = 1; break;
            default: g_bFailRename = 1; break;
        }

        XSTATUS nStatus = strip_file("f.txt", XFALSE);
        faults_clear();

        if (nStatus != XSTDERR || !file_is("f.txt", pDirty) || file_mode("f.txt") != 0640 || dir_count() != 1)
        {
            fprintf(stderr, "Failing %s\n", pFailures[i]);
            CHECK(0, "A rewrite that fails leaves the file as it was, and nothing beside it");
        }

        dir_end();
    }

    return 0;
}

XTEST_MAIN(
    XTEST_CASE(read_buffer),
    XTEST_CASE(strip_content),
    XTEST_CASE(strip_atomic),
    XTEST_CASE(strip_failures)
)
