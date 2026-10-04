#ifndef TAZ_ERROR_H
#define TAZ_ERROR_H

#include <stddef.h>
#include <stdint.h>

#include <uv.h>

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

    /* Map a failed synchronous uv_fs_* request (req->result < 0) to an
     * ErrorCode via the platform error from uv_fs_get_system_error. */
    taz_v1_ErrorCode taz_error_from_fs_req(const uv_fs_t *req);

    /* The platform error string for a failed synchronous uv_fs_* request,
     * for use as the ErrorInfo detail field. */
    const char *taz_error_fs_detail(const uv_fs_t *req);

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
