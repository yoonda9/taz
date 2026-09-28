// Tests for the C framing layer: pack/unpack, max-payload table, validate.

#include <cstdint>
#include <cstring>

#include <gtest/gtest.h>

#include "taz/frame.h"
#include "taz/v1/common.pb.h"

namespace
{

// ---------------------------------------------------------------------------
// Pack / unpack
// ---------------------------------------------------------------------------

TEST(Frame, PackUnpackRoundTrip)
{
    taz_frame_header_t in{};
    in.type      = taz_v1_FrameType_FRAME_TYPE_REQUEST;
    in.flags     = 0xABU;
    in.opcode    = 0x1234U;
    in.length    = 0x00ABCDEFU;
    in.stream_id = 0xDEADBEEFU;

    uint8_t buf[TAZ_FRAME_HEADER_SIZE]{};
    taz_frame_pack_header(&in, buf);

    taz_frame_header_t out{};
    taz_frame_unpack_header(buf, &out);

    EXPECT_EQ(out.type,      in.type);
    EXPECT_EQ(out.flags,     in.flags);
    EXPECT_EQ(out.opcode,    in.opcode);
    EXPECT_EQ(out.length,    in.length);
    EXPECT_EQ(out.stream_id, in.stream_id);
}

TEST(Frame, WireLayoutLittleEndian)
{
    // Verify each multi-byte field is stored little-endian.
    taz_frame_header_t h{};
    h.type      = taz_v1_FrameType_FRAME_TYPE_RESPONSE;
    h.flags     = 0x01U;
    h.opcode    = 0x1234U;   // lo=0x34, hi=0x12
    h.length    = 0x01020304U; // bytes: 04 03 02 01
    h.stream_id = 0xAABBCCDDU; // bytes: DD CC BB AA

    uint8_t buf[TAZ_FRAME_HEADER_SIZE]{};
    taz_frame_pack_header(&h, buf);

    EXPECT_EQ(buf[0], static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_RESPONSE));
    EXPECT_EQ(buf[1], 0x01U);

    // opcode LE
    EXPECT_EQ(buf[2], 0x34U);
    EXPECT_EQ(buf[3], 0x12U);

    // length LE
    EXPECT_EQ(buf[4], 0x04U);
    EXPECT_EQ(buf[5], 0x03U);
    EXPECT_EQ(buf[6], 0x02U);
    EXPECT_EQ(buf[7], 0x01U);

    // stream_id LE
    EXPECT_EQ(buf[8],  0xDDU);
    EXPECT_EQ(buf[9],  0xCCU);
    EXPECT_EQ(buf[10], 0xBBU);
    EXPECT_EQ(buf[11], 0xAAU);
}

TEST(Frame, HeaderSizeIs12Bytes)
{
    EXPECT_EQ(TAZ_FRAME_HEADER_SIZE, 12);
}

// ---------------------------------------------------------------------------
// Max-payload table (§6)
// ---------------------------------------------------------------------------

TEST(Frame, MaxPayloadPingAndPongIsZero)
{
    EXPECT_EQ(taz_frame_max_payload(taz_v1_FrameType_FRAME_TYPE_PING), 0U);
    EXPECT_EQ(taz_frame_max_payload(taz_v1_FrameType_FRAME_TYPE_PONG), 0U);
}

TEST(Frame, MaxPayloadCapabilityIs1024)
{
    EXPECT_EQ(taz_frame_max_payload(taz_v1_FrameType_FRAME_TYPE_CAPABILITY),
              TAZ_FRAME_MAX_PAYLOAD_CAPABILITY);
    EXPECT_EQ(TAZ_FRAME_MAX_PAYLOAD_CAPABILITY, 1024U);
}

TEST(Frame, MaxPayloadErrorIs4096)
{
    EXPECT_EQ(taz_frame_max_payload(taz_v1_FrameType_FRAME_TYPE_ERROR),
              TAZ_FRAME_MAX_PAYLOAD_ERROR);
    EXPECT_EQ(TAZ_FRAME_MAX_PAYLOAD_ERROR, 4096U);
}

TEST(Frame, MaxPayloadRequestResponseFileChunkIs65536)
{
    EXPECT_EQ(taz_frame_max_payload(taz_v1_FrameType_FRAME_TYPE_REQUEST),
              TAZ_FRAME_MAX_PAYLOAD_REQUEST);
    EXPECT_EQ(taz_frame_max_payload(taz_v1_FrameType_FRAME_TYPE_RESPONSE),
              TAZ_FRAME_MAX_PAYLOAD_RESPONSE);
    EXPECT_EQ(taz_frame_max_payload(taz_v1_FrameType_FRAME_TYPE_FILE_CHUNK),
              TAZ_FRAME_MAX_PAYLOAD_FILE_CHUNK);
    EXPECT_EQ(TAZ_FRAME_MAX_PAYLOAD_REQUEST, 65536U);
}

