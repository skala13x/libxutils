/* libxutils tests: a TLS session over a socket pair, both ends in one thread.
 *
 * OpenSSL itself serves on one end and the library's TLS client is the other, trusting only a certificate made
 * for the case. Nothing waits on a port or on another thread: each step writes what the peer would have sent by
 * then, so a case can take the session to exactly the state it needs. */
#ifndef XUTILS_TEST_TLS_PAIR_H
#define XUTILS_TEST_TLS_PAIR_H

#include "sock.h"
#include "xtime.h"

#ifdef XSOCK_USE_SSL
#include <openssl/x509v3.h>

/* A TLS session over a socket pair: OpenSSL itself on one end as the server, the library's TLS client on the
   other, trusting only a certificate made for the case. */
typedef struct {
    SSL_CTX *pServerCtx;
    SSL *pServer;
    EVP_PKEY *pKey;
    X509 *pCertificate;
    xsock_t remote;
    xsock_t client;
} tls_pair_t;

static inline void tls_pair_close(tls_pair_t *pTls)
{
    XSock_Close(&pTls->client);
    if (pTls->pServer != NULL) SSL_free(pTls->pServer);
    if (pTls->pServerCtx != NULL) SSL_CTX_free(pTls->pServerCtx);
    if (pTls->pCertificate != NULL) X509_free(pTls->pCertificate);
    if (pTls->pKey != NULL) EVP_PKEY_free(pTls->pKey);
    XSock_Close(&pTls->remote);
    ERR_clear_error();
}

static inline int tls_pair_open(tls_pair_t *pTls)
{
    memset(pTls, 0, sizeof(*pTls));
    pTls->remote.nFD = pTls->client.nFD = XSOCK_INVALID;
    XSock_InitSSL();

    EVP_PKEY_CTX *pKeyCtx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, NULL);
    if (pKeyCtx == NULL || EVP_PKEY_keygen_init(pKeyCtx) != 1 ||
        EVP_PKEY_CTX_set_ec_paramgen_curve_nid(pKeyCtx, NID_X9_62_prime256v1) != 1 ||
        EVP_PKEY_keygen(pKeyCtx, &pTls->pKey) != 1)
    {
        EVP_PKEY_CTX_free(pKeyCtx);
        return XSTDERR;
    }

    EVP_PKEY_CTX_free(pKeyCtx);
    pTls->pCertificate = X509_new();
    if (pTls->pCertificate == NULL) return XSTDERR;

    X509_set_version(pTls->pCertificate, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(pTls->pCertificate), 1);
    X509_gmtime_adj(X509_getm_notBefore(pTls->pCertificate), -60);
    X509_gmtime_adj(X509_getm_notAfter(pTls->pCertificate), 3600);
    X509_set_pubkey(pTls->pCertificate, pTls->pKey);
    X509_NAME *pName = X509_get_subject_name(pTls->pCertificate);
    X509_NAME_add_entry_by_txt(pName, "CN", MBSTRING_ASC, (const unsigned char*)"localhost", -1, -1, 0);
    X509_set_issuer_name(pTls->pCertificate, pName);
    if (X509_sign(pTls->pCertificate, pTls->pKey, EVP_sha256()) <= 0) return XSTDERR;

    pTls->pServerCtx = SSL_CTX_new(TLS_server_method());
    if (pTls->pServerCtx == NULL ||
        SSL_CTX_use_certificate(pTls->pServerCtx, pTls->pCertificate) != 1 ||
        SSL_CTX_use_PrivateKey(pTls->pServerCtx, pTls->pKey) != 1) return XSTDERR;

    XSOCKET pair[2];
    if (XSock_CreatePair(pair) != XSTDOK) return XSTDERR;
    XSock_Init(&pTls->remote, XSOCK_TCP_PEER, pair[0]);
    if (XSock_NonBlock(&pTls->remote, XTRUE) == XSOCK_INVALID) return XSTDERR;

    pTls->pServer = SSL_new(pTls->pServerCtx);
    if (pTls->pServer == NULL || SSL_set_fd(pTls->pServer, (int)pair[0]) != 1) return XSTDERR;
    SSL_set_accept_state(pTls->pServer);

    xsock_t *pClient = &pTls->client;
    if (XSock_Init(pClient, XSOCK_TCP_CLIENT | XSOCK_SSL | XSOCK_NB, pair[1]) == XSOCK_ERROR ||
        XSock_NonBlock(pClient, XTRUE) == XSOCK_INVALID ||
        XSock_InitSSLClient(pClient, "localhost") == XSOCK_INVALID ||
        X509_STORE_add_cert(SSL_CTX_get_cert_store(XSock_GetSSLCTX(pClient)), pTls->pCertificate) != 1)
        return XSTDERR;

    uint64_t nDeadline = XTime_GetMs() + 5000;
    while (XTime_GetMs() < nDeadline)
    {
        int nServer = SSL_accept(pTls->pServer);
        if (nServer != 1)
        {
            int nError = SSL_get_error(pTls->pServer, nServer);
            if (nError != SSL_ERROR_WANT_READ && nError != SSL_ERROR_WANT_WRITE) return XSTDERR;
        }

        if (XSock_SSLConnect(pClient) == XSOCK_INVALID) return XSTDERR;
        if (SSL_is_init_finished(pTls->pServer) && SSL_is_init_finished(XSock_GetSSL(pClient)))
        {
            if (XSock_NonBlock(pClient, XFALSE) == XSOCK_INVALID) return XSTDERR;
            XSock_TimeOutR(pClient, 2, 0);
            return XSTDOK;
        }
    }

    return XSTDERR;
}

/* What the server sends, and how it ends the stream after it: with its close_notify, with no more than the end of
   the transport under it, or not yet */
typedef enum {
    TLS_PAIR_OPEN = 0,
    TLS_PAIR_NOTIFY,
    TLS_PAIR_CUT
} tls_pair_end_t;

static inline int tls_pair_answer(tls_pair_t *pTls, const char *pAnswer, tls_pair_end_t eEnd)
{
    int nLength = (int)strlen(pAnswer);
    if (SSL_write(pTls->pServer, pAnswer, nLength) != nLength) return XSTDERR;
    if (eEnd == TLS_PAIR_OPEN) return XSTDOK;
    if (eEnd == TLS_PAIR_NOTIFY && SSL_shutdown(pTls->pServer) < 0) return XSTDERR;

    XSock_Close(&pTls->remote);
    return XSTDOK;
}

static inline int tls_pair_skip(tls_pair_t *pTls)
{
    tls_pair_close(pTls);
    printf("The TLS fixture could not be built, skipping\n");
    return 77;
}

#endif /* XSOCK_USE_SSL */
#endif /* XUTILS_TEST_TLS_PAIR_H */
