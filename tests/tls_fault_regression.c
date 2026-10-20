/* TLS setup failures release their objects and transport descriptors. */
#include "test.h"
#include "sock.h"
#include "thread.h"
#include "tls_fixture.h"
#include <fcntl.h>

enum { TLS_FAULT_NONE, TLS_FAULT_METHOD, TLS_FAULT_CONTEXT, TLS_FAULT_SESSION,
    TLS_FAULT_SNI, TLS_FAULT_HOST, TLS_FAULT_IP, TLS_FAULT_CERT, TLS_FAULT_KEY, TLS_FAULT_CHAIN };

static int g_nFault;
static int g_nHits;
static int g_nContexts;
static int g_nSessions;

static xbool_t tls_fault(int nFault)
{
    if (g_nFault != nFault || g_nHits) return XFALSE;
    g_nHits++;
    return XTRUE;
}

const SSL_METHOD *__real_TLS_client_method(void);
const SSL_METHOD *__real_TLS_server_method(void);
SSL_CTX *__real_SSL_CTX_new(const SSL_METHOD*);
SSL *__real_SSL_new(SSL_CTX*);
void __real_SSL_CTX_free(SSL_CTX*);
void __real_SSL_free(SSL*);
long __real_SSL_ctrl(SSL*, int, long, void*);
int __real_X509_VERIFY_PARAM_set1_host(X509_VERIFY_PARAM*, const char*, size_t);
int __real_X509_VERIFY_PARAM_set1_ip_asc(X509_VERIFY_PARAM*, const char*);
int __real_SSL_CTX_use_certificate(SSL_CTX*, X509*);
int __real_SSL_CTX_use_PrivateKey(SSL_CTX*, EVP_PKEY*);
int __real_SSL_CTX_use_certificate_chain_file(SSL_CTX*, const char*);
int __real_OPENSSL_init_ssl(uint64_t, const OPENSSL_INIT_SETTINGS*);

/* A slow setup, so that every thread that asks for it meanwhile is there to wait for it, and a cleanup that only
   counts, so that the process keeps an OpenSSL it can use */
static xatomic_t g_nSetups;
static xatomic_t g_nSetupsDone;
static xatomic_t g_nCleanups;

int __wrap_OPENSSL_init_ssl(uint64_t nOptions, const OPENSSL_INIT_SETTINGS *pSettings)
{
    XSYNC_ATOMIC_ADD(&g_nSetups, 1);
    xusleep(20000);
    int nStatus = __real_OPENSSL_init_ssl(nOptions, pSettings);
    XSYNC_ATOMIC_ADD(&g_nSetupsDone, 1);
    return nStatus;
}

void __wrap_OPENSSL_cleanup(void)
{
    XSYNC_ATOMIC_ADD(&g_nCleanups, 1);
    xusleep(20000);
}

const SSL_METHOD *__wrap_TLS_client_method(void)
{
    return tls_fault(TLS_FAULT_METHOD) ? NULL : __real_TLS_client_method();
}

const SSL_METHOD *__wrap_TLS_server_method(void)
{
    return tls_fault(TLS_FAULT_METHOD) ? NULL : __real_TLS_server_method();
}

SSL_CTX *__wrap_SSL_CTX_new(const SSL_METHOD *pMethod)
{
    if (tls_fault(TLS_FAULT_CONTEXT)) return NULL;
    SSL_CTX *pContext = __real_SSL_CTX_new(pMethod);
    if (pContext) g_nContexts++;
    return pContext;
}

SSL *__wrap_SSL_new(SSL_CTX *pContext)
{
    if (tls_fault(TLS_FAULT_SESSION)) return NULL;
    SSL *pSSL = __real_SSL_new(pContext);
    if (pSSL) g_nSessions++;
    return pSSL;
}

void __wrap_SSL_CTX_free(SSL_CTX *pContext)
{
    if (pContext) g_nContexts--;
    __real_SSL_CTX_free(pContext);
}

void __wrap_SSL_free(SSL *pSSL)
{
    if (pSSL) g_nSessions--;
    __real_SSL_free(pSSL);
}

