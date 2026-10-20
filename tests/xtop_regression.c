/* libxutils: the xtop tool, its screen lines and its REST API. The tool is compiled in with its main renamed, so
 * the cases call its functions as they are, and the injecting allocator fails each allocation the REST round trip
 * makes. */
#include "alloc_inject.h"
#include <fcntl.h>
#include <unistd.h>

#define main xtop_main
#include "../tools/xtop.c"
#undef main

/* ---------------- fixtures ---------------- */

/* The window takes its size from the terminal, and from COLUMNS and LINES when there is none: standard output
   goes to /dev/null while a case renders, so the size is the one the case asks for wherever it runs. */
static int g_nStdout = -1;

static void screen_begin(size_t nColumns)
{
    char sColumns[16];
    snprintf(sColumns, sizeof(sColumns), "%zu", nColumns);
    setenv("COLUMNS", sColumns, 1);
    setenv("LINES", "200", 1);

    fflush(stdout);
    g_nStdout = dup(STDOUT_FILENO);
    int nNull = open("/dev/null", O_WRONLY);
    if (nNull >= 0)
    {
        dup2(nNull, STDOUT_FILENO);
        close(nNull);
    }
}

static void screen_end(void)
{
    fflush(stdout);
    if (g_nStdout < 0) return;
    dup2(g_nStdout, STDOUT_FILENO);
    close(g_nStdout);
    g_nStdout = -1;
}

/* The text of a line without its color and format sequences. A window line is as long as its entry says, and
   not always terminated: an empty one is its spaces alone. */
static void plain_text_n(const char *pLine, size_t nLength, char *pPlain, size_t nSize)
{
    const char *pEnd = pLine + nLength;
    size_t nUsed = 0;

    while (pLine < pEnd && *pLine && nUsed + 1 < nSize)
    {
        if (*pLine == '\033')
        {
            while (pLine < pEnd && *pLine && *pLine != 'm') pLine++;
            if (pLine < pEnd && *pLine) pLine++;
            continue;
        }

        pPlain[nUsed++] = *pLine++;
    }

    pPlain[nUsed] = XSTR_NUL;
}

static void plain_text(const char *pLine, char *pPlain, size_t nSize)
{
    plain_text_n(pLine, strlen(pLine), pPlain, nSize);
}

static int window_has(xcli_win_t *pWin, const char *pText)
{
    for (size_t i = 0; i < pWin->lines.nUsed; i++)
    {
        xarray_data_t *pItem = XArray_Get(&pWin->lines, i);
        if (pItem == NULL || pItem->pData == NULL) continue;

        char sPlain[8192];
        plain_text_n((const char*)pItem->pData, pItem->nSize, sPlain, sizeof(sPlain));
        if (strstr(sPlain, pText) != NULL) return 1;
    }

    return 0;
}

static xarray_data_t array_item(void *pData)
{
    xarray_data_t item;
    memset(&item, 0, sizeof(item));
    item.pData = pData;
    return item;
}

static xnet_iface_t *iface_new(const char *pName, const char *pIPAddr)
{
    xnet_iface_t *pIface = (xnet_iface_t*)calloc(1, sizeof(xnet_iface_t));
    if (pIface == NULL) return NULL;

    xstrncpy(pIface->sName, sizeof(pIface->sName), pName);
    xstrncpy(pIface->sIPAddr, sizeof(pIface->sIPAddr), pIPAddr);
    xstrncpy(pIface->sHWAddr, sizeof(pIface->sHWAddr), "aa:bb:cc:dd:ee:ff");
    pIface->bActive = XTRUE;
    return pIface;
}

/* A name with every byte JSON needs escaped, long enough that the old fixed block cut its interface short */
static void weird_name(char *pName, size_t nSize)
{
    xstrncpy(pName, nSize, "q\"uote\\back\x01\x1f" "tab\t");
    size_t nUsed = strlen(pName);
    for (; nUsed + 1 < nSize && nUsed < 200; nUsed++) pName[nUsed] = (char)('a' + nUsed % 26);
    pName[nUsed] = XSTR_NUL;
}

