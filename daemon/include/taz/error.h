#ifndef TAZ_ERROR_H
#define TAZ_ERROR_H

#include <stddef.h>
#include <stdint.h>

#include "taz/v1/common.pb.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /* Encode an ErrorInfo payload from (code, message, detail).
     * message and detail may be NULL (encoded as empty strings).
     * Returns bytes written; 0 on encode failure. */
    size_t taz_error_encode(uint8_t *buf, size_t bufsize, taz_v1_ErrorCode code,
                            const char *message, const char *detail);

#ifdef __cplusplus
}
#endif

#endif /* TAZ_ERROR_H */
