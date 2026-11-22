/*!
 *  @file libxutils/src/xver.h
 *
 *  This source is part of the "libxutils" project
 *  2015-2026  Sandro Kalatozishvili (s.kalatoz@gmail.com)
 *
 * @brief Get additional information about library
 */

#ifndef __XUTILS_VER_H__
#define __XUTILS_VER_H__

#define XUTILS_VERSION_MAX      2
#define XUTILS_VERSION_MIN      8
#define XUTILS_BUILD_NUMBER     62
#define XUTILS_RELEASE_DATE     "22Nov2026"

#ifdef __cplusplus
extern "C" {
#endif

const char* XUtils_Version(void);
const char* XUtils_VersionShort(void);

#ifdef __cplusplus
}
#endif

#endif /* __XUTILS_VER_H__ */
