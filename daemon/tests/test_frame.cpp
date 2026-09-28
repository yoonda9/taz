// Tests for the C framing layer: pack/unpack, max-payload table, validate.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>

#include <gtest/gtest.h>

#include "taz/frame.h"
#include "taz/v1/common.pb.h"

namespace
{

// Protocol §6 defaults as literals, so frame.c is checked against the spec
// rather than against the frame.h macros it is built from.
struct SpecLimit
{
    uint8_t type;
    uint32_t max_payload;
};

constexpr SpecLimit kSpecLimits[] = {
    {taz_v1_FrameType_FRAME_TYPE_REQUEST, 65536U},
    {taz_v1_FrameType_FRAME_TYPE_RESPONSE, 65536U},
    {taz_v1_FrameType_FRAME_TYPE_FILE_CHUNK, 65536U},
    {taz_v1_FrameType_FRAME_TYPE_ERROR, 4096U},
    {taz_v1_FrameType_FRAME_TYPE_PING, 0U},
    {taz_v1_FrameType_FRAME_TYPE_PONG, 0U},
    {taz_v1_FrameType_FRAME_TYPE_CAPABILITY, 1024U},
};

// Every FrameType in common.proto (0x01.._MAX) needs a row above.
static_assert(std::size(kSpecLimits) ==
                  static_cast<std::size_t>(_taz_v1_FrameType_MAX),
              "new FrameType: add its protocol §6 limit to kSpecLimits");

// UNSPECIFIED, the first reserved value, and the largest type byte.
constexpr uint8_t kUnknownTypes[] = {
    taz_v1_FrameType_FRAME_TYPE_UNSPECIFIED,
    static_cast<uint8_t>(_taz_v1_FrameType_MAX + 1),
    0xFFU,
};

// buf and out start as all ones, so pack and unpack must assign every byte and
// field rather than OR into a zeroed destination.
taz_frame_header_t RoundTrip(const taz_frame_header_t &in)
{
    uint8_t buf[TAZ_FRAME_HEADER_SIZE];
    std::memset(buf, 0xFF, sizeof buf);
    taz_frame_pack_header(&in, buf);
    taz_frame_header_t out;
    std::memset(&out, 0xFF, sizeof out);
    taz_frame_unpack_header(buf, &out);
    return out;
}

taz_frame_verdict_t Validate(uint8_t type, uint32_t length)
{
    taz_frame_header_t h{};
    h.type = type;
    h.length = length;
    return taz_frame_validate_header(&h);
}

// ---------------------------------------------------------------------------
// Pack / unpack
// ---------------------------------------------------------------------------

TEST(Frame, PackUnpackRoundTrip)
{
    taz_frame_header_t in{};
    in.type = taz_v1_FrameType_FRAME_TYPE_REQUEST;
    in.flags = 0xABU;
    in.opcode = 0x1234U;
    in.length = 0x12ABCDEFU;
    in.stream_id = 0xDEADBEEFU;

    const taz_frame_header_t out = RoundTrip(in);

    EXPECT_EQ(out.type, in.type);
    EXPECT_EQ(out.flags, in.flags);
    EXPECT_EQ(out.opcode, in.opcode);
    EXPECT_EQ(out.length, in.length);
    EXPECT_EQ(out.stream_id, in.stream_id);
}

TEST(Frame, WireLayoutLittleEndian)
{
    // Verify each multi-byte field is stored little-endian.
    taz_frame_header_t h{};
    h.type = taz_v1_FrameType_FRAME_TYPE_RESPONSE;
    h.flags = 0x01U;
    h.opcode = 0x1234U;        // lo=0x34, hi=0x12
    h.length = 0x01020304U;    // bytes: 04 03 02 01
    h.stream_id = 0xAABBCCDDU; // bytes: DD CC BB AA

    uint8_t buf[TAZ_FRAME_HEADER_SIZE]{};
    taz_frame_pack_header(&h, buf);

    EXPECT_EQ(buf[0],
              static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_RESPONSE));
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
    EXPECT_EQ(buf[8], 0xDDU);
    EXPECT_EQ(buf[9], 0xCCU);
    EXPECT_EQ(buf[10], 0xBBU);
    EXPECT_EQ(buf[11], 0xAAU);
}

TEST(Frame, HeaderSizeIs12Bytes)
{
    EXPECT_EQ(TAZ_FRAME_HEADER_SIZE, 12);
}

