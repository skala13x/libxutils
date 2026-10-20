/*!
 *  @file libxutils/examples/xlog.c
 *
 *  This source is part of the "libxutils" project
 *  2015-2026  Sandro Kalatozishvili (s.kalatoz@gmail.com)
 *
 * @brief Example of working with the logger: log levels and what is
 * shown by default, the format of the lines, returning from a function
 * through a log line, a callback that sees every line and decides where
 * it goes, and logging into a file.
 */

#include "xstd.h"
#include "log.h"
#include "str.h"
#include "xfs.h"

#define XLOG_EXAMPLE_NAME   "xlog-example"

typedef struct {
    xbool_t bQuiet;
    int nLines;
    int nErrors;
} xlog_stats_t;

/* Gets every line with its level. Returns >0 to show it on the screen as well,
   0 to keep it off the screen and <0 to keep it out of the log file too. */
static int log_callback(const char *pLog, size_t nLength, xlog_flag_t eFlag, void *pCtx)
{
    (void)pLog;
    (void)nLength;

    xlog_stats_t *pStats = (xlog_stats_t*)pCtx;
    if (eFlag == XLOG_ERROR) pStats->nErrors++;
    pStats->nLines++;

    return pStats->bQuiet ? 0 : 1;
}

/* xthrowe() logs an error with the text of errno and returns the given value */
static XSTATUS open_config(const char *pPath)
{
    xfile_t file;
    if (XFile_Open(&file, pPath, NULL, NULL) < 0)
        return xthrowe("Can not open config: %s", pPath);

    XFile_Close(&file);
    return XSTDOK;
}

int main()
{
//////////////////////////////////////////////////////////////////////////////////////////////
// Log levels: notes, info and debug lines are off by default
//////////////////////////////////////////////////////////////////////////////////////////////

    xlog_init(XLOG_EXAMPLE_NAME, XLOG_DEFAULT, XFALSE);

    xlog("Lines without a level are always shown");
    xlogi("This info line is not shown");
    xlogw("Warnings and errors are shown by default");

    xlog_enable(XLOG_NOTE | XLOG_INFO | XLOG_DEBUG);
    xlogn("A note");
    xlogi("An info line");
    xlogd("A debug line, debug is %s", xlog_enabled(XLOG_DEBUG) ? "enabled" : "disabled");

//////////////////////////////////////////////////////////////////////////////////////////////
// Format: time of the day, aligned messages
//////////////////////////////////////////////////////////////////////////////////////////////

    xlog_timing(XLOG_TIME);
    xlog_indent(XTRUE);

    xlogi("Lines have the time now");
    xloge("And the messages start in the same column");

//////////////////////////////////////////////////////////////////////////////////////////////
// Return from a function through a log line
//////////////////////////////////////////////////////////////////////////////////////////////

    if (open_config("/no/such/config.json") == XSTDERR)
        xlogw("Using the default configuration");

//////////////////////////////////////////////////////////////////////////////////////////////
// A callback sees every line, it can keep lines off the screen
//////////////////////////////////////////////////////////////////////////////////////////////

    xlog_stats_t stats = { XFALSE, 0, 0 };
    xlog_callback(log_callback, &stats);

    xlogi("Counted and shown");
    stats.bQuiet = XTRUE;
    xloge("Counted but not shown");
    stats.bQuiet = XFALSE;

    xlog_callback(NULL, NULL);
    xlogi("The callback saw %d lines, %d of them errors", stats.nLines, stats.nErrors);

//////////////////////////////////////////////////////////////////////////////////////////////
// Log into a file: <path>/<name>.log, a new file every day
//////////////////////////////////////////////////////////////////////////////////////////////

    /* Colors are escape codes, a file is better without them */
    xlog_coloring(XLOG_COLORING_DISABLE);
    xlog_screen(XFALSE);
    xlog_path(".");
    xlog_file(XTRUE);

    xlogi("Written to the file");
    xlogw("Written to the file too");

    /* Turning the file off closes it */
    xlog_file(XFALSE);
    xlog_screen(XTRUE);
    xlog_coloring(XLOG_COLORING_TAG);

    const char *pLogPath = "./" XLOG_EXAMPLE_NAME ".log";
    char *pContent = (char*)XPath_Load(pLogPath, NULL);

    xlogi("The file %s has:\n%s", pLogPath, pContent != NULL ? pContent : "nothing");
    free(pContent);
    xunlink(pLogPath);

    xlog_destroy();
    return 0;
}