/* Stats as a server would hold them: every field set, counters as wide as they go */
static int stats_fill(xmon_stats_t *pStats)
{
    if (XMon_InitStats(pStats) != XSTDOK) return XSTDERR;

    xmem_info_t *pMem = &pStats->memInfo;
    pMem->nReclaimable = UINT64_MAX - 1;
    pMem->nBuffers = UINT64_MAX - 2;
    pMem->nResidentMemory = UINT64_MAX - 3;
    pMem->nVirtualMemory = UINT64_MAX - 4;
    pMem->nMemoryCached = UINT64_MAX - 5;
    pMem->nMemoryShared = UINT64_MAX - 6;
    pMem->nMemoryAvail = UINT64_MAX - 7;
    pMem->nMemoryTotal = UINT64_MAX - 8;
    pMem->nMemoryFree = UINT64_MAX - 9;
    pMem->nSwapCached = UINT64_MAX - 10;
    pMem->nSwapTotal = UINT64_MAX - 11;
    pMem->nSwapFree = UINT64_MAX - 12;

    xcpu_stats_t *pCPU = &pStats->cpuStats;
    pCPU->nLoadAvg[0] = XFloatToU32(0.5f);
    pCPU->nLoadAvg[1] = XFloatToU32(1.25f);
    pCPU->nLoadAvg[2] = XFloatToU32(2.0f);
    pCPU->usage.nKernelSpaceUsage = XFloatToU32(10.5f);
    pCPU->usage.nUserSpaceUsage = XFloatToU32(30.25f);
    pCPU->sum.nID = -1;
    pCPU->sum.nIdleTime = XFloatToU32(59.25f);

    for (int i = 0; i < 2; i++)
    {
        xcpu_info_t *pCore = (xcpu_info_t*)calloc(1, sizeof(xcpu_info_t));
        if (pCore == NULL) return XSTDERR;

        pCore->nID = i;
        pCore->nActive = 1;
        pCore->nSoftInterrupts = XFloatToU32(0.25f + i);
        pCore->nHardInterrupts = XFloatToU32(0.5f + i);
        pCore->nUserSpaceNiced = XFloatToU32(1.5f + i);
        pCore->nKernelSpace = XFloatToU32(4.75f + i);
        pCore->nUserSpace = XFloatToU32(20.5f + i);
        pCore->nIdleTime = XFloatToU32(70.0f - i);
        pCore->nIOWait = XFloatToU32(2.0f + i);
        pCore->nStealTime = XFloatToU32(0.125f + i);
        pCore->nGuestTime = XFloatToU32(0.0625f + i);
        pCore->nGuestNiced = XFloatToU32(0.03125f + i);
        pCore->nTemperature = 45000 + (uint32_t)i;

        if (XArray_AddData(&pCPU->cores, pCore, 0) < 0)
        {
            free(pCore);
            return XSTDERR;
        }
    }

    pCPU->nCoreCount = 2;

    char sName[XNAME_MAX];
    weird_name(sName, sizeof(sName));

    xnet_iface_t *pIfaces[2] = {
        iface_new(sName, "fe80:0000:0000:0000:1234:5678:9abc:def0"),
        iface_new("eth0", "10.0.0.1")
    };

    for (int i = 0; i < 2; i++)
    {
        xnet_iface_t *pIface = pIfaces[i];
        if (pIface == NULL)
        {
            free(pIfaces[1]);
            return XSTDERR;
        }

        pIface->nType = 1 + i;
        pIface->nBandwidth = INT64_MAX - i;
        pIface->nBytesSent = INT64_MAX - 1 - i;
        pIface->nPacketsSent = INT64_MAX - 2 - i;
        pIface->nBytesReceived = INT64_MAX - 3 - i;
        pIface->nPacketsReceived = INT64_MAX - 4 - i;
        pIface->nBytesSentPerSec = UINT64_MAX - 5 - (uint64_t)i;
        pIface->nPacketsSentPerSec = UINT64_MAX - 6 - (uint64_t)i;
        pIface->nBytesReceivedPerSec = UINT64_MAX - 7 - (uint64_t)i;
        pIface->nPacketsReceivedPerSec = UINT64_MAX - 8 - (uint64_t)i;

        if (XArray_AddData(&pStats->netIfaces, pIface, 0) < 0)
        {
            free(pIface);
            if (!i) free(pIfaces[1]);
            return XSTDERR;
        }
    }

    return XSTDOK;
}

/* A server session that asks for eRequest, as XTOP_HandleRequest() leaves one for XTOP_SendResponse() */
typedef struct {
    xtop_ctx_t ctx;
    xapi_t api;
    xapi_session_t session;
    xmon_request_t eRequest;
} xtop_server_t;

static void server_begin(xtop_server_t *pServer, xmon_stats_t *pStats, xmon_request_t eRequest)
{
    memset(pServer, 0, sizeof(*pServer));
    XTOP_InitContext(&pServer->ctx);
    pServer->ctx.pStats = pStats;
    pServer->api.pUserCtx = &pServer->ctx;
    pServer->session.pApi = &pServer->api;
    pServer->eRequest = eRequest;
    pServer->session.pSessionData = &pServer->eRequest;
}

