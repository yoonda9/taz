#include "version.h"

#include <string.h>

#include "taz/error.h"
#include "taz/frame.h"
#include "taz/payload.h"
#include "taz/v1/common.pb.h"

#define VERSION_PAYLOAD_BUFSZ 512U
#define VERSION_SEND_BUFSZ (TAZ_FRAME_HEADER_SIZE + TAZ_FRAME_MAX_PAYLOAD_ERROR)

static void write_frame(uint8_t type, uint8_t flags, uint16_t opcode,
                        uint32_t stream_id, const uint8_t *payload,
                        uint32_t pay_len, taz_dispatch_write_fn_t write_fn,
                        void *ctx)
{
    uint8_t buf[VERSION_SEND_BUFSZ];
    taz_frame_header_t h;

    if (pay_len > TAZ_FRAME_MAX_PAYLOAD_ERROR)
    {
        return;
    }

    h.type = type;
    h.flags = flags;
    h.opcode = opcode;
    h.length = pay_len;
    h.stream_id = stream_id;
    taz_frame_pack_header(&h, buf);

    if (pay_len > 0U && payload != NULL)
    {
        (void)memcpy(buf + TAZ_FRAME_HEADER_SIZE, payload, (size_t)pay_len);
    }

    write_fn(buf, (size_t)(TAZ_FRAME_HEADER_SIZE + pay_len), ctx);
}

void handle_version(const taz_frame_header_t *header, const uint8_t *payload,
                    taz_dispatch_write_fn_t write_fn, void *ctx)
{
    uint8_t pay[VERSION_PAYLOAD_BUFSZ];
    size_t len;

    (void)payload;

    len = taz_payload_version_response(pay, sizeof(pay));
    if (len == 0U)
    {
        uint8_t err_payload[TAZ_FRAME_MAX_PAYLOAD_ERROR];
        size_t err_len = taz_error_encode(err_payload, sizeof(err_payload),
                                          taz_v1_ErrorCode_ERROR_CODE_INTERNAL,
                                          "version encode failed", NULL);
        if (err_len > 0U)
        {
            write_frame((uint8_t)taz_v1_FrameType_FRAME_TYPE_ERROR,
                        (uint8_t)taz_v1_FrameFlag_FRAME_FLAG_NONE,
                        header->opcode, header->stream_id, err_payload,
                        (uint32_t)err_len, write_fn, ctx);
        }
        return;
    }

    write_frame((uint8_t)taz_v1_FrameType_FRAME_TYPE_RESPONSE,
                (uint8_t)taz_v1_FrameFlag_FRAME_FLAG_NONE, header->opcode,
                header->stream_id, pay, (uint32_t)len, write_fn, ctx);
}
