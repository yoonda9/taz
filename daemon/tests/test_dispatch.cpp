// Tests for dispatch module: opcode routing, error responses.

#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <pb_decode.h>
#include <pb_encode.h>

#include "taz/config.h"
#include "taz/dispatch.h"
#include "taz/frame.h"
#include "taz/v1/common.pb.h"
#include "taz/v1/daemon_control.pb.h"

namespace
{

struct WriteCtx
{
    std::vector<std::vector<uint8_t>> frames;
};

void capture_write(const uint8_t *data, size_t len, void *ctx)
{
    auto *wctx = static_cast<WriteCtx *>(ctx);
    wctx->frames.emplace_back(data, data + len);
}

taz_frame_header_t MakeHeader(uint8_t type, uint16_t opcode, uint32_t stream_id,
                              uint32_t length = 0U)
{
    taz_frame_header_t h{};
    h.type = type;
    h.flags = 0U;
    h.opcode = opcode;
    h.length = length;
    h.stream_id = stream_id;
    return h;
}

taz_frame_header_t UnpackHeader(const std::vector<uint8_t> &frame)
{
    taz_frame_header_t h{};
    taz_frame_unpack_header(frame.data(), &h);
    return h;
}

bool DecodeErrorInfo(const std::vector<uint8_t> &frame, taz_v1_ErrorInfo *out)
{
    if (frame.size() <= static_cast<size_t>(TAZ_FRAME_HEADER_SIZE))
    {
        return false;
    }
    pb_istream_t stream = pb_istream_from_buffer(
        frame.data() + TAZ_FRAME_HEADER_SIZE,
        frame.size() - static_cast<size_t>(TAZ_FRAME_HEADER_SIZE));
    return pb_decode(&stream, taz_v1_ErrorInfo_fields, out);
}

// ---------------------------------------------------------------------------
// PING → PONG
// ---------------------------------------------------------------------------

TEST(Dispatch, PingProducesPong)
{
    taz_dispatch_t d;
    taz_dispatch_init(&d);
    WriteCtx wctx;

    const taz_frame_header_t h = MakeHeader(
        static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_PING), 0U, 7U);
    taz_dispatch_frame(&d, &h, nullptr, TAZ_FRAME_OK, capture_write, &wctx);

    ASSERT_EQ(wctx.frames.size(), 1U);
    const auto resp = UnpackHeader(wctx.frames[0]);
    EXPECT_EQ(resp.type,
              static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_PONG));
    EXPECT_EQ(resp.stream_id, 7U);
    EXPECT_EQ(resp.length, 0U);
    EXPECT_EQ(d.active_count, 0U);
}

// ---------------------------------------------------------------------------
// VERSION opcode → RESPONSE with decodable VersionResponse
// ---------------------------------------------------------------------------

TEST(Dispatch, VersionOpcodeProducesResponse)
{
    taz_dispatch_t d;
    taz_dispatch_init(&d);
    WriteCtx wctx;

    const taz_frame_header_t h =
        MakeHeader(static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_REQUEST),
                   static_cast<uint16_t>(taz_v1_Opcode_OPCODE_VERSION), 1U);
    taz_dispatch_frame(&d, &h, nullptr, TAZ_FRAME_OK, capture_write, &wctx);

    ASSERT_EQ(wctx.frames.size(), 1U);
    const auto &frame = wctx.frames[0];
    ASSERT_GE(frame.size(), static_cast<size_t>(TAZ_FRAME_HEADER_SIZE));

    const auto resp = UnpackHeader(frame);
    EXPECT_EQ(resp.type,
              static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_RESPONSE));
    EXPECT_EQ(resp.stream_id, 1U);
    EXPECT_GT(resp.length, 0U);

    taz_v1_VersionResponse ver = taz_v1_VersionResponse_init_zero;
    pb_istream_t stream = pb_istream_from_buffer(
        frame.data() + TAZ_FRAME_HEADER_SIZE,
        frame.size() - static_cast<size_t>(TAZ_FRAME_HEADER_SIZE));
    ASSERT_TRUE(pb_decode(&stream, taz_v1_VersionResponse_fields, &ver));
    EXPECT_GT(std::strlen(ver.version), 0U);

    // Sync handler closes the stream before returning.
    EXPECT_EQ(d.active_count, 0U);
}