/* ---------------- the cases ---------------- */

static int XTest_compare_order(void)
{
    xtop_ctx_t ctx;
    XTOP_InitContext(&ctx);

    /* Idle times are floats in their bits: a negative one is a huge integer, which a difference overflows on */
    xcpu_info_t cores[3];
    memset(cores, 0, sizeof(cores));
    cores[0].nIdleTime = XFloatToU32(-1.0f);
    cores[1].nIdleTime = XFloatToU32(100.0f);
    cores[2].nIdleTime = XFloatToU32(5.0f);

    xarray_data_t low = array_item(&cores[0]), high = array_item(&cores[1]), some = array_item(&cores[2]);
    ctx.nSort = XTOP_SORT_BUSY;
    CHECK(XTOP_CompareCPUs(&low, &high, &ctx) < 0 && XTOP_CompareCPUs(&high, &low, &ctx) > 0,
        "The busiest core comes first, whatever its idle time");
    CHECK(XTOP_CompareCPUs(&some, &high, &ctx) < 0 && XTOP_CompareCPUs(&low, &some, &ctx) < 0,
        "And the order holds between any two");
    CHECK(XTOP_CompareCPUs(&some, &some, &ctx) == 0, "A core is its own equal");

    ctx.nSort = XTOP_SORT_FREE;
    CHECK(XTOP_CompareCPUs(&high, &low, &ctx) < 0 && XTOP_CompareCPUs(&low, &high, &ctx) > 0,
        "Sorted for the free ones, the most idle comes first");
    CHECK(XTOP_CompareCPUs(&high, &some, &ctx) < 0, "Before one less idle");

    /* Traffic is 64 bits wide: as an int, three gigabytes a second was less than two bytes */
    xnet_iface_t ifaces[4];
    memset(ifaces, 0, sizeof(ifaces));
    ifaces[0].nBytesReceivedPerSec = 3000000000ULL;
    ifaces[1].nBytesReceivedPerSec = 1;
    ifaces[1].nBytesSentPerSec = 1;
    ifaces[2].nBytesReceivedPerSec = UINT64_MAX;
    ifaces[2].nBytesSentPerSec = UINT64_MAX;
    ifaces[3].nBytesReceivedPerSec = UINT64_MAX;

    xarray_data_t busy = array_item(&ifaces[0]), idle = array_item(&ifaces[1]);
    xarray_data_t full = array_item(&ifaces[2]), most = array_item(&ifaces[3]);

    ctx.nSort = XTOP_SORT_BUSY;
    CHECK(XTOP_CompareIFaces(&busy, &idle, &ctx) < 0 && XTOP_CompareIFaces(&idle, &busy, &ctx) > 0,
        "The busiest interface comes first");
    CHECK(XTOP_CompareIFaces(&full, &busy, &ctx) < 0, "Traffic near the 64-bit limit is the most");
    CHECK(XTOP_CompareIFaces(&full, &most, &ctx) == 0,
        "Traffic past the 64-bit limit counts as the most there is, not as what it wraps round to");

    ctx.nSort = XTOP_SORT_FREE;
    CHECK(XTOP_CompareIFaces(&idle, &busy, &ctx) < 0 && XTOP_CompareIFaces(&busy, &idle, &ctx) > 0,
        "Sorted for the free ones, the least busy comes first");

    xstrncpy(ifaces[0].sName, sizeof(ifaces[0].sName), "eth0");
    xstrncpy(ifaces[1].sName, sizeof(ifaces[1].sName), "averyverylongifacename");
    ctx.nSort = XTOP_SORT_LEN;
    CHECK(XTOP_CompareIFaces(&busy, &idle, &ctx) < 0 && XTOP_CompareIFaces(&idle, &busy, &ctx) > 0 &&
        XTOP_CompareIFaces(&busy, &busy, &ctx) == 0, "Sorted by length, the shorter name comes first");
    ctx.nSort = XTOP_SORT_NAME;
    CHECK(XTOP_CompareIFaces(&idle, &busy, &ctx) < 0, "Sorted by name, in the order of the names");

    XSock_Close(&ctx.sock);
    return 0;
}

