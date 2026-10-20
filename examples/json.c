/*!
 *  @file libxutils/examples/json.c
 *
 *  This source is part of the "libxutils" project
 *  2015-2026  Sandro Kalatozishvili (s.kalatoz@gmail.com)
 *
 * @brief Example of working with JSON. A service configuration is parsed
 * and read with defaults for what it leaves out, a broken document is
 * reported with the position of the error, the configuration is changed
 * and printed, a status report is built in code and a flat message is
 * read without building a tree at all. The file itself is not changed.
 *
 * Usage: json <config.json>
 */

#include "xstd.h"
#include "json.h"
#include "pool.h"
#include "log.h"
#include "str.h"
#include "xfs.h"

#define XJSON_POOL_SIZE     (1024 * 64)

/* The getters return 0, false or "" for a member that is missing or has another
   type, so a value with a default of its own is read after checking the type. */
static int config_int(xjson_obj_t *pObj, const char *pName, int nDefault)
{
    xjson_obj_t *pValue = XJSON_GetObject(pObj, pName);
    return pValue != NULL && pValue->nType == XJSON_TYPE_NUMBER ? XJSON_GetInt(pValue) : nDefault;
}

/* A number written with a point is a FLOAT, without it a NUMBER, XJSON_GetFloat() reads only a FLOAT */
static double config_float(xjson_obj_t *pObj, const char *pName, double fDefault)
{
    xjson_obj_t *pValue = XJSON_GetObject(pObj, pName);
    if (pValue == NULL) return fDefault;

    if (pValue->nType == XJSON_TYPE_FLOAT) return XJSON_GetFloat(pValue);
    if (pValue->nType == XJSON_TYPE_NUMBER) return XJSON_GetInt(pValue);
    return fDefault;
}

static const char* config_string(xjson_obj_t *pObj, const char *pName, const char *pDefault)
{
    xjson_obj_t *pValue = XJSON_GetObject(pObj, pName);
    return pValue != NULL && pValue->nType == XJSON_TYPE_STRING ? XJSON_GetString(pValue) : pDefault;
}

static void print_routes(xjson_obj_t *pRoutes)
{
    size_t i, nCount = XJSON_GetArrayLength(pRoutes);
    xlog("Routes: %zu", nCount);

    for (i = 0; i < nCount; i++)
    {
        xjson_obj_t *pRoute = XJSON_GetArrayItem(pRoutes, i);
        xjson_obj_t *pMethods = XJSON_GetObject(pRoute, "methods");
        size_t j, nMethods = XJSON_GetArrayLength(pMethods);
        char sMethods[XSTR_TINY] = XSTR_INIT;

        for (j = 0; j < nMethods; j++)
        {
            const char *pMethod = XJSON_GetString(XJSON_GetArrayItem(pMethods, j));
            xstrncat(sMethods, sizeof(sMethods), "%s%s", j ? "," : XSTR_EMPTY, pMethod);
        }

        xbool_t bAuth = XJSON_GetBool(XJSON_GetObject(pRoute, "auth"));
        xlog("  %-12s %-16s auth: %-3s weight: %.2f", config_string(pRoute, "path", "/"),
            nMethods ? sMethods : "any method", bAuth ? "yes" : "no", config_float(pRoute, "weight", 1.0));
    }
}

static void print_error(xjson_t *pJson, const char *pSource)
{
    char sError[XSTR_MIN];
    XJSON_GetErrorStr(pJson, sError, sizeof(sError));
    xloge("Failed to parse %s: %s", pSource, sError);
}

/* Members of an object are kept in a hash map, a dump does not keep their order */
static void print_dump(xjson_obj_t *pObj, size_t nTabSize, const char *pTitle)
{
    size_t nLength = 0;
    char *pDump = XJSON_DumpObj(pObj, nTabSize, &nLength);

    if (pDump == NULL)
    {
        xloge("Failed to dump: %s", pTitle);
        return;
    }

    xlog("%s (%zu bytes):%s%s\n", pTitle, nLength, nTabSize ? "\n" : " ", pDump);
    xfree(pObj->pPool, pDump);
}