// ---------------------------------------------------------------------------
// Unknown frame type (UNKNOWN_TYPE verdict) → ERROR NOT_SUPPORTED
// ---------------------------------------------------------------------------

TEST(Dispatch, UnknownTypeProducesNotSupported)
{
    taz_dispatch_t d;
    taz_dispatch_init(&d);
    WriteCtx wctx;

    const taz_frame_header_t h = MakeHeader(
        static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_UNSPECIFIED), 0U, 3U);
    taz_dispatch_frame(&d, &h, nullptr, TAZ_FRAME_UNKNOWN_TYPE, capture_write,
                       &wctx);

    ASSERT_EQ(wctx.frames.size(), 1U);
    const auto &frame = wctx.frames[0];
    const auto resp = UnpackHeader(frame);
    EXPECT_EQ(resp.type,
              static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_ERROR));
    EXPECT_EQ(resp.stream_id, 3U);

    taz_v1_ErrorInfo err = taz_v1_ErrorInfo_init_zero;
    ASSERT_TRUE(DecodeErrorInfo(frame, &err));
    EXPECT_EQ(err.code, taz_v1_ErrorCode_ERROR_CODE_NOT_SUPPORTED);
}

// ---------------------------------------------------------------------------
// Unknown opcode in REQUEST → ERROR NOT_SUPPORTED (stream not opened)
// ---------------------------------------------------------------------------

TEST(Dispatch, UnknownOpcodeProducesNotSupported)
{
    taz_dispatch_t d;
    taz_dispatch_init(&d);
    WriteCtx wctx;

    const taz_frame_header_t h = MakeHeader(
        static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_REQUEST), 0xFFFFU, 5U);
    taz_dispatch_frame(&d, &h, nullptr, TAZ_FRAME_OK, capture_write, &wctx);

    ASSERT_EQ(wctx.frames.size(), 1U);
    const auto &frame = wctx.frames[0];
    const auto resp = UnpackHeader(frame);
    EXPECT_EQ(resp.type,
              static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_ERROR));
    EXPECT_EQ(resp.stream_id, 5U);
    EXPECT_EQ(resp.opcode, 0xFFFFU);

    taz_v1_ErrorInfo err = taz_v1_ErrorInfo_init_zero;
    ASSERT_TRUE(DecodeErrorInfo(frame, &err));
    EXPECT_EQ(err.code, taz_v1_ErrorCode_ERROR_CODE_NOT_SUPPORTED);

    EXPECT_EQ(d.active_count, 0U);
}

// ---------------------------------------------------------------------------
// Duplicate stream_id → ERROR INVALID_REQUEST (active set unchanged)
// ---------------------------------------------------------------------------

TEST(Dispatch, DuplicateStreamIdProducesInvalidRequest)
{
    taz_dispatch_t d;
    taz_dispatch_init(&d);
    WriteCtx wctx;

    // Simulate an in-flight async stream by pre-populating the active set.
    const uint32_t sid = 42U;
    d.active_streams[0] = sid;
    d.active_count = 1U;

    const taz_frame_header_t h =
        MakeHeader(static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_REQUEST),
                   static_cast<uint16_t>(taz_v1_Opcode_OPCODE_VERSION), sid);
    taz_dispatch_frame(&d, &h, nullptr, TAZ_FRAME_OK, capture_write, &wctx);

    ASSERT_EQ(wctx.frames.size(), 1U);
    const auto &frame = wctx.frames[0];
    const auto resp = UnpackHeader(frame);
    EXPECT_EQ(resp.type,
              static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_ERROR));
    EXPECT_EQ(resp.stream_id, sid);

    taz_v1_ErrorInfo err = taz_v1_ErrorInfo_init_zero;
    ASSERT_TRUE(DecodeErrorInfo(frame, &err));
    EXPECT_EQ(err.code, taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST);

    // Rejection must not mutate the active set.
    EXPECT_EQ(d.active_count, 1U);
    EXPECT_EQ(d.active_streams[0], sid);
}

// ---------------------------------------------------------------------------
// OVERSIZED verdict → no-op (no write, no stream opened)
// ---------------------------------------------------------------------------

