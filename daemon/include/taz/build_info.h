/**
 * @file build_info.h
 * @brief Build-time version and identifier accessors.
 */

#ifndef TAZ_BUILD_INFO_H
#define TAZ_BUILD_INFO_H

#ifdef __cplusplus
extern "C"
{
#endif

    /** Returns the CMake PROJECT_VERSION string (e.g. "0.1.0"). */
    const char *taz_build_version(void);

    /** Returns a short git hash identifying this build (e.g. "a1b2c3d"). */
    const char *taz_build_id(void);

#ifdef __cplusplus
}
#endif

#endif /* TAZ_BUILD_INFO_H */