int main(int argc, char *argv[])
{
    xlog_defaults();

    if (argc < 2)
    {
        xlog("Usage: %s <config.json>", argv[0]);
        return XSTDERR;
    }

    /* Parsing makes many small allocations, a pool serves them from large blocks */
    xpool_t pool;
    if (XPool_Init(&pool, XJSON_POOL_SIZE) < 0)
    {
        xloge("Failed to create a memory pool: %s", XSTRERR);
        return XSTDERR;
    }

//////////////////////////////////////////////////////////////////////////////////////////////
// Parse the configuration file
//////////////////////////////////////////////////////////////////////////////////////////////

    size_t nSize = 0;
    char *pText = (char*)XPath_Load(argv[1], &nSize);

    if (pText == NULL)
    {
        xloge("Can not read file: %s (%s)", argv[1], XSTRERR);
        XPool_Destroy(&pool);
        return XSTDERR;
    }

    /* The tree gets copies of the names and values, the error message reads the text */
    xjson_t json;
    int nStatus = XJSON_Parse(&json, &pool, pText, nSize);

    if (nStatus != XJSON_SUCCESS) print_error(&json, argv[1]);
    free(pText);

    if (nStatus != XJSON_SUCCESS || json.pRootObj->nType != XJSON_TYPE_OBJECT)
    {
        if (nStatus == XJSON_SUCCESS) xloge("The configuration is not an object: %s", argv[1]);
        XJSON_Destroy(&json);
        XPool_Destroy(&pool);
        return XSTDERR;
    }

//////////////////////////////////////////////////////////////////////////////////////////////
// Read the values, with defaults for those the file leaves out
//////////////////////////////////////////////////////////////////////////////////////////////

    xjson_obj_t *pRoot = json.pRootObj;
    xjson_obj_t *pServer = XJSON_GetObject(pRoot, "server");
    xjson_obj_t *pLimits = XJSON_GetObject(pRoot, "limits");

    xlog("Service %s, version %d, debug %s", config_string(pRoot, "service", "unnamed"),
        config_int(pRoot, "version", 1), XJSON_GetBool(XJSON_GetObject(pRoot, "debug")) ? "on" : "off");

    /* A member set to null is there, it is not the same as a missing one */
    xjson_obj_t *pTls = XJSON_GetObject(pServer, "tls");
    const char *pTlsState = pTls == NULL ? "not configured" : pTls->nType == XJSON_TYPE_NULL ? "off" : "on";

    xlog("Listen on %s:%d, timeout %.1f s, TLS %s", config_string(pServer, "address", "127.0.0.1"),
        config_int(pServer, "port", 80), config_float(pServer, "timeout", 5.0), pTlsState);

    /* There are no workers in the file, the default is used */
    xlog("Workers: %d, connections: %d, rate: %.2f/s, clock skew: %d s", config_int(pServer, "workers", 4),
        config_int(pLimits, "connections", 128), config_float(pLimits, "rate", 0.0), config_int(pLimits, "clockSkew", 0));

    print_routes(XJSON_GetObject(pRoot, "routes"));

    xjson_obj_t *pAdmins = XJSON_GetObject(pRoot, "admins");
    size_t i, nAdmins = XJSON_GetArrayLength(pAdmins);
    xlog_wn("Admins:");

    for (i = 0; i < nAdmins; i++)
        xlog_wn(" %s", XJSON_GetString(XJSON_GetArrayItem(pAdmins, i)));

    xlog("\n");

//////////////////////////////////////////////////////////////////////////////////////////////
// A broken document: the error says what is wrong and where
//////////////////////////////////////////////////////////////////////////////////////////////

    const char *pBroken = "{ \"port\": 8080, \"tls\": null, }";
    xjson_t broken;

    xlog("Parsing: %s", pBroken);
    if (XJSON_Parse(&broken, &pool, pBroken, strlen(pBroken)) != XJSON_SUCCESS) print_error(&broken, "the text");
    else xloge("A trailing comma was accepted");

    XJSON_Destroy(&broken);
    xlog(XSTR_EMPTY);

//////////////////////////////////////////////////////////////////////////////////////////////
// Change the configuration
//////////////////////////////////////////////////////////////////////////////////////////////

    /* A parsed object refuses a member it has already, this call returns it with updates allowed */
    pServer = XJSON_GetOrCreateObject(pRoot, "server", XTRUE);
    if (XJSON_AddInt(pServer, "port", 8443) != XJSON_ERR_NONE) xloge("Failed to change the port");

    /* AddObject() takes the object only when it succeeds, otherwise it is still ours to free */
    pTls = XJSON_NewObject(&pool, "tls", XFALSE);
    if (pTls != NULL)
    {
        XJSON_AddString(pTls, "cert", "/etc/xutils-gateway/cert.pem");
        XJSON_AddString(pTls, "key", "/etc/xutils-gateway/key.pem");
        if (XJSON_AddObject(pServer, pTls) != XJSON_ERR_NONE) XJSON_FreeObject(pTls);
    }

    /* Array items have no names */
    XJSON_AddString(pAdmins, NULL, "carol");
    XJSON_RemoveArrayItem(XJSON_GetObject(pRoot, "routes"), 2);

    /* Created when it is not there, the existing one is returned when it is */
    xjson_obj_t *pMetrics = XJSON_GetOrCreateObject(pRoot, "metrics", XFALSE);
    XJSON_AddBool(pMetrics, "enabled", XTRUE);
    XJSON_AddInt(pMetrics, "interval", 15);

    /* The tab size indents the dump, 0 writes it in one line */
    print_dump(pRoot, 4, "The changed configuration");

//////////////////////////////////////////////////////////////////////////////////////////////
// Build a summary of the configuration in code
//////////////////////////////////////////////////////////////////////////////////////////////

    xjson_obj_t *pSummary = XJSON_NewObject(&pool, NULL, XFALSE);
    if (pSummary == NULL)
    {
        xloge("Failed to create the summary: %s", XSTRERR);
        XJSON_Destroy(&json);
        XPool_Destroy(&pool);
        return XSTDERR;
    }

    /* A strict object is not written if an add has failed, the adds need no checks one by one */
    XJSON_SetStrict(pSummary, XTRUE);
    XJSON_AddString(pSummary, "service", config_string(pRoot, "service", "unnamed"));
    XJSON_AddInt(pSummary, "port", config_int(pServer, "port", 80));
    XJSON_AddBool(pSummary, "tls", XJSON_GetObject(pServer, "tls") != NULL);
    XJSON_AddU32(pSummary, "admins", (uint32_t)XJSON_GetArrayLength(pAdmins));

    xjson_obj_t *pRoutes = XJSON_GetObject(pRoot, "routes");
    xjson_obj_t *pAccess = XJSON_GetOrCreateObject(pSummary, "access", XFALSE);
    size_t nRoutes = XJSON_GetArrayLength(pRoutes);

    for (i = 0; i < nRoutes; i++)
    {
        xjson_obj_t *pRoute = XJSON_GetArrayItem(pRoutes, i);
        xbool_t bAuth = XJSON_GetBool(XJSON_GetObject(pRoute, "auth"));
        XJSON_AddString(pAccess, config_string(pRoute, "path", "/"), bAuth ? "admins" : "public");
    }

    print_dump(pSummary, 0, "Summary");
    XJSON_FreeObject(pSummary);

//////////////////////////////////////////////////////////////////////////////////////////////
// Read a flat message without building a tree
//////////////////////////////////////////////////////////////////////////////////////////////

    const char *pMessage = "{\"id\": 7, \"user\": \"carol\", \"path\": \"/api/users\"}";
    xjson_field_t fields[] = {
        { "user", NULL, 0, XJSON_TYPE_INVALID },
        { "path", NULL, 0, XJSON_TYPE_INVALID },
        { "token", NULL, 0, XJSON_TYPE_INVALID }
    };

    size_t nFields = sizeof(fields) / sizeof(fields[0]);
    xlog("Message: %s", pMessage);

    /* Nothing is allocated, the values point into the message and are not NUL terminated */
    if (XJSON_ScanFlat(pMessage, strlen(pMessage), fields, nFields))
    {
        for (i = 0; i < nFields; i++)
        {
            if (fields[i].nType == XJSON_TYPE_INVALID) xlog("  %s: missing", fields[i].pName);
            else xlog("  %s: %.*s", fields[i].pName, (int)fields[i].nLength, fields[i].pValue);
        }
    }
    else xloge("The message is not a flat JSON object");

    XJSON_Destroy(&json);
    XPool_Destroy(&pool);
    return 0;
}
