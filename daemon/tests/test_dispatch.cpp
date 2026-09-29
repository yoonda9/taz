// Tests for dispatch module: opcode routing, error responses.

#include <cstring>
#include <vector>

#include <gtest/gtest.h>
#include <pb_decode.h>

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

} // namespace
