/*!
 *  @file libxutils/examples/files.c
 *
 *  This source is part of the "libxutils" project
 *  2015-2026  Sandro Kalatozishvili (s.kalatoz@gmail.com)
 *
 * @brief Example of working with files and directories. A file is copied
 * and the copy is looked at: its size and permissions, the number of its
 * lines, one line of it, its whole content in memory and the directory it
 * is in. Without arguments the example writes a file of its own to copy,
 * in a working directory that it removes at the end.
 *
 * Usage: files [source destination]
 */

#include "xstd.h"
#include "log.h"
#include "str.h"
#include "xfs.h"

#define XFILES_WORKDIR      "files-example"

/* XFile_Print() formats into the file like fprintf() */
static XSTATUS write_sample(const char *pPath)
{
    xfile_t file;
    if (XFile_Open(&file, pPath, "cwt", NULL) < 0) return XSTDERR;

    const char *pDays[] = { "Monday", "Tuesday", "Wednesday", "Thursday", "Friday" };
    XSTATUS nStatus = XSTDOK;
    int i;

    for (i = 0; i < 5 && nStatus == XSTDOK; i++)
        if (XFile_Print(&file, "%d. %s: %d tasks\n", i + 1, pDays[i], (i * 3) % 5 + 1) <= 0) nStatus = XSTDERR;

    XFile_Close(&file);
    return nStatus;
}

static void list_directory(const char *pPath)
{
    xdir_t dir;
    if (XDir_Open(&dir, pPath) < 0)
    {
        xloge("Can not open directory: %s (%s)", pPath, XSTRERR);
        return;
    }

    xlog("Directory %s:", pPath);
    const char *pSlash = pPath[strlen(pPath) - 1] == '/' ? XSTR_EMPTY : "/";
    char sName[XNAME_MAX];

    /* XDir_Read() leaves out "." and ".." */
    while (XDir_Read(&dir, sName, sizeof(sName)) > 0)
    {
        char sEntry[XPATH_MAX], sPerm[XPERM_LEN + 1];
        xstrncpyf(sEntry, sizeof(sEntry), "%s%s%s", pPath, pSlash, sName);

        if (XPath_GetPerm(sPerm, sizeof(sPerm), sEntry) < 0) xstrncpy(sPerm, sizeof(sPerm), "?");
        xlog("  %s %8ld  %s", sPerm, XPath_GetSize(sEntry), sName);
    }

    XDir_Close(&dir);
}

int main(int argc, char *argv[])
{
    xlog_defaults();
    xlog_enable(XLOG_INFO);

    char sSource[XPATH_MAX], sTarget[XPATH_MAX];
    xbool_t bOwnFiles = argc < 3;

    if (bOwnFiles)
    {
        /* XDir_Create() makes the missing parents too, 0 means failure */
        if (XDir_Create(XFILES_WORKDIR, 0755) <= 0)
        {
            xloge("Can not create directory: %s (%s)", XFILES_WORKDIR, XSTRERR);
            return XSTDERR;
        }

        xstrncpyf(sSource, sizeof(sSource), "%s/week.txt", XFILES_WORKDIR);
        xstrncpyf(sTarget, sizeof(sTarget), "%s/week.copy", XFILES_WORKDIR);

        if (write_sample(sSource) != XSTDOK)
        {
            xloge("Can not write file: %s (%s)", sSource, XSTRERR);
            XDir_Remove(XFILES_WORKDIR);
            return XSTDERR;
        }
    }
    else
    {
        xstrncpy(sSource, sizeof(sSource), argv[1]);
        xstrncpy(sTarget, sizeof(sTarget), argv[2]);
    }

//////////////////////////////////////////////////////////////////////////////////////////////
// Copy the file, the copy gets the permissions of the source
//////////////////////////////////////////////////////////////////////////////////////////////

    int nCopied = XPath_CopyFile(sSource, sTarget);
    if (nCopied < 0)
    {
        xloge("Can not copy %s to %s (%s)", sSource, sTarget, XSTRERR);
        if (bOwnFiles) XDir_Remove(XFILES_WORKDIR);
        return XSTDERR;
    }

    char sPerm[XPERM_LEN + 1];
    if (XPath_GetPerm(sPerm, sizeof(sPerm), sTarget) < 0) xstrncpy(sPerm, sizeof(sPerm), "?");
    xlog("Copied %d bytes from %s to %s, permissions %s\n", nCopied, sSource, sTarget, sPerm);

//////////////////////////////////////////////////////////////////////////////////////////////
// Count the lines of the copy and read one of them
//////////////////////////////////////////////////////////////////////////////////////////////

    xfile_t file;
    if (XFile_Open(&file, sTarget, NULL, NULL) >= 0)
    {
        int nLines = XFile_GetLineCount(&file);
        char sLine[XLINE_MAX];

        /* The lines are read from where the file is, counting read it to its end */
        XFile_Seek(&file, 0, SEEK_SET);
        int nLength = XFile_ReadLine(&file, sLine, sizeof(sLine), 2);

        if (nLength > 0 && sLine[nLength - 1] == '\n') sLine[nLength - 1] = XSTR_NUL;
        xlog("%d lines, line 2: %s\n", nLines, nLength > 0 ? sLine : "<none>");
        XFile_Close(&file);
    }

//////////////////////////////////////////////////////////////////////////////////////////////
// Load both files into memory and compare them
//////////////////////////////////////////////////////////////////////////////////////////////

    size_t nSourceSize = 0, nTargetSize = 0;
    uint8_t *pSource = XPath_Load(sSource, &nSourceSize);
    uint8_t *pTarget = XPath_Load(sTarget, &nTargetSize);

    /* The loaded data is NUL terminated, so a text can be used as a string */
    xbool_t bSame = pSource != NULL && pTarget != NULL &&
        nSourceSize == nTargetSize && !memcmp(pSource, pTarget, nSourceSize);

    xlog("Loaded %zu bytes, the copy is %s\n", nTargetSize, bSame ? "identical" : "different");
    free(pSource);
    free(pTarget);

//////////////////////////////////////////////////////////////////////////////////////////////
// List the directory of the copy
//////////////////////////////////////////////////////////////////////////////////////////////

    xpath_t path;
    XPath_Parse(&path, sTarget, XFALSE);
    list_directory(xstrused(path.sPath) ? path.sPath : ".");

    /* XDir_Remove() removes the directory with everything in it */
    if (bOwnFiles && XDir_Remove(XFILES_WORKDIR) < 0)
    {
        xloge("Can not remove directory: %s (%s)", XFILES_WORKDIR, XSTRERR);
        return XSTDERR;
    }

    return bSame ? 0 : XSTDERR;
}