TEST(Frame, MaxPayloadUnknownTypeBoundIs65536)
{
    // UNSPECIFIED(0) and any reserved type (>=8) use the 64 KiB bound (§10.1).
    EXPECT_EQ(taz_frame_max_payload(taz_v1_FrameType_FRAME_TYPE_UNSPECIFIED),
              TAZ_FRAME_MAX_PAYLOAD);
    EXPECT_EQ(taz_frame_max_payload(0x08U), TAZ_FRAME_MAX_PAYLOAD);
    EXPECT_EQ(taz_frame_max_payload(0xFFU), TAZ_FRAME_MAX_PAYLOAD);
    EXPECT_EQ(TAZ_FRAME_MAX_PAYLOAD, 65536U);
}

// ---------------------------------------------------------------------------
// Validate header
// ---------------------------------------------------------------------------

TEST(Frame, ValidateKnownTypesAtLimitIsOk)
{
    const uint8_t known_types[] = {
        taz_v1_FrameType_FRAME_TYPE_REQUEST,
        taz_v1_FrameType_FRAME_TYPE_RESPONSE,
        taz_v1_FrameType_FRAME_TYPE_FILE_CHUNK,
        taz_v1_FrameType_FRAME_TYPE_ERROR,
        taz_v1_FrameType_FRAME_TYPE_CAPABILITY,
    };
    for (uint8_t t : known_types) {
        taz_frame_header_t h{};
        h.type   = t;
        h.length = taz_frame_max_payload(t);
        EXPECT_EQ(taz_frame_validate_header(&h), TAZ_FRAME_OK) << "type=" << +t;
    }
}

TEST(Frame, ValidatePingPongZeroLengthIsOk)
{
    taz_frame_header_t h{};
    h.type   = taz_v1_FrameType_FRAME_TYPE_PING;
    h.length = 0U;
    EXPECT_EQ(taz_frame_validate_header(&h), TAZ_FRAME_OK);

    h.type = taz_v1_FrameType_FRAME_TYPE_PONG;
    EXPECT_EQ(taz_frame_validate_header(&h), TAZ_FRAME_OK);
}

TEST(Frame, ValidatePingWithPayloadIsOversized)
{
    taz_frame_header_t h{};
    h.type   = taz_v1_FrameType_FRAME_TYPE_PING;
    h.length = 1U;
    EXPECT_EQ(taz_frame_validate_header(&h), TAZ_FRAME_OVERSIZED);
}

TEST(Frame, ValidateCapabilityOversized)
{
    taz_frame_header_t h{};
    h.type   = taz_v1_FrameType_FRAME_TYPE_CAPABILITY;
    h.length = TAZ_FRAME_MAX_PAYLOAD_CAPABILITY + 1U;
    EXPECT_EQ(taz_frame_validate_header(&h), TAZ_FRAME_OVERSIZED);
}

TEST(Frame, ValidateRequestOversized)
{
    taz_frame_header_t h{};
    h.type   = taz_v1_FrameType_FRAME_TYPE_REQUEST;
    h.length = TAZ_FRAME_MAX_PAYLOAD_REQUEST + 1U;
    EXPECT_EQ(taz_frame_validate_header(&h), TAZ_FRAME_OVERSIZED);
}

TEST(Frame, ValidateUnknownTypeWithinBoundIsUnknownType)
{
    taz_frame_header_t h{};
    h.type   = taz_v1_FrameType_FRAME_TYPE_UNSPECIFIED; // 0x00
    h.length = 0U;
    EXPECT_EQ(taz_frame_validate_header(&h), TAZ_FRAME_UNKNOWN_TYPE);

    h.type = 0x08U; // reserved
    EXPECT_EQ(taz_frame_validate_header(&h), TAZ_FRAME_UNKNOWN_TYPE);
}

TEST(Frame, ValidateUnknownTypeOversized)
{
    // Unknown type still applies the 64 KiB guard (§10.1).
    taz_frame_header_t h{};
    h.type   = 0x08U;
    h.length = TAZ_FRAME_MAX_PAYLOAD + 1U;
    EXPECT_EQ(taz_frame_validate_header(&h), TAZ_FRAME_OVERSIZED);
}

TEST(Frame, ValidateUnknownTypeAtExactBoundIsUnknownType)
{
    taz_frame_header_t h{};
    h.type   = 0x09U;
    h.length = TAZ_FRAME_MAX_PAYLOAD;
    EXPECT_EQ(taz_frame_validate_header(&h), TAZ_FRAME_UNKNOWN_TYPE);
}

} // namespace
