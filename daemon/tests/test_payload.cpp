// Tests for CAPABILITY, VersionResponse, and ERROR payload encoders.

#include <cstring>
#include <set>

#include <gtest/gtest.h>
#include <pb_decode.h>

#include "taz/build_info.h"
#include "taz/error.h"
#include "taz/frame.h"
#include "taz/payload.h"
#include "taz/v1/common.pb.h"
#include "taz/v1/daemon_control.pb.h"

namespace
{

// ---------------------------------------------------------------------------
// CAPABILITY round-trip
// ---------------------------------------------------------------------------

TEST(Payload, CapabilityRoundTrip)
{
    uint8_t buf[TAZ_FRAME_MAX_PAYLOAD_CAPABILITY];
    const size_t n = taz_payload_capability(buf, sizeof(buf));
    ASSERT_GT(n, 0U);
    EXPECT_LE(n, static_cast<size_t>(TAZ_FRAME_MAX_PAYLOAD_CAPABILITY));

    taz_v1_CapabilityPayload cap = taz_v1_CapabilityPayload_init_zero;
    pb_istream_t stream = pb_istream_from_buffer(buf, n);
    ASSERT_TRUE(pb_decode(&stream, taz_v1_CapabilityPayload_fields, &cap));

    EXPECT_EQ(cap.protocol_major, 1U);
    EXPECT_EQ(cap.protocol_minor, 0U);
    ASSERT_EQ(cap.operations_count, 21U);

    // Collect operations into a set for order-independent verification.
    const std::set<uint32_t> ops(cap.operations,
                                 cap.operations + cap.operations_count);
    EXPECT_TRUE(ops.count(static_cast<uint32_t>(taz_v1_Opcode_OPCODE_PING)));
    EXPECT_TRUE(ops.count(static_cast<uint32_t>(taz_v1_Opcode_OPCODE_VERSION)));
    EXPECT_TRUE(ops.count(
        static_cast<uint32_t>(taz_v1_Opcode_OPCODE_CONFIGURATION_GET)));
    EXPECT_TRUE(ops.count(
        static_cast<uint32_t>(taz_v1_Opcode_OPCODE_CONFIGURATION_UPDATE)));
    EXPECT_TRUE(
        ops.count(static_cast<uint32_t>(taz_v1_Opcode_OPCODE_COMMAND_EXEC)));
    EXPECT_TRUE(
        ops.count(static_cast<uint32_t>(taz_v1_Opcode_OPCODE_FILE_STAT)));
    EXPECT_TRUE(
        ops.count(static_cast<uint32_t>(taz_v1_Opcode_OPCODE_FILE_CREATE)));
    EXPECT_TRUE(
        ops.count(static_cast<uint32_t>(taz_v1_Opcode_OPCODE_FILE_DELETE)));
    EXPECT_TRUE(
        ops.count(static_cast<uint32_t>(taz_v1_Opcode_OPCODE_FILE_CHMOD)));
    EXPECT_TRUE(
        ops.count(static_cast<uint32_t>(taz_v1_Opcode_OPCODE_DIR_MAKE)));
    EXPECT_TRUE(
        ops.count(static_cast<uint32_t>(taz_v1_Opcode_OPCODE_DIR_LIST)));
    EXPECT_TRUE(
        ops.count(static_cast<uint32_t>(taz_v1_Opcode_OPCODE_DIR_REMOVE)));
    EXPECT_TRUE(
        ops.count(static_cast<uint32_t>(taz_v1_Opcode_OPCODE_FILE_PUT)));
    EXPECT_TRUE(ops.count(static_cast<uint32_t>(taz_v1_Opcode_OPCODE_CANCEL)));
    EXPECT_TRUE(
        ops.count(static_cast<uint32_t>(taz_v1_Opcode_OPCODE_FILE_GET)));
    EXPECT_TRUE(
        ops.count(static_cast<uint32_t>(taz_v1_Opcode_OPCODE_PROCESS_LIST)));
    EXPECT_TRUE(
        ops.count(static_cast<uint32_t>(taz_v1_Opcode_OPCODE_PROCESS_KILL)));
    EXPECT_TRUE(
        ops.count(static_cast<uint32_t>(taz_v1_Opcode_OPCODE_PROCESS_INFO)));
    EXPECT_TRUE(
        ops.count(static_cast<uint32_t>(taz_v1_Opcode_OPCODE_PROCESS_MONITOR)));
    EXPECT_TRUE(ops.count(static_cast<uint32_t>(taz_v1_Opcode_OPCODE_LOG)));
    EXPECT_TRUE(
        ops.count(static_cast<uint32_t>(taz_v1_Opcode_OPCODE_TIMEOUT_SET)));

    ASSERT_GE(cap.compression_count, 1U);
    EXPECT_STREQ(cap.compression[0], "NONE");
}

TEST(Payload, CapabilityFitsInFrameLimit)
{
    uint8_t buf[TAZ_FRAME_MAX_PAYLOAD_CAPABILITY];
    const size_t n = taz_payload_capability(buf, sizeof(buf));
    EXPECT_GT(n, 0U);
    EXPECT_LE(n, static_cast<size_t>(TAZ_FRAME_MAX_PAYLOAD_CAPABILITY));
}

// ---------------------------------------------------------------------------
// VersionResponse round-trip
// ---------------------------------------------------------------------------

TEST(Payload, VersionResponseRoundTrip)
{
    uint8_t buf[TAZ_FRAME_MAX_PAYLOAD_RESPONSE];
    const size_t n = taz_payload_version_response(buf, sizeof(buf));
    ASSERT_GT(n, 0U);

    taz_v1_VersionResponse rsp = taz_v1_VersionResponse_init_zero;
    pb_istream_t stream = pb_istream_from_buffer(buf, n);
    ASSERT_TRUE(pb_decode(&stream, taz_v1_VersionResponse_fields, &rsp));

    EXPECT_STREQ(rsp.version, taz_build_version());
    EXPECT_STREQ(rsp.build, taz_build_id());
    EXPECT_GT(std::strlen(rsp.platform), 0U);
}

// ---------------------------------------------------------------------------
// ERROR round-trip
// ---------------------------------------------------------------------------

TEST(Payload, ErrorRoundTrip)
{
    uint8_t buf[TAZ_FRAME_MAX_PAYLOAD_ERROR];
    const size_t n = taz_error_encode(buf, sizeof(buf),
                                      taz_v1_ErrorCode_ERROR_CODE_NOT_FOUND,
                                      "item not found", "key=abc");
    ASSERT_GT(n, 0U);
    EXPECT_LE(n, static_cast<size_t>(TAZ_FRAME_MAX_PAYLOAD_ERROR));

    taz_v1_ErrorInfo info = taz_v1_ErrorInfo_init_zero;
    pb_istream_t stream = pb_istream_from_buffer(buf, n);
    ASSERT_TRUE(pb_decode(&stream, taz_v1_ErrorInfo_fields, &info));

    EXPECT_EQ(info.code, taz_v1_ErrorCode_ERROR_CODE_NOT_FOUND);
    EXPECT_STREQ(info.message, "item not found");
    EXPECT_STREQ(info.detail, "key=abc");
}

TEST(Payload, ErrorNullMessageAndDetail)
{
    uint8_t buf[TAZ_FRAME_MAX_PAYLOAD_ERROR];
    const size_t n = taz_error_encode(
        buf, sizeof(buf), taz_v1_ErrorCode_ERROR_CODE_PROTOCOL_ERROR, nullptr,
        nullptr);
    ASSERT_GT(n, 0U);

    taz_v1_ErrorInfo info = taz_v1_ErrorInfo_init_zero;
    pb_istream_t stream = pb_istream_from_buffer(buf, n);
    ASSERT_TRUE(pb_decode(&stream, taz_v1_ErrorInfo_fields, &info));

    EXPECT_EQ(info.code, taz_v1_ErrorCode_ERROR_CODE_PROTOCOL_ERROR);
    EXPECT_STREQ(info.message, "");
    EXPECT_STREQ(info.detail, "");
}

TEST(Payload, ErrorBufferTooSmall)
{
    uint8_t buf[1];
    const size_t n =
        taz_error_encode(buf, sizeof(buf), taz_v1_ErrorCode_ERROR_CODE_INTERNAL,
                         "internal error", nullptr);
    EXPECT_EQ(n, 0U);
}

} // namespace
