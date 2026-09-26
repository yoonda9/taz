/**
 * @file version.h
 * @brief Protocol version advertised in the CAPABILITY frame.
 */

#ifndef TAZER_VERSION_H
#define TAZER_VERSION_H

#ifdef __cplusplus
extern "C"
{
#endif

/** Protocol major version; clients refuse to speak on a mismatch. */
#define TAZER_PROTOCOL_MAJOR 1U
/** Protocol minor version; differing minors are compatible. */
#define TAZER_PROTOCOL_MINOR 0U

#ifdef __cplusplus
}
#endif

#endif /* TAZER_VERSION_H */