static int XTest_column_width(void)
{
    char sLine[XLINE_MAX], sPlain[XLINE_MAX];
    xstrnul(sLine);

    XTOP_AddCPUInfoUnit(sLine, sizeof(sLine), 5.0, XFALSE);
    plain_text(sLine, sPlain, sizeof(sPlain));
    CHECK(!strcmp(sPlain, "    5.00"), "A value is right-aligned in its column of eight");

    XTOP_AddCPUInfoUnit(sLine, sizeof(sLine), 123456789.0, XFALSE);
    XTOP_AddCPUInfoUnit(sLine, sizeof(sLine), 80.0, XTRUE);
    plain_text(sLine, sPlain, sizeof(sPlain));
    CHECK(!strcmp(sPlain, "    5.00 123456789.00   80.00"),
        "One wider than the column is shown after a space, and the next is still in its own column");

    xstrnul(sLine);
    XTOP_AddCPUTemperature(sLine, sizeof(sLine), 45000);
    XTOP_AddCPUTemperature(sLine, sizeof(sLine), 123456789);
    XTOP_AddCPUTemperature(sLine, sizeof(sLine), 85000);
    plain_text(sLine, sPlain, sizeof(sPlain));
    CHECK(!strcmp(sPlain, "    45.0 123456.8    85.0"), "So is a temperature too wide for its column");

    /* The whole line for a core: every value is there, the wide one included */
    xcpu_info_t core;
    memset(&core, 0, sizeof(core));
    core.nID = 3;
    core.nIdleTime = XFloatToU32(12345678.0f);
    core.nTemperature = 50000;

    screen_begin(160);
    xcli_win_t win;
    XCLIWin_Init(&win, XTRUE);
    XSTATUS nStatus = XTOP_AddCPUInfo(&win, &core);
    screen_end();

    CHECK(nStatus == XSTDOK && window_has(&win, "   3 12345678.00    0.00"), "The core's line has the wide value");
    CHECK(window_has(&win, "    0.00    50.0"), "And the temperature at its end");
    XCLIWin_Destroy(&win);
    return 0;
}

static void test_clear_iface(xarray_data_t *pItem)
{
    free(pItem->pData);
}

static int XTest_iface_names(void)
{
    const char *pLongName = "averyverylongifacename0";
    xtop_ctx_t ctx;
    XTOP_InitContext(&ctx);
    xstrncpy(ctx.sName, sizeof(ctx.sName), "averyvery");

    xarray_t ifaces;
    CHECK(XArray_InitPool(&ifaces, 0, 1, 0) != NULL, "Set up the interfaces");
    ifaces.clearCb = test_clear_iface;

    xnet_iface_t *pLong = iface_new(pLongName, "10.0.0.2");
    xnet_iface_t *pShort = iface_new("lo", "127.0.0.1");
    CHECK(pLong != NULL && pShort != NULL, "Allocate the interfaces");
    CHECK(XArray_AddData(&ifaces, pLong, 0) >= 0 && XArray_AddData(&ifaces, pShort, 0) >= 0, "Add them");

    /* A narrow window cuts a long name short on the screen, and only there. The window knows its size from the
       line before, as in the tool, which starts every frame with an empty one. */
    screen_begin(100);
    xcli_win_t win;
    XCLIWin_Init(&win, XTRUE);
    XCLIWin_AddEmptyLine(&win);
    XTOP_AddNetworkInfo(&ctx, &win, &ifaces);
    screen_end();

    CHECK(!strcmp(pLong->sName, pLongName), "The interface keeps its name");
    CHECK(window_has(&win, " averyvery... ") && !window_has(&win, "averyverylong"),
        "The screen shows it cut to twelve columns");
    CHECK(window_has(&win, " lo "), "And a short one whole");
    XCLIWin_Destroy(&win);

    /* Drawn again, it is cut the same way, from the whole name rather than from what the last frame left */
    xstrncpy(ctx.sName, sizeof(ctx.sName), pLongName);
    screen_begin(100);
    XCLIWin_Init(&win, XTRUE);
    XCLIWin_AddEmptyLine(&win);
    XTOP_AddNetworkInfo(&ctx, &win, &ifaces);
    screen_end();

    CHECK(!strcmp(pLong->sName, pLongName) && window_has(&win, " averyvery... "),
        "The tracked interface is found by its whole name, and shown cut");
    XCLIWin_Destroy(&win);

    /* A wide window has room for it */
    screen_begin(160);
    XCLIWin_Init(&win, XTRUE);
    XCLIWin_AddEmptyLine(&win);
    XTOP_AddNetworkInfo(&ctx, &win, &ifaces);
    screen_end();

    CHECK(window_has(&win, pLongName) && !window_has(&win, "..."), "A wide window shows the whole name");
    XCLIWin_Destroy(&win);

    XArray_Destroy(&ifaces);
    XSock_Close(&ctx.sock);
    return 0;
}