long __wrap_SSL_ctrl(SSL *pSSL, int nCommand, long nValue, void *pValue)
{
    if (nCommand == SSL_CTRL_SET_TLSEXT_HOSTNAME && tls_fault(TLS_FAULT_SNI)) return 0;
    return __real_SSL_ctrl(pSSL, nCommand, nValue, pValue);
}

int __wrap_X509_VERIFY_PARAM_set1_host(X509_VERIFY_PARAM *pParam, const char *pName, size_t nLength)
{
    return tls_fault(TLS_FAULT_HOST) ? 0 : __real_X509_VERIFY_PARAM_set1_host(pParam, pName, nLength);
}

int __wrap_X509_VERIFY_PARAM_set1_ip_asc(X509_VERIFY_PARAM *pParam, const char *pAddr)
{
    return tls_fault(TLS_FAULT_IP) ? 0 : __real_X509_VERIFY_PARAM_set1_ip_asc(pParam, pAddr);
}

int __wrap_SSL_CTX_use_certificate(SSL_CTX *pContext, X509 *pCert)
{
    return tls_fault(TLS_FAULT_CERT) ? 0 : __real_SSL_CTX_use_certificate(pContext, pCert);
}

int __wrap_SSL_CTX_use_PrivateKey(SSL_CTX *pContext, EVP_PKEY *pKey)
{
    return tls_fault(TLS_FAULT_KEY) ? 0 : __real_SSL_CTX_use_PrivateKey(pContext, pKey);
}

int __wrap_SSL_CTX_use_certificate_chain_file(SSL_CTX *pContext, const char *pPath)
{
    return tls_fault(TLS_FAULT_CHAIN) ? 0 : __real_SSL_CTX_use_certificate_chain_file(pContext, pPath);
}

static int tls_pair(xsock_t *pSock, int *pRemote)
{
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) return XSTDERR;
    *pRemote = pair[1];
    if (XSock_Init(pSock, XSOCK_TCP_PEER, pair[0]) < 0)
    {
        close(pair[0]);
        close(pair[1]);
        return XSTDERR;
    }
    return XSock_NonBlock(pSock, XTRUE) >= 0 ? XSTDOK : XSTDERR;
}

static int tls_failure(xsock_t *pSock, XSOCKET nFD, XSOCKET nResult, xsock_status_t eExpected)
{
    xbool_t bClosed = fcntl(nFD, F_GETFD) < 0 && errno == EBADF;
    xbool_t bEmpty = !pSock->pPrivate && !XSock_GetSSL(pSock) && !XSock_GetSSLCTX(pSock);
    xsock_status_t eStatus = pSock->eStatus;
    XSock_Close(pSock);
    CHECK(nResult == XSOCK_INVALID && g_nHits == 1 && eStatus == eExpected, "The specific TLS setup failure is reported");
    CHECK(bClosed && bEmpty && !g_nContexts && !g_nSessions, "All TLS objects and the failed transport are released");
    return 0;
}

static int XTest_context(void)
{
    for (int nServer = 0; nServer < 2; nServer++)
        for (int nFault = TLS_FAULT_METHOD; nFault <= (nServer ? TLS_FAULT_CONTEXT : TLS_FAULT_SESSION); nFault++)
        {
            xsock_t sock;
            int nRemote;
            CHECK(tls_pair(&sock, &nRemote) > 0, "Create a nonblocking transport");
            XSOCKET nFD = sock.nFD;
            g_nFault = nFault;
            g_nHits = 0;
            XSOCKET nResult = nServer ? XSock_InitSSLServer(&sock, 0) : XSock_InitSSLClient(&sock, "localhost");
            xsock_status_t eExpected = nFault == TLS_FAULT_METHOD ? XSOCK_ERR_SSLMET :
                nFault == TLS_FAULT_CONTEXT ? XSOCK_ERR_SSLCTX : XSOCK_ERR_SSLNEW;
            close(nRemote);
            CHECK(tls_failure(&sock, nFD, nResult, eExpected) == 0, "Both TLS roles recover their resources on setup failure");
            g_nFault = TLS_FAULT_NONE;
        }
    return 0;
}