TEST(Dispatch, OversizedVerdictIsNoOp)
{
    taz_dispatch_t d;
    taz_dispatch_init(&d);
    WriteCtx wctx;

    const taz_frame_header_t h =
        MakeHeader(static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_REQUEST),
                   static_cast<uint16_t>(taz_v1_Opcode_OPCODE_VERSION), 99U);
    taz_dispatch_frame(&d, &h, nullptr, TAZ_FRAME_OVERSIZED, capture_write,
                       &wctx);

    EXPECT_TRUE(wctx.frames.empty());
    EXPECT_EQ(d.active_count, 0U);
}

// ---------------------------------------------------------------------------
// CONFIGURATION_GET / CONFIGURATION_UPDATE through the dispatch table
// ---------------------------------------------------------------------------

// Field 1 (length-delimited) claims five bytes but only two follow, so no
// request message decodes from it.
std::vector<uint8_t> TruncatedRequest()
{
    return {0x0AU, 0x05U, 'a', 'b'};
}

class DispatchConfig : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        taz_dispatch_init(&d_);
        taz_config_reset();
    }

    void TearDown() override
    {
        taz_config_reset();
    }

    // Dispatch one REQUEST and return the single frame written in reply.
    std::vector<uint8_t> Request(taz_v1_Opcode opcode,
                                 const std::vector<uint8_t> &payload)
    {
        WriteCtx wctx;
        const taz_frame_header_t h = MakeHeader(
            static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_REQUEST),
            static_cast<uint16_t>(opcode), 3U,
            static_cast<uint32_t>(payload.size()));
        taz_dispatch_frame(&d_, &h, payload.empty() ? nullptr : payload.data(),
                           TAZ_FRAME_OK, capture_write, &wctx);
        EXPECT_EQ(wctx.frames.size(), 1U);
        // Sync handlers close the stream before returning.
        EXPECT_EQ(d_.active_count, 0U);
        if (wctx.frames.empty())
        {
            return {};
        }
        const auto resp = UnpackHeader(wctx.frames[0]);
        EXPECT_EQ(resp.stream_id, 3U);
        EXPECT_EQ(resp.opcode, static_cast<uint16_t>(opcode));
        return wctx.frames[0];
    }

    std::map<std::string, std::string> Get(const std::vector<uint8_t> &payload)
    {
        const auto frame =
            Request(taz_v1_Opcode_OPCODE_CONFIGURATION_GET, payload);
        std::map<std::string, std::string> out;
        taz_v1_ConfigurationGetResponse resp =
            taz_v1_ConfigurationGetResponse_init_zero;
        EXPECT_TRUE(IsResponse(frame));
        EXPECT_TRUE(DecodePayload(frame, taz_v1_ConfigurationGetResponse_fields,
                                  &resp));
        for (pb_size_t i = 0; i < resp.config_count; i++)
        {
            out[resp.config[i].key] = resp.config[i].value;
        }
        return out;
    }

    static bool IsResponse(const std::vector<uint8_t> &frame)
    {
        return frame.size() >= static_cast<size_t>(TAZ_FRAME_HEADER_SIZE) &&
               UnpackHeader(frame).type ==
                   static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_RESPONSE);
    }

    static bool DecodePayload(const std::vector<uint8_t> &frame,
                              const pb_msgdesc_t *fields, void *out)
    {
        if (frame.size() < static_cast<size_t>(TAZ_FRAME_HEADER_SIZE))
        {
            return false;
        }
        pb_istream_t stream = pb_istream_from_buffer(
            frame.data() + TAZ_FRAME_HEADER_SIZE,
            frame.size() - static_cast<size_t>(TAZ_FRAME_HEADER_SIZE));
        return pb_decode(&stream, fields, out);
    }

    static std::vector<uint8_t> Encode(const pb_msgdesc_t *fields,
                                       const void *msg)
    {
        std::vector<uint8_t> buf(4096U);
        pb_ostream_t stream = pb_ostream_from_buffer(buf.data(), buf.size());
        EXPECT_TRUE(pb_encode(&stream, fields, msg));
        buf.resize(stream.bytes_written);
        return buf;
    }

    static std::vector<uint8_t> GetRequest(const char *key)
    {
        taz_v1_ConfigurationGetRequest req =
            taz_v1_ConfigurationGetRequest_init_zero;
        req.keys_count = 1;
        std::strncpy(req.keys[0], key, sizeof(req.keys[0]) - 1U);
        return Encode(taz_v1_ConfigurationGetRequest_fields, &req);
    }

    static std::vector<uint8_t> UpdateRequest(const char *key,
                                              const char *value)
    {
        taz_v1_ConfigurationUpdateRequest req =
            taz_v1_ConfigurationUpdateRequest_init_zero;
        req.config_count = 1;
        std::strncpy(req.config[0].key, key, sizeof(req.config[0].key) - 1U);
        std::strncpy(req.config[0].value, value,
                     sizeof(req.config[0].value) - 1U);
        return Encode(taz_v1_ConfigurationUpdateRequest_fields, &req);
    }

    // Send UPDATE for one key; fill *resp from the RESPONSE frame.
    void Update(const char *key, const char *value,
                taz_v1_ConfigurationUpdateResponse *resp)
    {
        const auto frame = Request(taz_v1_Opcode_OPCODE_CONFIGURATION_UPDATE,
                                   UpdateRequest(key, value));
        ASSERT_TRUE(IsResponse(frame));
        ASSERT_TRUE(DecodePayload(
            frame, taz_v1_ConfigurationUpdateResponse_fields, resp));
    }

    static void ExpectInvalidRequest(const std::vector<uint8_t> &frame)
    {
        ASSERT_GE(frame.size(), static_cast<size_t>(TAZ_FRAME_HEADER_SIZE));
        EXPECT_EQ(UnpackHeader(frame).type,
                  static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_ERROR));
        taz_v1_ErrorInfo err = taz_v1_ErrorInfo_init_zero;
        ASSERT_TRUE(DecodeErrorInfo(frame, &err));
        EXPECT_EQ(err.code, taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST);
    }

  private:
    taz_dispatch_t d_{};
};

