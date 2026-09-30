#ifndef TAZ_ERROR_H
#define TAZ_ERROR_H

#include <stddef.h>
#include <stdint.h>

#include "taz/dispatch.h"
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

    /* Map a POSIX errno value to an ErrorCode. */
    taz_v1_ErrorCode taz_error_from_errno(int errnum);

#ifdef _WIN32
    /* Map a Win32 GetLastError() value to an ErrorCode. */
    taz_v1_ErrorCode taz_error_from_win32(unsigned long err);
#endif

    /* Build and send an ERROR frame via write_fn.
     * message and detail may be NULL. */
    void taz_error_send(taz_dispatch_write_fn_t write_fn, void *ctx,
                        uint32_t stream_id, uint16_t opcode,
                        taz_v1_ErrorCode code, const char *message,
                        const char *detail);

#ifdef __cplusplus
}
#endif

#endif /* TAZ_ERROR_H */
