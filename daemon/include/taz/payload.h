#ifndef TAZ_PAYLOAD_H
#define TAZ_PAYLOAD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /* Encode a CAPABILITY frame payload (protocol 1.0, VERSION advertised).
     * Returns bytes written; 0 on encode failure. */
    size_t taz_payload_capability(uint8_t *buf, size_t bufsize);

    /* Encode a VersionResponse payload using build-time version strings.
     * Returns bytes written; 0 on encode failure. */
    size_t taz_payload_version_response(uint8_t *buf, size_t bufsize);

#ifdef __cplusplus
}
#endif

#endif /* TAZ_PAYLOAD_H */