TEST_F(DispatchConfig, GetWithEmptyPayloadReturnsAllDefaults)
{
    const std::map<std::string, std::string> expected = {
        {"log.level", "INFO"},
        {"compression", "NONE"},
        {"exec.max_output_bytes", "1048576"},
    };
    EXPECT_EQ(Get({}), expected);
}

TEST_F(DispatchConfig, GetSubsetReturnsOnlyRequestedKey)
{
    const std::map<std::string, std::string> expected = {{"log.level", "INFO"}};
    EXPECT_EQ(Get(GetRequest("log.level")), expected);
}

TEST_F(DispatchConfig, UpdateAppliesValidKeyAndGetReflectsIt)
{
    taz_v1_ConfigurationUpdateResponse resp =
        taz_v1_ConfigurationUpdateResponse_init_zero;
    Update("log.level", "DEBUG", &resp);
    ASSERT_EQ(resp.applied_count, 1);
    EXPECT_STREQ(resp.applied[0], "log.level");
    EXPECT_EQ(resp.rejected_count, 0);

    const std::map<std::string, std::string> expected = {
        {"log.level", "DEBUG"}};
    EXPECT_EQ(Get(GetRequest("log.level")), expected);
}

TEST_F(DispatchConfig, UpdateRejectsUnknownKey)
{
    taz_v1_ConfigurationUpdateResponse resp =
        taz_v1_ConfigurationUpdateResponse_init_zero;
    Update("invalid.key", "x", &resp);
    EXPECT_EQ(resp.applied_count, 0);
    ASSERT_EQ(resp.rejected_count, 1);
    EXPECT_STREQ(resp.rejected[0].key, "invalid.key");
    EXPECT_GT(std::strlen(resp.rejected[0].reason), 0U);
}

TEST_F(DispatchConfig, MalformedGetProducesInvalidRequest)
{
    ExpectInvalidRequest(
        Request(taz_v1_Opcode_OPCODE_CONFIGURATION_GET, TruncatedRequest()));
}

TEST_F(DispatchConfig, MalformedUpdateProducesInvalidRequest)
{
    ExpectInvalidRequest(
        Request(taz_v1_Opcode_OPCODE_CONFIGURATION_UPDATE, TruncatedRequest()));
    // Nothing was applied.
    EXPECT_EQ(Get(GetRequest("log.level")).at("log.level"), "INFO");
}

} // namespace
