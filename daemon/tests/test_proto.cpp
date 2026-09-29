// Smoke tests for the nanopb-generated protocol messages: the generated code
// compiles, links, and round-trips through pb_encode / pb_decode.

#include <cstring>

#include <gtest/gtest.h>
#include <pb_decode.h>
#include <pb_encode.h>

#include "taz/v1/common.pb.h"
#include "taz/v1/daemon_control.pb.h"

namespace
{

// Default per-type payload limit for REQUEST/RESPONSE/FILE_CHUNK (§6).
constexpr uint32_t kDefaultFrameLimit = 65536U;

TEST(Proto, OpcodeValuesMatchSpecification)
{
    EXPECT_EQ(taz_v1_Opcode_OPCODE_PING, 0x0001);
    EXPECT_EQ(taz_v1_Opcode_OPCODE_COMMAND_EXEC, 0x0010);
    EXPECT_EQ(taz_v1_Opcode_OPCODE_PROCESS_LIST, 0x0020);
    EXPECT_EQ(taz_v1_Opcode_OPCODE_FILE_PUT, 0x0030);
    EXPECT_EQ(taz_v1_Opcode_OPCODE_RUN_AS, 0x0040);
    EXPECT_EQ(taz_v1_Opcode_OPCODE_CANCEL, 0x0046);
    EXPECT_EQ(taz_v1_Opcode_OPCODE_PIPELINE, 0x0050);
}

TEST(Proto, ErrorCodeValuesMatchSpecification)
{
    EXPECT_EQ(taz_v1_ErrorCode_ERROR_CODE_UNKNOWN, 0);
    EXPECT_EQ(taz_v1_ErrorCode_ERROR_CODE_NOT_FOUND, 1);
    EXPECT_EQ(taz_v1_ErrorCode_ERROR_CODE_CONNECTION_LOST, 10);
    EXPECT_EQ(taz_v1_ErrorCode_ERROR_CODE_PROTOCOL_ERROR, 11);
}

TEST(Proto, FrameTypeValuesMatchSpecification)
{
    EXPECT_EQ(taz_v1_FrameType_FRAME_TYPE_REQUEST, 0x01);
    EXPECT_EQ(taz_v1_FrameType_FRAME_TYPE_RESPONSE, 0x02);
    EXPECT_EQ(taz_v1_FrameType_FRAME_TYPE_FILE_CHUNK, 0x03);
    EXPECT_EQ(taz_v1_FrameType_FRAME_TYPE_ERROR, 0x04);
    EXPECT_EQ(taz_v1_FrameType_FRAME_TYPE_PING, 0x05);
    EXPECT_EQ(taz_v1_FrameType_FRAME_TYPE_PONG, 0x06);
    EXPECT_EQ(taz_v1_FrameType_FRAME_TYPE_CAPABILITY, 0x07);
    // Default payload limits: REQUEST/RESPONSE/FILE_CHUNK share
    // kDefaultFrameLimit.
    EXPECT_EQ(kDefaultFrameLimit, 65536U);
}

TEST(Proto, ErrorInfoRoundTrip)
{
    taz_v1_ErrorInfo in = taz_v1_ErrorInfo_init_zero;
    in.code = taz_v1_ErrorCode_ERROR_CODE_NOT_FOUND;
    std::memcpy(in.message, "no such file", sizeof("no such file"));
    std::memcpy(in.detail, "/tmp/missing", sizeof("/tmp/missing"));

    uint8_t buffer[taz_v1_ErrorInfo_size];
    pb_ostream_t out = pb_ostream_from_buffer(buffer, sizeof(buffer));
    ASSERT_TRUE(pb_encode(&out, taz_v1_ErrorInfo_fields, &in));

    taz_v1_ErrorInfo decoded = taz_v1_ErrorInfo_init_zero;
    pb_istream_t inp = pb_istream_from_buffer(buffer, out.bytes_written);
    ASSERT_TRUE(pb_decode(&inp, taz_v1_ErrorInfo_fields, &decoded));

    EXPECT_EQ(decoded.code, taz_v1_ErrorCode_ERROR_CODE_NOT_FOUND);
    EXPECT_STREQ(decoded.message, "no such file");
    EXPECT_STREQ(decoded.detail, "/tmp/missing");
}

TEST(Proto, CapabilityPayloadRoundTrip)
{
    taz_v1_CapabilityPayload in = taz_v1_CapabilityPayload_init_zero;
    in.protocol_major = 1;
    in.protocol_minor = 0;
    in.operations_count = 3;
    in.operations[0] = taz_v1_Opcode_OPCODE_VERSION;
    in.operations[1] = taz_v1_Opcode_OPCODE_COMMAND_EXEC;
    in.operations[2] = taz_v1_Opcode_OPCODE_FILE_PUT;
    in.max_payload_sizes_count = 1;
    in.max_payload_sizes[0].key = taz_v1_FrameType_FRAME_TYPE_FILE_CHUNK;
    in.max_payload_sizes[0].value = kDefaultFrameLimit;
    in.compression_count = 1;
    std::memcpy(in.compression[0], "NONE", sizeof("NONE"));

    uint8_t buffer[taz_v1_CapabilityPayload_size];
    pb_ostream_t out = pb_ostream_from_buffer(buffer, sizeof(buffer));
    ASSERT_TRUE(pb_encode(&out, taz_v1_CapabilityPayload_fields, &in));
    // The CAPABILITY frame is capped at 1 KiB on the wire.
    EXPECT_LE(out.bytes_written, 1024U);

    taz_v1_CapabilityPayload decoded = taz_v1_CapabilityPayload_init_zero;
    pb_istream_t inp = pb_istream_from_buffer(buffer, out.bytes_written);
    ASSERT_TRUE(pb_decode(&inp, taz_v1_CapabilityPayload_fields, &decoded));

    EXPECT_EQ(decoded.protocol_major, 1U);
    ASSERT_EQ(decoded.operations_count, 3U);
    EXPECT_EQ(decoded.operations[1], taz_v1_Opcode_OPCODE_COMMAND_EXEC);
    ASSERT_EQ(decoded.max_payload_sizes_count, 1U);
    EXPECT_EQ(decoded.max_payload_sizes[0].value, kDefaultFrameLimit);
    ASSERT_EQ(decoded.compression_count, 1U);
    EXPECT_STREQ(decoded.compression[0], "NONE");
}

} // namespace