static int XTest_process_line(void)
{
    xcli_bar_t bar;
    xmem_info_t mem;
    xcpu_stats_t cpu;

    memset(&mem, 0, sizeof(mem));
    mem.nMemoryTotal = 1024 * 1024;
    mem.nMemoryFree = 512 * 1024;
    mem.nMemoryAvail = 600 * 1024;
    mem.nSwapTotal = 1024;
    mem.nSwapFree = 1024;

    /* The raw kernel time is a count of ticks: as float bits, it is a figure next to nothing */
    memset(&cpu, 0, sizeof(cpu));
    cpu.usage.nUserSpaceUsage = XFloatToU32(33.25f);
    cpu.usage.nKernelSpaceUsage = XFloatToU32(12.5f);
    cpu.usage.nKernelSpace = 987654321;
    cpu.usage.nUserSpace = 123456789;

    screen_begin(160);
    XProgBar_GetDefaults(&bar);
    bar.bInPercent = XTRUE;
    bar.bInSuffix = XTRUE;
    bar.cLoader = '|';

    xcli_win_t win;
    XCLIWin_Init(&win, XTRUE);
    XSTATUS nStatus = XTOP_AddOverallBar(&win, &bar, &mem, &cpu);
    screen_end();

    CHECK(nStatus == XSTDOK && window_has(&win, "US: 33.25, KS: 12.50"),
        "The process line shows the user and kernel space percentages");
    XCLIWin_Destroy(&win);
    return 0;
}

