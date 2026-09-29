// Tests for the connection-layer frame routing (taz_conn_handle_frame),
// including integration with the reassembly state machine.

#include <cstdint>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>
#include <pb_decode.h>

#include "taz/connection.h"
#include "taz/dispatch.h"
#include "taz/frame.h"
#include "taz/reassembly.h"
#include "taz/v1/common.pb.h"

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
// OK verdict (PING) is routed through dispatch → PONG
// ---------------------------------------------------------------------------

TEST(Connection, OkPingRouted)
{
    taz_dispatch_t d;
    taz_dispatch_init(&d);
    WriteCtx wctx;
    int close_out = -1;

    const taz_frame_header_t h = MakeHeader(
        static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_PING), 0U, 11U);
    taz_conn_handle_frame(&d, &h, nullptr, TAZ_FRAME_OK, capture_write, &wctx,
                          &close_out);

    EXPECT_EQ(close_out, 0);
    ASSERT_EQ(wctx.frames.size(), 1U);
    const auto resp = UnpackHeader(wctx.frames[0]);
    EXPECT_EQ(resp.type,
              static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_PONG));
    EXPECT_EQ(resp.stream_id, 11U);
}

// ---------------------------------------------------------------------------
// UNKNOWN_TYPE verdict → ERROR NOT_SUPPORTED, close_out = 0
// ---------------------------------------------------------------------------

TEST(Connection, UnknownTypeProducesNotSupported)
{
    taz_dispatch_t d;
    taz_dispatch_init(&d);
    WriteCtx wctx;
    int close_out = -1;

    const taz_frame_header_t h = MakeHeader(
        static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_UNSPECIFIED), 0U, 3U);
    taz_conn_handle_frame(&d, &h, nullptr, TAZ_FRAME_UNKNOWN_TYPE,
                          capture_write, &wctx, &close_out);

    EXPECT_EQ(close_out, 0);
    ASSERT_EQ(wctx.frames.size(), 1U);
    const auto resp = UnpackHeader(wctx.frames[0]);
    EXPECT_EQ(resp.type,
              static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_ERROR));
    EXPECT_EQ(resp.stream_id, 3U);

    taz_v1_ErrorInfo err = taz_v1_ErrorInfo_init_zero;
    ASSERT_TRUE(DecodeErrorInfo(wctx.frames[0], &err));
    EXPECT_EQ(err.code, taz_v1_ErrorCode_ERROR_CODE_NOT_SUPPORTED);
}

// ---------------------------------------------------------------------------
// OVERSIZED verdict → ERROR PROTOCOL_ERROR, close_out = 1
// ---------------------------------------------------------------------------

TEST(Connection, OversizedProducesProtocolErrorAndClose)
{
    taz_dispatch_t d;
    taz_dispatch_init(&d);
    WriteCtx wctx;
    int close_out = 0;

    const taz_frame_header_t h =
        MakeHeader(static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_REQUEST),
                   static_cast<uint16_t>(taz_v1_Opcode_OPCODE_VERSION), 7U,
                   static_cast<uint32_t>(TAZ_FRAME_MAX_PAYLOAD) + 1U);
    taz_conn_handle_frame(&d, &h, nullptr, TAZ_FRAME_OVERSIZED, capture_write,
                          &wctx, &close_out);

    EXPECT_EQ(close_out, 1);
    ASSERT_EQ(wctx.frames.size(), 1U);
    const auto resp = UnpackHeader(wctx.frames[0]);
    EXPECT_EQ(resp.type,
              static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_ERROR));
    EXPECT_EQ(resp.stream_id, 7U);

    taz_v1_ErrorInfo err = taz_v1_ErrorInfo_init_zero;
    ASSERT_TRUE(DecodeErrorInfo(wctx.frames[0], &err));
    EXPECT_EQ(err.code, taz_v1_ErrorCode_ERROR_CODE_PROTOCOL_ERROR);
}

// ---------------------------------------------------------------------------
// Integration: unknown-type frame then PING via reassembly
// → NOT_SUPPORTED then PONG (skip consumed exactly length bytes)
// ---------------------------------------------------------------------------

struct ReassemblyCtx
{
    taz_dispatch_t *dispatch;
    WriteCtx *wctx;
    int close_out;
};

void reassembly_on_frame(const taz_frame_header_t *header,
                         const uint8_t *payload, taz_frame_verdict_t verdict,
                         void *ctx_ptr)
{
    auto *rctx = static_cast<ReassemblyCtx *>(ctx_ptr);
    int frame_close = 0;
    taz_conn_handle_frame(rctx->dispatch, header, payload, verdict,
                          capture_write, rctx->wctx, &frame_close);
    if (frame_close != 0)
    {
        rctx->close_out = 1;
    }
}

