#include "taz/error.h"

#include <string.h>

#include <pb_encode.h>

#include "taz/v1/common.pb.h"

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