static int XTest_rest_roundtrip(void)
{
    xmon_stats_t src, dst;
    CHECK(stats_fill(&src) == XSTDOK, "Fill the server's stats");

    xtop_server_t server;
    server_begin(&server, &src, XTOP_ALL);

    xstring_t body;
    CHECK(XString_Init(&body, XSTR_MID, XFALSE) >= 0, "Set up the body");
    CHECK(XTOP_AssembleBody(&server.session, &body) == XSTDOK, "The server assembles its response");

    xjson_t json;
    CHECK(XJSON_Parse(&json, NULL, body.pData, body.nLength), "The response is JSON, however long its blocks");
    CHECK(strstr(body.pData, "\\\"uote\\\\back\\u0001\\u001ftab\\u0009") != NULL,
        "With the bytes of a name JSON needs escaped, escaped");

    /* Blocks of the same size the cores take, written over, so a field nothing set does not read as zero */
    for (int i = 0; i < 8; i++)
    {
        void *pBlocks[8];
        for (int j = 0; j < 8; j++)
        {
            pBlocks[j] = malloc(sizeof(xcpu_info_t));
            if (pBlocks[j] != NULL) memset(pBlocks[j], 0xA5, sizeof(xcpu_info_t));
        }
        for (int j = 0; j < 8; j++) free(pBlocks[j]);
    }

    CHECK(XMon_InitStats(&dst) == XSTDOK, "Set up the client's stats");
    CHECK(XTOP_GetJSONStats(&dst, &json) == XSTDOK, "The client takes the stats from it");
    XJSON_Destroy(&json);

    /* Process usage, each under its own name */
    CHECK(XU32ToFloat(dst.cpuStats.usage.nKernelSpaceUsage) == 10.5f, "Kernel space is kernel space");
    CHECK(XU32ToFloat(dst.cpuStats.usage.nUserSpaceUsage) == 30.25f, "User space is user space");
    CHECK(XU32ToFloat(dst.cpuStats.nLoadAvg[0]) == 0.5f && XU32ToFloat(dst.cpuStats.nLoadAvg[1]) == 1.25f &&
        XU32ToFloat(dst.cpuStats.nLoadAvg[2]) == 2.0f, "The load averages");
    CHECK(dst.cpuStats.sum.nID == -1 && dst.cpuStats.sum.nIdleTime == src.cpuStats.sum.nIdleTime, "The sum");

    /* Every core, and nothing in it the response did not carry */
    CHECK(dst.cpuStats.nCoreCount == 2 && dst.cpuStats.cores.nUsed == 2, "Both cores");
    for (size_t i = 0; i < 2; i++)
    {
        xcpu_info_t *pSent = (xcpu_info_t*)XArray_GetData(&src.cpuStats.cores, i);
        xcpu_info_t *pTaken = (xcpu_info_t*)XArray_GetData(&dst.cpuStats.cores, i);
        CHECK(pSent != NULL && pTaken != NULL, "Each core is there");
        CHECK(pTaken->nID == pSent->nID && pTaken->nActive == pSent->nActive &&
            pTaken->nSoftInterrupts == pSent->nSoftInterrupts && pTaken->nHardInterrupts == pSent->nHardInterrupts &&
            pTaken->nUserSpaceNiced == pSent->nUserSpaceNiced && pTaken->nKernelSpace == pSent->nKernelSpace &&
            pTaken->nUserSpace == pSent->nUserSpace && pTaken->nIdleTime == pSent->nIdleTime &&
            pTaken->nIOWait == pSent->nIOWait && pTaken->nStealTime == pSent->nStealTime &&
            pTaken->nGuestTime == pSent->nGuestTime && pTaken->nGuestNiced == pSent->nGuestNiced &&
            pTaken->nTemperature == pSent->nTemperature, "With every figure it was sent");
        CHECK(!pTaken->nSoftInterruptsRaw && !pTaken->nHardInterruptsRaw && !pTaken->nUserSpaceNicedRaw &&
            !pTaken->nKernelSpaceRaw && !pTaken->nUserSpaceRaw && !pTaken->nIdleTimeRaw && !pTaken->nIOWaitRaw &&
            !pTaken->nStealRaw && !pTaken->nGuestRaw && !pTaken->nGuestNicedRaw && !pTaken->nTotalRaw,
            "And the raw counts, which it was not, zero");
    }

    /* Memory, to the last digit of 64 bits */
    CHECK(!memcmp(&dst.memInfo, &src.memInfo, sizeof(xmem_info_t)), "Every memory figure");

    /* Interfaces: the long name with its escapes, and counters as wide as they go */
    CHECK(dst.netIfaces.nUsed == 2, "Both interfaces");
    for (size_t i = 0; i < 2; i++)
    {
        xnet_iface_t *pSent = (xnet_iface_t*)XArray_GetData(&src.netIfaces, i);
        xnet_iface_t *pTaken = (xnet_iface_t*)XArray_GetData(&dst.netIfaces, i);
        CHECK(pSent != NULL && pTaken != NULL, "Each interface is there");
        CHECK(!strcmp(pTaken->sName, pSent->sName) && !strcmp(pTaken->sIPAddr, pSent->sIPAddr) &&
            !strcmp(pTaken->sHWAddr, pSent->sHWAddr), "With its name and addresses as they are");
        CHECK(pTaken->nType == pSent->nType && pTaken->bActive == pSent->bActive &&
            pTaken->nBandwidth == pSent->nBandwidth && pTaken->nBytesSent == pSent->nBytesSent &&
            pTaken->nPacketsSent == pSent->nPacketsSent && pTaken->nBytesReceived == pSent->nBytesReceived &&
            pTaken->nPacketsReceived == pSent->nPacketsReceived &&
            pTaken->nBytesSentPerSec == pSent->nBytesSentPerSec &&
            pTaken->nPacketsSentPerSec == pSent->nPacketsSentPerSec &&
            pTaken->nBytesReceivedPerSec == pSent->nBytesReceivedPerSec &&
            pTaken->nPacketsReceivedPerSec == pSent->nPacketsReceivedPerSec, "And every counter");
    }

    /* Each endpoint on its own is JSON too */
    xmon_request_t eRequests[3] = { XTOP_CPU, XTOP_MEMORY, XTOP_NETWORK };
    const char *pKeys[3] = { "cpu", "memory", "network" };
    for (int i = 0; i < 3; i++)
    {
        server_begin(&server, &src, eRequests[i]);
        XString_Clear(&body);
        CHECK(XString_Init(&body, XSTR_MID, XFALSE) >= 0, "Set up the body");
        CHECK(XTOP_AssembleBody(&server.session, &body) == XSTDOK, "Assemble one endpoint");
        CHECK(XJSON_Parse(&json, NULL, body.pData, body.nLength), "It is JSON");
        CHECK(XJSON_GetObject(json.pRootObj, pKeys[i]) != NULL, "With what it asked for");
        for (int j = 0; j < 3; j++) CHECK(i == j || XJSON_GetObject(json.pRootObj, pKeys[j]) == NULL, "Only that");
        XJSON_Destroy(&json);
        XSock_Close(&server.ctx.sock);
    }

    XString_Clear(&body);
    XMon_DestroyStats(&dst);
    XMon_DestroyStats(&src);
    return 0;
}

/* The smallest response the client takes: a key renamed in it is an object missing */
static const char *g_pMinimal =
    "{\"cpu\":{\"loadAverage\":[{\"interval\":\"1m\",\"value\":1.5},{\"interval\":\"5m\",\"value\":2.5},"
    "{\"interval\":\"15m\",\"value\":3.5}],\"process\":{\"kernelSpace\":1,\"userSpace\":2},"
    "\"usage\":{\"sum\":{\"id\":-1},\"cores\":[{\"id\":0}]}},"
    "\"memory\":{\"memTotal\":5},\"network\":[{\"name\":\"lo\",\"active\":true}]}";