static int XTest_identity(void)
{
    for (int nAfterInit = 0; nAfterInit < 2; nAfterInit++)
        for (int nFault = TLS_FAULT_SNI; nFault <= TLS_FAULT_IP; nFault++)
        {
            xsock_t sock;
            int nRemote;
            CHECK(tls_pair(&sock, &nRemote) > 0, "Create a transport for identity verification");
            XSOCKET nFD = sock.nFD;
            const char *pHost = nFault == TLS_FAULT_IP ? "127.0.0.1" : "localhost";
            if (nAfterInit) CHECK(XSock_InitSSLClient(&sock, "localhost") >= 0, "Start the normal TLS client");
            g_nFault = nFault;
            g_nHits = 0;
            XSOCKET nResult;
            if (nAfterInit)
            {
                xsock_cert_t cert;
                XSock_InitCert(&cert);
                cert.pHostName = pHost;
                cert.nVerifyFlags = SSL_VERIFY_PEER;
                nResult = XSock_SetSSLCert(&sock, &cert);
            }
            else nResult = XSock_InitSSLClient(&sock, pHost);
            close(nRemote);
            CHECK(tls_failure(&sock, nFD, nResult, XSOCK_ERR_SSLCNT) == 0, "Failed identity setup leaves no unverified client");
            g_nFault = TLS_FAULT_NONE;
        }
    return 0;
}

static int XTest_certificates(void)
{
    tls_fixture_t identity = {0};
    int nCreated = tls_fixture_begin(&identity);
    if (nCreated != XSTDOK) { tls_fixture_end(&identity); return 1; }
    int nStatus = 0;
    for (int nFault = TLS_FAULT_CERT; nFault <= TLS_FAULT_CHAIN; nFault++)
    {
        xsock_t sock;
        int nRemote;
        CHECK(tls_pair(&sock, &nRemote) > 0, "Create the certificate fixture");
        XSOCKET nFD = sock.nFD;
        CHECK(XSock_InitSSLServer(&sock, 0) >= 0, "Create the server context");
        xsock_cert_t cert;
        XSock_InitCert(&cert);
        if (nFault == TLS_FAULT_CHAIN)
        {
            cert.pCertPath = identity.sCert;
            cert.pKeyPath = identity.sKey;
            cert.pCaPath = identity.sCert;
        }
        else
        {
            cert.p12Path = identity.sP12;
            cert.p12Pass = "regression";
        }
        g_nFault = nFault;
        g_nHits = 0;
        XSOCKET nResult = XSock_SetSSLCert(&sock, &cert);
        xsock_status_t eExpected = nFault == TLS_FAULT_CERT ? XSOCK_ERR_SSLCRT :
            nFault == TLS_FAULT_KEY ? XSOCK_ERR_SSLKEY : XSOCK_ERR_SSLCA;
        close(nRemote);
        nStatus = tls_failure(&sock, nFD, nResult, eExpected);
        g_nFault = TLS_FAULT_NONE;
        if (nStatus) break;
    }
    tls_fixture_end(&identity);
    return nStatus;
}

