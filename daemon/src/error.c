#include "taz/error.h"

#include <errno.h>
#include <string.h>

#include <pb_encode.h>

#include "taz/frame.h"
#include "taz/v1/common.pb.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#define ERROR_SEND_BUFSZ (TAZ_FRAME_HEADER_SIZE + TAZ_FRAME_MAX_PAYLOAD_ERROR)

size_t taz_error_encode(uint8_t *buf, size_t bufsize, taz_v1_ErrorCode code,
                        const char *message, const char *detail)
{
    taz_v1_ErrorInfo msg = taz_v1_ErrorInfo_init_zero;
    msg.code = code;
    if (message != NULL)
    {
        (void)strncpy(msg.message, message, sizeof(msg.message) - 1U);
    }
    if (detail != NULL)
    {
        (void)strncpy(msg.detail, detail, sizeof(msg.detail) - 1U);
    }

    pb_ostream_t stream = pb_ostream_from_buffer(buf, bufsize);
    if (!pb_encode(&stream, taz_v1_ErrorInfo_fields, &msg))
    {
        return 0U;
    }
    return stream.bytes_written;
}

taz_v1_ErrorCode taz_error_from_errno(int errnum)
{
    switch (errnum)
    {
        case ENOENT:
            return taz_v1_ErrorCode_ERROR_CODE_NOT_FOUND;
        case EACCES:
        case EPERM:
            return taz_v1_ErrorCode_ERROR_CODE_PERMISSION_DENIED;
        case EEXIST:
            return taz_v1_ErrorCode_ERROR_CODE_ALREADY_EXISTS;
        case ESRCH:
            return taz_v1_ErrorCode_ERROR_CODE_NOT_FOUND;
        default:
            return taz_v1_ErrorCode_ERROR_CODE_INTERNAL;
    }
}

#ifdef _WIN32
taz_v1_ErrorCode taz_error_from_win32(unsigned long err)
{
    switch (err)
    {
        case ERROR_FILE_NOT_FOUND:
            return taz_v1_ErrorCode_ERROR_CODE_NOT_FOUND;
        case ERROR_PATH_NOT_FOUND:
            return taz_v1_ErrorCode_ERROR_CODE_NOT_FOUND;
        case ERROR_ACCESS_DENIED:
            return taz_v1_ErrorCode_ERROR_CODE_PERMISSION_DENIED;
        case ERROR_ALREADY_EXISTS:
            return taz_v1_ErrorCode_ERROR_CODE_ALREADY_EXISTS;
        case ERROR_FILE_EXISTS:
            return taz_v1_ErrorCode_ERROR_CODE_ALREADY_EXISTS;
        default:
            return taz_v1_ErrorCode_ERROR_CODE_INTERNAL;
    }
}
#endif

void taz_error_send(taz_dispatch_write_fn_t write_fn, void *ctx,
                    uint32_t stream_id, uint16_t opcode, taz_v1_ErrorCode code,
                    const char *message, const char *detail)
{
    uint8_t buf[ERROR_SEND_BUFSZ];
    uint8_t payload[TAZ_FRAME_MAX_PAYLOAD_ERROR];
    taz_frame_header_t h;
    size_t pay_len;

    pay_len = taz_error_encode(payload, sizeof(payload), code, message, detail);
    if (pay_len == 0U)
    {
        return;
    }

    h.type = (uint8_t)taz_v1_FrameType_FRAME_TYPE_ERROR;
    h.flags = (uint8_t)taz_v1_FrameFlag_FRAME_FLAG_NONE;
    h.opcode = opcode;
    h.length = (uint32_t)pay_len;
    h.stream_id = stream_id;
    taz_frame_pack_header(&h, buf);
    (void)memcpy(buf + TAZ_FRAME_HEADER_SIZE, payload, pay_len);

    write_fn(buf, (size_t)(TAZ_FRAME_HEADER_SIZE + pay_len), ctx);
}