static int parse_stats(const char *pText, xmon_stats_t *pStats)
{
    xjson_t json;
    if (!XJSON_Parse(&json, NULL, pText, strlen(pText))) return 99;
    int nStatus = XTOP_GetJSONStats(pStats, &json);
    XJSON_Destroy(&json);
    return nStatus;
}

static int XTest_missing_objects(void)
{
    xmon_stats_t stats;
    CHECK(XMon_InitStats(&stats) == XSTDOK, "Set up the stats");
    CHECK(parse_stats(g_pMinimal, &stats) == XSTDOK, "The whole response is taken");
    CHECK(XU32ToFloat(stats.cpuStats.nLoadAvg[0]) == 1.5f && XU32ToFloat(stats.cpuStats.nLoadAvg[1]) == 2.5f &&
        XU32ToFloat(stats.cpuStats.nLoadAvg[2]) == 3.5f, "With every load average under its own interval");
    CHECK(stats.memInfo.nMemoryTotal == 5 && stats.netIfaces.nUsed == 1 && stats.cpuStats.cores.nUsed == 1,
        "And every other part");

    const char *pKeys[] = { "cpu", "loadAverage", "process", "usage", "sum", "cores", "memory", "network" };
    for (size_t i = 0; i < sizeof(pKeys) / sizeof(*pKeys); i++)
    {
        char sKey[32], sText[1024];
        snprintf(sKey, sizeof(sKey), "\"%s\"", pKeys[i]);
        const char *pAt = strstr(g_pMinimal, sKey);
        CHECK(pAt != NULL, "The key is in the response");

        /* The key renamed, by a letter added before its closing quote */
        size_t nBefore = (size_t)(pAt - g_pMinimal) + strlen(sKey) - 1;
        snprintf(sText, sizeof(sText), "%.*sX%s", (int)nBefore, g_pMinimal, g_pMinimal + nBefore);

        if (parse_stats(sText, &stats) != XSTDERR)
        {
            fprintf(stderr, "Missing %s\n", pKeys[i]);
            CHECK(0, "A response without one of its objects is refused");
        }
    }

    XMon_DestroyStats(&stats);
    return 0;
}

static int XTest_endpoints(void)
{
    struct { const char *pUri; XSTATUS nStatus; xmon_request_t eRequest; } cases[] = {
        { "/api/all", XSTDOK, XTOP_ALL },
        { "/api/cpu", XSTDOK, XTOP_CPU },
        { "/api/memory", XSTDOK, XTOP_MEMORY },
        { "/api/network", XSTDOK, XTOP_NETWORK },
        { "/api/all/", XSTDOK, XTOP_ALL },
        { "/api/cpu?pretty=1", XSTDOK, XTOP_CPU },
        { "/apix/all", XSTDOK, XTOP_NONE },
        { "/api_v2/network", XSTDOK, XTOP_NONE },
        { "/api/allx", XSTDOK, XTOP_NONE },
        { "/api/cpuinfo", XSTDOK, XTOP_NONE },
        { "/api/memoryleak", XSTDOK, XTOP_NONE },
        { "/api/networks", XSTDOK, XTOP_NONE },
        { "/api/al", XSTDOK, XTOP_NONE },
        { "/api/all/more", XSTDOK, XTOP_NONE },
        { "/api", XSTDOK, XTOP_NONE },
        { "/API/all", XSTDOK, XTOP_NONE },
        { "/all", XSTDOK, XTOP_NONE },
        { "/", XSTDERR, XTOP_NONE },
        { "?api/all", XSTDERR, XTOP_NONE }
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++)
    {
        xmon_request_t eRequest = XTOP_ALL;
        XSTATUS nStatus = XTOP_GetRequestType(cases[i].pUri, &eRequest);
        if (nStatus != cases[i].nStatus || eRequest != cases[i].eRequest)
        {
            fprintf(stderr, "%s: %d %d\n", cases[i].pUri, nStatus, (int)eRequest);
            CHECK(0, "Each path asks for its endpoint, whole, or for none");
        }
    }

    return 0;
}

static int response_stats(const char *pWire, xmon_stats_t *pStats)
{
    xhttp_t http;
    xhttp_status_t eStatus = XHTTP_ParseData(&http, (uint8_t*)pWire, strlen(pWire));
    int nStatus = (eStatus == XHTTP_COMPLETE || eStatus == XHTTP_PARSED) ? XTOP_ParseResponse(&http, pStats) : 99;
    XHTTP_Clear(&http);
    return nStatus;
}

