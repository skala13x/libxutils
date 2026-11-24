/*!
 *  @file libxutils/src/crypt/base64.h
 *
 *  This source is part of the "libxutils" project
 *  2015-2020  Sandro Kalatozishvili (s.kalatoz@gmail.com)
 *
 * @brief Implementation of Base64 encrypt/decrypt functions.
 */

#ifndef __XUTILS_BASE64_H__
#define __XUTILS_BASE64_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "xstd.h"

#define XBASE64_TABLE_SIZE          256

char* XBase64_Encrypt(const uint8_t *pInput, size_t *pLength);
char* XBase64_Decrypt(const uint8_t *pInput, size_t *pLength);

/* Decodes what XBase64_Decrypt takes straight into pOutput, which holds nSize bytes, without allocating: XTRUE with
   the decoded length in pOutLength, XFALSE for input XBase64_Decrypt refuses or that does not fit in nSize */
xbool_t XBase64_DecryptTo(const uint8_t *pInput, size_t nLength, uint8_t *pOutput, size_t nSize, size_t *pOutLength);

char *XBase64_UrlEncrypt(const uint8_t *pInput, size_t *pLength);
char *XBase64_UrlDecrypt(const uint8_t *pInput, size_t *pLength);

#ifdef __cplusplus
}
#endif

#endif /* __XUTILS_BASE64_H__ */
