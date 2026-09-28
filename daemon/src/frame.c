#include "taz/frame.h"

#include <stddef.h>

#include "taz/v1/common.pb.h"

/* Protocol §6 defaults, indexed by frame type. */
static const uint32_t MAX_PAYLOAD[_taz_v1_FrameType_ARRAYSIZE] = {
    [taz_v1_FrameType_FRAME_TYPE_REQUEST] = TAZ_FRAME_MAX_PAYLOAD_REQUEST,
    [taz_v1_FrameType_FRAME_TYPE_RESPONSE] = TAZ_FRAME_MAX_PAYLOAD_RESPONSE,
    [taz_v1_FrameType_FRAME_TYPE_FILE_CHUNK] = TAZ_FRAME_MAX_PAYLOAD_FILE_CHUNK,
    [taz_v1_FrameType_FRAME_TYPE_ERROR] = TAZ_FRAME_MAX_PAYLOAD_ERROR,
    [taz_v1_FrameType_FRAME_TYPE_PING] = TAZ_FRAME_MAX_PAYLOAD_PING,
    [taz_v1_FrameType_FRAME_TYPE_PONG] = TAZ_FRAME_MAX_PAYLOAD_PONG,
    [taz_v1_FrameType_FRAME_TYPE_CAPABILITY] = TAZ_FRAME_MAX_PAYLOAD_CAPABILITY,
};

/* Nonzero for an assigned type. Assigned types are contiguous from 0x01
 * (protocol §4.2); 0x00 is UNSPECIFIED. */
static int is_known_type(uint8_t type)
{
    return (type != taz_v1_FrameType_FRAME_TYPE_UNSPECIFIED) &&
           (type <= _taz_v1_FrameType_MAX);
}

static void store_le(uint8_t *dst, uint32_t value, size_t width)
{
    for (size_t i = 0U; i < width; ++i)
    {
        dst[i] = (uint8_t)(value >> (8U * i));
    }
}

static uint32_t load_le(const uint8_t *src, size_t width)
{
    uint32_t value = 0U;
    for (size_t i = 0U; i < width; ++i)
    {
        value |= (uint32_t)src[i] << (8U * i);
    }
    return value;
}

/* Wire layout (protocol §4.1), little-endian:
 * type[0] flags[1] opcode[2..3] length[4..7] stream_id[8..11] */

void taz_frame_pack_header(const taz_frame_header_t *header,
                           uint8_t buf[TAZ_FRAME_HEADER_SIZE])
{
    buf[0] = header->type;
    buf[1] = header->flags;
    store_le(&buf[2], header->opcode, sizeof header->opcode);
    store_le(&buf[4], header->length, sizeof header->length);
    store_le(&buf[8], header->stream_id, sizeof header->stream_id);
}

void taz_frame_unpack_header(const uint8_t buf[TAZ_FRAME_HEADER_SIZE],
                             taz_frame_header_t *header)
{
    header->type = buf[0];
    header->flags = buf[1];
    header->opcode = (uint16_t)load_le(&buf[2], sizeof header->opcode);
    header->length = load_le(&buf[4], sizeof header->length);
    header->stream_id = load_le(&buf[8], sizeof header->stream_id);
}

uint32_t taz_frame_max_payload(uint8_t type)
{
    /* Unknown types (incl. UNSPECIFIED) share the largest bound (§10.1). */
    return is_known_type(type) ? MAX_PAYLOAD[type] : TAZ_FRAME_MAX_PAYLOAD;
}

taz_frame_verdict_t taz_frame_validate_header(const taz_frame_header_t *header)
{
    if (header->length > taz_frame_max_payload(header->type))
    {
        return TAZ_FRAME_OVERSIZED;
    }
    return is_known_type(header->type) ? TAZ_FRAME_OK : TAZ_FRAME_UNKNOWN_TYPE;
}