static int XTest_accept(void)
{
    xsock_t listener, peer;
    int nListen = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(nListen >= 0 && XSock_Init(&listener, XSOCK_TCP_SERVER, nListen) == XSOCK_SUCCESS, "Create the TLS listener");
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t nLength = sizeof(addr);
    CHECK(bind(nListen, (struct sockaddr*)&addr, sizeof(addr)) == 0 && listen(nListen, 4) == 0 &&
        getsockname(nListen, (struct sockaddr*)&addr, &nLength) == 0, "Bind the listener to an ephemeral port");
    CHECK(XSock_NonBlock(&listener, XTRUE) >= 0 && XSock_InitSSLServer(&listener, 0) >= 0, "Install the server TLS context");
    int nClient = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(nClient >= 0 && connect(nClient, (struct sockaddr*)&addr, sizeof(addr)) == 0, "Connect the first client");
    g_nFault = TLS_FAULT_SESSION;
    g_nHits = 0;
    XSOCKET nResult = XSock_Accept(&listener, &peer);
    g_nFault = TLS_FAULT_NONE;
    xbool_t bEmpty = peer.nFD == XSOCK_INVALID && !peer.pPrivate;
    XSock_Close(&peer);
    close(nClient);
    CHECK(nResult == XSOCK_INVALID && g_nHits == 1 && bEmpty, "An accepted peer with no SSL session is completely released");
    CHECK(listener.nFD == nListen && listener.eStatus == XSOCK_ERR_SSLNEW && g_nContexts == 1 && !g_nSessions,
        "Session allocation failure preserves the listener and its certificate context");
    nClient = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(nClient >= 0 && connect(nClient, (struct sockaddr*)&addr, sizeof(addr)) == 0, "A second client can connect");
    CHECK(XSock_Accept(&listener, &peer) >= 0 && XSock_GetSSL(&peer) && peer.eStatus == XSOCK_WANT_READ,
        "The listener accepts a TLS peer after the allocation failure");
    XSock_Close(&listener);
    CHECK(SSL_get_SSL_CTX(XSock_GetSSL(&peer)) != NULL && g_nSessions == 1, "An accepted session retains its shared context");
    XSock_Close(&peer);
    close(nClient);
    CHECK(!g_nContexts && !g_nSessions, "Closing the last peer releases all remaining TLS ownership");
    return 0;
}

typedef struct {
    xatomic_t nGo;
    xatomic_t nEarly;
    xbool_t bDeinit;
} tls_lifecycle_t;

static void *tls_lifecycle_worker(void *pContext)
{
    tls_lifecycle_t *pTest = (tls_lifecycle_t*)pContext;
    while (!XSYNC_ATOMIC_GET(&pTest->nGo)) xyield();

    if (pTest->bDeinit) XSock_DeinitSSL();
    else
    {
        XSock_InitSSL();
        if (XSYNC_ATOMIC_GET(&g_nSetupsDone) != 1) XSYNC_ATOMIC_ADD(&pTest->nEarly, 1);
    }

    return NULL;
}

static int tls_lifecycle_race(xbool_t bDeinit, int *pEarly)
{
    tls_lifecycle_t test;
    memset(&test, 0, sizeof(test));
    test.bDeinit = bDeinit;

    xthread_t workers[8];
    for (int i = 0; i < 8; i++)
        if (XThread_Create(&workers[i], tls_lifecycle_worker, &test, XFALSE) != XSTDOK) return XSTDERR;

    XSYNC_ATOMIC_SET(&test.nGo, 1);
    for (int i = 0; i < 8; i++) XThread_Join(&workers[i]);
    *pEarly = (int)XSYNC_ATOMIC_GET(&test.nEarly);
    return XSTDOK;
}

static int XTest_lifecycle(void)
{
    /* OpenSSL is set up once however many threads ask at once, and none of them goes on before it is: a flag looked
       at and then set let several set it up together */
    int nEarly = 0;
    CHECK(tls_lifecycle_race(XFALSE, &nEarly) == XSTDOK, "Eight threads set TLS up at once");
    CHECK(XSYNC_ATOMIC_GET(&g_nSetups) == 1, "OpenSSL is set up once");
    CHECK(!nEarly, "And no thread went on before it was");
    XSock_InitSSL();
    CHECK(XSYNC_ATOMIC_GET(&g_nSetups) == 1, "Asking again does nothing");

    /* Torn down once too, and for good: OpenSSL cannot be set up again after its cleanup, and taking a setup that
       failed for one that worked was what came next */
    CHECK(tls_lifecycle_race(XTRUE, &nEarly) == XSTDOK, "Eight threads tear TLS down at once");
    CHECK(XSYNC_ATOMIC_GET(&g_nCleanups) == 1, "OpenSSL is cleaned up once");
    XSock_InitSSL();
    XSock_DeinitSSL();
    CHECK(XSYNC_ATOMIC_GET(&g_nSetups) == 1 && XSYNC_ATOMIC_GET(&g_nCleanups) == 1, "And is not set up again after");
    return 0;
}

XTEST_MAIN(
    XTEST_CASE(context),
    XTEST_CASE(identity),
    XTEST_CASE(certificates),
    XTEST_CASE(accept),
    XTEST_CASE(lifecycle)
)