TEST(Frame, OpcodeEdgeValuesRoundTrip)
{
    // 0xFFFF and 0x0134 must survive pack/unpack unchanged.
    taz_frame_header_t h{};
    h.type = taz_v1_FrameType_FRAME_TYPE_REQUEST;
    h.opcode = 0xFFFFU;
    EXPECT_EQ(RoundTrip(h).opcode, 0xFFFFU);

    h.opcode = 0x0134U;
    EXPECT_EQ(RoundTrip(h).opcode, 0x0134U);
}

TEST(Frame, StreamIdBoundaryValuesRoundTrip)
{
    // stream_id=0 and 0xFFFFFFFF must survive pack/unpack unchanged.
    taz_frame_header_t h{};
    h.type = taz_v1_FrameType_FRAME_TYPE_REQUEST;
    h.stream_id = 0U;
    EXPECT_EQ(RoundTrip(h).stream_id, 0U);

    h.stream_id = 0xFFFFFFFFU;
    EXPECT_EQ(RoundTrip(h).stream_id, 0xFFFFFFFFU);
}

TEST(Frame, AllFlagBitsClearedRoundTrips)
{
    // flags=0 next to all-ones fields: packing must not set any flag bit.
    taz_frame_header_t h{};
    h.type = taz_v1_FrameType_FRAME_TYPE_REQUEST;
    h.flags = 0U;
    h.opcode = 0xFFFFU;
    h.length = 0xFFFFFFFFU;
    h.stream_id = 0xFFFFFFFFU;

    uint8_t buf[TAZ_FRAME_HEADER_SIZE]{};
    taz_frame_pack_header(&h, buf);
    EXPECT_EQ(buf[1], 0U);
    EXPECT_EQ(RoundTrip(h).flags, 0U);
}

// ---------------------------------------------------------------------------
// Max-payload table (§6)
// ---------------------------------------------------------------------------

TEST(Frame, MaxPayloadMatchesSpec)
{
    for (const SpecLimit &row : kSpecLimits)
    {
        EXPECT_EQ(taz_frame_max_payload(row.type), row.max_payload)
            << "type=" << +row.type;
    }
}

TEST(Frame, MaxPayloadUnknownTypeBoundIs65536)
{
    // UNSPECIFIED and reserved types use the 64 KiB bound (§10.1).
    for (uint8_t t : kUnknownTypes)
    {
        EXPECT_EQ(taz_frame_max_payload(t), 65536U) << "type=" << +t;
    }
    EXPECT_EQ(TAZ_FRAME_MAX_PAYLOAD, 65536U);
}

// ---------------------------------------------------------------------------
// Validate header
// ---------------------------------------------------------------------------

TEST(Frame, ValidateKnownTypesAtLimitOkAndOneOverOversized)
{
    for (const SpecLimit &row : kSpecLimits)
    {
        EXPECT_EQ(Validate(row.type, row.max_payload), TAZ_FRAME_OK)
            << "type=" << +row.type;
        EXPECT_EQ(Validate(row.type, row.max_payload + 1U), TAZ_FRAME_OVERSIZED)
            << "type=" << +row.type;
    }
}

TEST(Frame, ValidateUnknownTypesAgainst64KiBBound)
{
    // Within the bound an unknown type is skippable; over it, oversized
    // (§10.1).
    for (uint8_t t : kUnknownTypes)
    {
        EXPECT_EQ(Validate(t, 0U), TAZ_FRAME_UNKNOWN_TYPE) << "type=" << +t;
        EXPECT_EQ(Validate(t, 65536U), TAZ_FRAME_UNKNOWN_TYPE) << "type=" << +t;
        EXPECT_EQ(Validate(t, 65537U), TAZ_FRAME_OVERSIZED) << "type=" << +t;
    }
}

TEST(Frame, ValidatePingNonzeroOpcodeIsOk)
{
    // Validation never inspects the opcode field; PING with opcode 0xFFFF is
    // OK.
    taz_frame_header_t h{};
    h.type = taz_v1_FrameType_FRAME_TYPE_PING;
    h.length = 0U;
    h.opcode = 0xFFFFU;
    EXPECT_EQ(taz_frame_validate_header(&h), TAZ_FRAME_OK);
}

TEST(Frame, ValidateAllFlagBitsSetIsOkAndRoundTrips)
{
    // flags=0xFF must validate as OK and survive a pack/unpack round-trip.
    taz_frame_header_t h{};
    h.type = taz_v1_FrameType_FRAME_TYPE_REQUEST;
    h.flags = 0xFFU;
    h.length = 0U;
    EXPECT_EQ(taz_frame_validate_header(&h), TAZ_FRAME_OK);
    EXPECT_EQ(RoundTrip(h).flags, 0xFFU);
}

} // namespace