TEST(Connection, UnknownTypeThenPingViaReassembly)
{
    taz_dispatch_t d;
    taz_dispatch_init(&d);
    taz_reassembly_state_t state{};
    taz_reassembly_init(&state);
    WriteCtx wctx;
    ReassemblyCtx rctx{&d, &wctx, 0};

    /* Unknown-type frame (type 0xFF, 4-byte payload) */
    const uint8_t unk_payload[] = {0x01U, 0x02U, 0x03U, 0x04U};
    uint8_t unk_buf[TAZ_FRAME_HEADER_SIZE + sizeof(unk_payload)];
    {
        taz_frame_header_t uh{};
        uh.type = 0xFFU;
        uh.flags = 0U;
        uh.opcode = 0U;
        uh.length = static_cast<uint32_t>(sizeof(unk_payload));
        uh.stream_id = 1U;
        taz_frame_pack_header(&uh, unk_buf);
        std::memcpy(unk_buf + TAZ_FRAME_HEADER_SIZE, unk_payload,
                    sizeof(unk_payload));
    }

    /* PING frame */
    uint8_t ping_buf[TAZ_FRAME_HEADER_SIZE];
    {
        taz_frame_header_t ph{};
        ph.type = static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_PING);
        ph.flags = 0U;
        ph.opcode = 0U;
        ph.length = 0U;
        ph.stream_id = 55U;
        taz_frame_pack_header(&ph, ping_buf);
    }

    /* Feed both frames as one burst */
    uint8_t combined[sizeof(unk_buf) + sizeof(ping_buf)];
    std::memcpy(combined, unk_buf, sizeof(unk_buf));
    std::memcpy(combined + sizeof(unk_buf), ping_buf, sizeof(ping_buf));
    taz_reassembly_feed(&state, combined, sizeof(combined), reassembly_on_frame,
                        &rctx);

    EXPECT_EQ(rctx.close_out, 0);
    ASSERT_EQ(wctx.frames.size(), 2U);

    const auto r0 = UnpackHeader(wctx.frames[0]);
    EXPECT_EQ(r0.type, static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_ERROR));
    EXPECT_EQ(r0.stream_id, 1U);
    taz_v1_ErrorInfo err0 = taz_v1_ErrorInfo_init_zero;
    ASSERT_TRUE(DecodeErrorInfo(wctx.frames[0], &err0));
    EXPECT_EQ(err0.code, taz_v1_ErrorCode_ERROR_CODE_NOT_SUPPORTED);

    const auto r1 = UnpackHeader(wctx.frames[1]);
    EXPECT_EQ(r1.type, static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_PONG));
    EXPECT_EQ(r1.stream_id, 55U);
}

// ---------------------------------------------------------------------------
// OVERSIZED via reassembly → PROTOCOL_ERROR and close
// ---------------------------------------------------------------------------

TEST(Connection, OversizedViaReassemblyProducesClose)
{
    taz_dispatch_t d;
    taz_dispatch_init(&d);
    taz_reassembly_state_t state{};
    taz_reassembly_init(&state);
    WriteCtx wctx;
    ReassemblyCtx rctx{&d, &wctx, 0};

    /* PING with length=1 is oversized */
    uint8_t buf[TAZ_FRAME_HEADER_SIZE];
    {
        taz_frame_header_t h{};
        h.type = static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_PING);
        h.flags = 0U;
        h.opcode = 0U;
        h.length = 1U;
        h.stream_id = 77U;
        taz_frame_pack_header(&h, buf);
    }

    taz_reassembly_feed(&state, buf, sizeof(buf), reassembly_on_frame, &rctx);

    EXPECT_EQ(rctx.close_out, 1);
    ASSERT_EQ(wctx.frames.size(), 1U);
    const auto resp = UnpackHeader(wctx.frames[0]);
    EXPECT_EQ(resp.type,
              static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_ERROR));
    EXPECT_EQ(resp.stream_id, 77U);
    taz_v1_ErrorInfo err = taz_v1_ErrorInfo_init_zero;
    ASSERT_TRUE(DecodeErrorInfo(wctx.frames[0], &err));
    EXPECT_EQ(err.code, taz_v1_ErrorCode_ERROR_CODE_PROTOCOL_ERROR);
}

} // namespace
