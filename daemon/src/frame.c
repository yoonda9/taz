#include "taz/frame.h"

#include "taz/v1/common.pb.h"

/* Byte positions in the 12-byte header that fall outside the ignored list */
#define FRAME_OFF_OPCODE_HI 3U
#define FRAME_OFF_LENGTH_1 5U
#define FRAME_OFF_LENGTH_2 6U
#define FRAME_OFF_LENGTH_3 7U
#define FRAME_OFF_STREAM_1 9U
#define FRAME_OFF_STREAM_2 10U
#define FRAME_OFF_STREAM_3 11U
#define FRAME_SHIFT_24 24U

void taz_frame_pack_header(const taz_frame_header_t *header,
                            uint8_t buf[TAZ_FRAME_HEADER_SIZE])
{
    buf[0] = header->type;
    buf[1] = header->flags;
    buf[2] = (uint8_t)(header->opcode & 0xFFU);
    buf[FRAME_OFF_OPCODE_HI] = (uint8_t)((header->opcode >> 8U) & 0xFFU);
    buf[4] = (uint8_t)(header->length & 0xFFU);
    buf[FRAME_OFF_LENGTH_1] = (uint8_t)((header->length >> 8U) & 0xFFU);
    buf[FRAME_OFF_LENGTH_2] = (uint8_t)((header->length >> 16U) & 0xFFU);
    buf[FRAME_OFF_LENGTH_3] = (uint8_t)((header->length >> FRAME_SHIFT_24) & 0xFFU);
    buf[8] = (uint8_t)(header->stream_id & 0xFFU);
    buf[FRAME_OFF_STREAM_1] = (uint8_t)((header->stream_id >> 8U) & 0xFFU);
    buf[FRAME_OFF_STREAM_2] = (uint8_t)((header->stream_id >> 16U) & 0xFFU);
    buf[FRAME_OFF_STREAM_3] = (uint8_t)((header->stream_id >> FRAME_SHIFT_24) & 0xFFU);
}

void taz_frame_unpack_header(const uint8_t buf[TAZ_FRAME_HEADER_SIZE],
                              taz_frame_header_t *header)
{
    header->type = buf[0];
    header->flags = buf[1];
    header->opcode =
        (uint16_t)((uint16_t)buf[2] | ((uint16_t)buf[FRAME_OFF_OPCODE_HI] << 8U));
    header->length = (uint32_t)buf[4] | ((uint32_t)buf[FRAME_OFF_LENGTH_1] << 8U) |
                     ((uint32_t)buf[FRAME_OFF_LENGTH_2] << 16U) |
                     ((uint32_t)buf[FRAME_OFF_LENGTH_3] << FRAME_SHIFT_24);
    header->stream_id = (uint32_t)buf[8] | ((uint32_t)buf[FRAME_OFF_STREAM_1] << 8U) |
                        ((uint32_t)buf[FRAME_OFF_STREAM_2] << 16U) |
                        ((uint32_t)buf[FRAME_OFF_STREAM_3] << FRAME_SHIFT_24);
}

uint32_t taz_frame_max_payload(uint8_t type)
{
    switch (type) {
    case taz_v1_FrameType_FRAME_TYPE_PING:
    case taz_v1_FrameType_FRAME_TYPE_PONG:
        return TAZ_FRAME_MAX_PAYLOAD_PING;
    case taz_v1_FrameType_FRAME_TYPE_CAPABILITY:
        return TAZ_FRAME_MAX_PAYLOAD_CAPABILITY;
    case taz_v1_FrameType_FRAME_TYPE_ERROR:
        return TAZ_FRAME_MAX_PAYLOAD_ERROR;
    default:
        /* REQUEST, RESPONSE, FILE_CHUNK, and all unknown types share the
         * 64 KiB bound (§6 and §10.1). */
        return TAZ_FRAME_MAX_PAYLOAD;
    }
}

taz_frame_verdict_t taz_frame_validate_header(const taz_frame_header_t *header)
{
    switch (header->type) {
    case taz_v1_FrameType_FRAME_TYPE_REQUEST:
    case taz_v1_FrameType_FRAME_TYPE_RESPONSE:
    case taz_v1_FrameType_FRAME_TYPE_FILE_CHUNK:
    case taz_v1_FrameType_FRAME_TYPE_ERROR:
    case taz_v1_FrameType_FRAME_TYPE_PING:
    case taz_v1_FrameType_FRAME_TYPE_PONG:
    case taz_v1_FrameType_FRAME_TYPE_CAPABILITY:
        if (header->length > taz_frame_max_payload(header->type)) {
            return TAZ_FRAME_OVERSIZED;
        }
        return TAZ_FRAME_OK;
    default:
        /* Unknown/unassigned type (incl. UNSPECIFIED=0x00, reserved >=0x08): §10.1 */
        if (header->length > TAZ_FRAME_MAX_PAYLOAD) {
            return TAZ_FRAME_OVERSIZED;
        }
        return TAZ_FRAME_UNKNOWN_TYPE;
    }
}