static int XTest_response_body(void)
{
    xmon_stats_t stats;
    CHECK(XMon_InitStats(&stats) == XSTDOK, "Set up the stats");
    size_t nLength = strlen(g_pMinimal);
    char sWire[2048];

    /* Chunked: the body is in two chunks, and no Content-Length says how long it is */
    size_t nHalf = nLength / 2;
    snprintf(sWire, sizeof(sWire), "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n%zx\r\n%.*s\r\n%zx\r\n%s\r\n0\r\n\r\n",
        nHalf, (int)nHalf, g_pMinimal, nLength - nHalf, g_pMinimal + nHalf);
    CHECK(response_stats(sWire, &stats) == XSTDOK && stats.memInfo.nMemoryTotal == 5, "A chunked body is taken");

    /* Up to the close */
    stats.memInfo.nMemoryTotal = 0;
    snprintf(sWire, sizeof(sWire), "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n%s", g_pMinimal);
    CHECK(response_stats(sWire, &stats) == XSTDOK && stats.memInfo.nMemoryTotal == 5, "So is one that ends at the close");

    stats.memInfo.nMemoryTotal = 0;
    snprintf(sWire, sizeof(sWire), "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n\r\n%s", nLength, g_pMinimal);
    CHECK(response_stats(sWire, &stats) == XSTDOK && stats.memInfo.nMemoryTotal == 5, "And one with a length");

    snprintf(sWire, sizeof(sWire), "HTTP/1.1 404 Not Found\r\nContent-Length: %zu\r\n\r\n%s", nLength, g_pMinimal);
    CHECK(response_stats(sWire, &stats) == XSTDERR, "A status other than 200 is refused, whatever its body");
    CHECK(response_stats("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n", &stats) == XSTDERR, "So is an empty body");
    CHECK(response_stats("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n{\"a\":", &stats) == XSTDERR,
        "And one that is not JSON");

    XMon_DestroyStats(&stats);
    return 0;
}

/* ---------------- the allocation sweep ---------------- */

static xmon_stats_t g_sweepStats;

/* The REST round trip, failing one allocation at a time: the server assembles, the client takes, and neither
   leaks whatever fails. A client that says it took the stats has every core and interface the response carried.
   The server's snapshot may carry fewer than it holds: XMon_GetCPUStats() and XMon_GetNetworkStats() leave out
   one they could not copy, and say how many they did. */
static int scn_xtop_rest(void)
{
    xtop_server_t server;
    server_begin(&server, &g_sweepStats, XTOP_ALL);

    xstring_t body;
    if (XString_Init(&body, XSTR_MIN, XFALSE) < 0) return 0;

    int nBroken = 0;
    if (XTOP_AssembleBody(&server.session, &body) == XSTDOK)
    {
        xjson_t json;
        if (XJSON_Parse(&json, NULL, body.pData, body.nLength))
        {
            xmon_stats_t stats;
            if (XMon_InitStats(&stats) == XSTDOK)
            {
                xjson_obj_t *pUsage = XJSON_GetObject(XJSON_GetObject(json.pRootObj, "cpu"), "usage");
                size_t nCores = XJSON_GetArrayLength(XJSON_GetObject(pUsage, "cores"));
                size_t nIfaces = XJSON_GetArrayLength(XJSON_GetObject(json.pRootObj, "network"));

                if (XTOP_GetJSONStats(&stats, &json) == XSTDOK &&
                    (stats.cpuStats.cores.nUsed != nCores || stats.netIfaces.nUsed != nIfaces)) nBroken = 1;
                XMon_DestroyStats(&stats);
            }
        }

        XJSON_Destroy(&json);
    }

    XString_Clear(&body);
    return nBroken;
}

static int XTest_alloc_sweep(void)
{
    CHECK(stats_fill(&g_sweepStats) == XSTDOK, "Fill the server's stats");
    const alloc_case_t sweep = { "xtop-rest", scn_xtop_rest };
    int nFailed = alloc_sweep(&sweep);
    XMon_DestroyStats(&g_sweepStats);
    CHECK(nFailed == 0, "The round trip survives a failure at every allocation it makes");
    return 0;
}

XTEST_MAIN(
    XTEST_CASE(compare_order),
    XTEST_CASE(column_width),
    XTEST_CASE(iface_names),
    XTEST_CASE(process_line),
    XTEST_CASE(rest_roundtrip),
    XTEST_CASE(missing_objects),
    XTEST_CASE(endpoints),
    XTEST_CASE(response_body),
    XTEST_CASE(alloc_sweep)
)
