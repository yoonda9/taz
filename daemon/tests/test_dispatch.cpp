// Tests for dispatch module: opcode routing, error responses.

#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <pb_decode.h>
#include <pb_encode.h>
#include <uv.h>

#include "taz/config.h"
#include "taz/dispatch.h"
#include "taz/exec.h"
#include "taz/frame.h"
#include "taz/v1/advanced.pb.h"
#include "taz/v1/command.pb.h"
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

std::string SelfExePath()
{
    char buf[4096];
    size_t size = sizeof(buf);
    if (uv_exepath(buf, &size) != 0)
    {
        return std::string();
    }
    return std::string(buf, size);
}

std::vector<uint8_t>
CommandExecRequestBytes(const std::string &command,
                        const std::vector<std::string> &args = {},
                        const char *as_user = "")
{
    // Heap-allocated: the real struct is ~40 KiB (see test_command.cpp).
    auto req = std::make_unique<taz_v1_CommandExecRequest>();
    (void)std::strncpy(req->command, command.c_str(),
                       sizeof(req->command) - 1U);
    req->args_count = static_cast<pb_size_t>(args.size());
    for (size_t i = 0U; i < args.size(); i++)
    {
        (void)std::strncpy(req->args[i], args[i].c_str(),
                           sizeof(req->args[i]) - 1U);
    }
    (void)std::strncpy(req->as_user, as_user, sizeof(req->as_user) - 1U);

    std::vector<uint8_t> buf(taz_v1_CommandExecRequest_size);
    pb_ostream_t ostream = pb_ostream_from_buffer(buf.data(), buf.size());
    EXPECT_TRUE(
        pb_encode(&ostream, taz_v1_CommandExecRequest_fields, req.get()));
    buf.resize(ostream.bytes_written);
    return buf;
}

bool CollectBytesCb(pb_istream_t *stream, const pb_field_iter_t * /*f*/,
                    void **arg)
{
    auto *out = static_cast<std::string *>(*arg);
    uint8_t buf[256];
    while (stream->bytes_left > 0U)
    {
        const size_t n =
            stream->bytes_left < sizeof(buf) ? stream->bytes_left : sizeof(buf);
        if (!pb_read(stream, buf, n))
        {
            return false;
        }
        out->append(reinterpret_cast<const char *>(buf), n);
    }
    return true;
}

bool DecodeExecResponse(const std::vector<uint8_t> &frame,
                        taz_v1_CommandExecResponse *out,
                        std::string *stdout_out)
{
    if (frame.size() < static_cast<size_t>(TAZ_FRAME_HEADER_SIZE))
    {
        return false;
    }
    out->stdout_data.funcs.decode = CollectBytesCb;
    out->stdout_data.arg = stdout_out;
    pb_istream_t stream = pb_istream_from_buffer(
        frame.data() + TAZ_FRAME_HEADER_SIZE,
        frame.size() - static_cast<size_t>(TAZ_FRAME_HEADER_SIZE));
    return pb_decode(&stream, taz_v1_CommandExecResponse_fields, out);
}

std::vector<uint8_t> CancelRequestBytes(uint32_t target_stream_id)
{
    taz_v1_CancelRequest req = taz_v1_CancelRequest_init_zero;
    req.target_stream_id = target_stream_id;
    std::vector<uint8_t> buf(taz_v1_CancelRequest_size);
    pb_ostream_t ostream = pb_ostream_from_buffer(buf.data(), buf.size());
    EXPECT_TRUE(pb_encode(&ostream, taz_v1_CancelRequest_fields, &req));
    buf.resize(ostream.bytes_written);
    return buf;
}

bool DecodeCancelResponse(const std::vector<uint8_t> &frame,
                          taz_v1_CancelResponse *out)
{
    if (frame.size() < static_cast<size_t>(TAZ_FRAME_HEADER_SIZE))
    {
        return false;
    }
    pb_istream_t stream = pb_istream_from_buffer(
        frame.data() + TAZ_FRAME_HEADER_SIZE,
        frame.size() - static_cast<size_t>(TAZ_FRAME_HEADER_SIZE));
    return pb_decode(&stream, taz_v1_CancelResponse_fields, out);
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
// Async dispatch: loop requirement, stream lifetime, cancel_all
// ---------------------------------------------------------------------------

TEST(Dispatch, CommandExecOpcodeIsMarkedAsync)
{
    EXPECT_TRUE(taz_dispatch_opcode_is_async(
        static_cast<uint16_t>(taz_v1_Opcode_OPCODE_COMMAND_EXEC)));
    EXPECT_FALSE(taz_dispatch_opcode_is_async(
        static_cast<uint16_t>(taz_v1_Opcode_OPCODE_VERSION)));
    EXPECT_FALSE(taz_dispatch_opcode_is_async(0xFFFFU));
}

TEST(Dispatch, CommandExecWithoutLoopProducesInternalErrorAndStaysClosed)
{
    taz_dispatch_t d;
    taz_dispatch_init(&d); // d.loop == NULL: a pure unit-test dispatch.
    WriteCtx wctx;

    const taz_frame_header_t h = MakeHeader(
        static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_REQUEST),
        static_cast<uint16_t>(taz_v1_Opcode_OPCODE_COMMAND_EXEC), 11U);
    taz_dispatch_frame(&d, &h, nullptr, TAZ_FRAME_OK, capture_write, &wctx);

    ASSERT_EQ(wctx.frames.size(), 1U);
    const auto &frame = wctx.frames[0];
    const auto resp = UnpackHeader(frame);
    EXPECT_EQ(resp.type,
              static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_ERROR));
    EXPECT_EQ(resp.stream_id, 11U);

    taz_v1_ErrorInfo err = taz_v1_ErrorInfo_init_zero;
    ASSERT_TRUE(DecodeErrorInfo(frame, &err));
    EXPECT_EQ(err.code, taz_v1_ErrorCode_ERROR_CODE_INTERNAL);

    // Rejected before the stream was ever opened.
    EXPECT_EQ(d.active_count, 0U);
}

TEST(Dispatch, CommandExecRunsRealProcessThroughDispatchFrame)
{
    uv_loop_t loop;
    ASSERT_EQ(uv_loop_init(&loop), 0);

    taz_dispatch_t d;
    taz_dispatch_init(&d);
    d.loop = &loop;
    WriteCtx wctx;

    const std::string exe = SelfExePath();
    ASSERT_FALSE(exe.empty());
    const auto payload =
        CommandExecRequestBytes(exe, {"--gtest_filter=NoSuchSuite.*"});

    const taz_frame_header_t h =
        MakeHeader(static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_REQUEST),
                   static_cast<uint16_t>(taz_v1_Opcode_OPCODE_COMMAND_EXEC),
                   13U, static_cast<uint32_t>(payload.size()));
    taz_dispatch_frame(&d, &h, payload.data(), TAZ_FRAME_OK, capture_write,
                       &wctx);

    // The handler returns once the process is spawned, well before it has
    // exited: no frame written yet, and the stream stays open.
    EXPECT_TRUE(wctx.frames.empty());
    EXPECT_EQ(d.active_count, 1U);

    ASSERT_EQ(uv_run(&loop, UV_RUN_DEFAULT), 0);
    ASSERT_EQ(uv_loop_close(&loop), 0);

    // on_done fired: exactly one RESPONSE, and the stream closed itself.
    EXPECT_EQ(d.active_count, 0U);
    ASSERT_EQ(wctx.frames.size(), 1U);
    const auto &frame = wctx.frames[0];
    const auto resp = UnpackHeader(frame);
    EXPECT_EQ(resp.type,
              static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_RESPONSE));
    EXPECT_EQ(resp.stream_id, 13U);

    taz_v1_CommandExecResponse decoded = taz_v1_CommandExecResponse_init_zero;
    std::string stdout_bytes;
    ASSERT_TRUE(DecodeExecResponse(frame, &decoded, &stdout_bytes));
    EXPECT_EQ(decoded.exit_code, 0);
    EXPECT_FALSE(decoded.timed_out);
    EXPECT_FALSE(stdout_bytes.empty());
}

TEST(Dispatch, CommandExecWithAsUserIsNotSupported)
{
    uv_loop_t loop;
    ASSERT_EQ(uv_loop_init(&loop), 0);

    taz_dispatch_t d;
    taz_dispatch_init(&d);
    d.loop = &loop;
    WriteCtx wctx;

    const auto payload = CommandExecRequestBytes(SelfExePath(), {}, "someone");

    const taz_frame_header_t h =
        MakeHeader(static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_REQUEST),
                   static_cast<uint16_t>(taz_v1_Opcode_OPCODE_COMMAND_EXEC),
                   21U, static_cast<uint32_t>(payload.size()));
    taz_dispatch_frame(&d, &h, payload.data(), TAZ_FRAME_OK, capture_write,
                       &wctx);

    // Rejected synchronously, before any process is spawned.
    ASSERT_EQ(wctx.frames.size(), 1U);
    const auto &frame = wctx.frames[0];
    const auto resp = UnpackHeader(frame);
    EXPECT_EQ(resp.type,
              static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_ERROR));
    EXPECT_EQ(resp.stream_id, 21U);

    taz_v1_ErrorInfo err = taz_v1_ErrorInfo_init_zero;
    ASSERT_TRUE(DecodeErrorInfo(frame, &err));
    EXPECT_EQ(err.code, taz_v1_ErrorCode_ERROR_CODE_NOT_SUPPORTED);
    EXPECT_EQ(d.active_count, 0U);

    ASSERT_EQ(uv_run(&loop, UV_RUN_DEFAULT), 0);
    ASSERT_EQ(uv_loop_close(&loop), 0);
}

TEST(Dispatch, CommandExecNonexistentCommandMapsToNotFound)
{
    uv_loop_t loop;
    ASSERT_EQ(uv_loop_init(&loop), 0);

    taz_dispatch_t d;
    taz_dispatch_init(&d);
    d.loop = &loop;
    WriteCtx wctx;

    const auto payload =
        CommandExecRequestBytes("/no/such/taz-test-binary-xyz", {});

    const taz_frame_header_t h =
        MakeHeader(static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_REQUEST),
                   static_cast<uint16_t>(taz_v1_Opcode_OPCODE_COMMAND_EXEC),
                   27U, static_cast<uint32_t>(payload.size()));
    taz_dispatch_frame(&d, &h, payload.data(), TAZ_FRAME_OK, capture_write,
                       &wctx);

    // uv_spawn fails synchronously for a missing executable.
    ASSERT_EQ(wctx.frames.size(), 1U);
    const auto &frame = wctx.frames[0];
    const auto resp = UnpackHeader(frame);
    EXPECT_EQ(resp.type,
              static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_ERROR));
    EXPECT_EQ(resp.stream_id, 27U);

    taz_v1_ErrorInfo err = taz_v1_ErrorInfo_init_zero;
    ASSERT_TRUE(DecodeErrorInfo(frame, &err));
    EXPECT_EQ(err.code, taz_v1_ErrorCode_ERROR_CODE_NOT_FOUND);
    EXPECT_EQ(d.active_count, 0U);

    ASSERT_EQ(uv_run(&loop, UV_RUN_DEFAULT), 0);
    ASSERT_EQ(uv_loop_close(&loop), 0);
}

TEST(Dispatch, CommandExecEmptyPayloadIsInvalidRequest)
{
    taz_dispatch_t d;
    taz_dispatch_init(&d);
    WriteCtx wctx;

    uv_loop_t loop;
    ASSERT_EQ(uv_loop_init(&loop), 0);
    d.loop = &loop;

    const taz_frame_header_t h = MakeHeader(
        static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_REQUEST),
        static_cast<uint16_t>(taz_v1_Opcode_OPCODE_COMMAND_EXEC), 33U);
    taz_dispatch_frame(&d, &h, nullptr, TAZ_FRAME_OK, capture_write, &wctx);

    ASSERT_EQ(wctx.frames.size(), 1U);
    const auto &frame = wctx.frames[0];
    const auto resp = UnpackHeader(frame);
    EXPECT_EQ(resp.type,
              static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_ERROR));

    taz_v1_ErrorInfo err = taz_v1_ErrorInfo_init_zero;
    ASSERT_TRUE(DecodeErrorInfo(frame, &err));
    EXPECT_EQ(err.code, taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST);
    EXPECT_EQ(d.active_count, 0U);

    ASSERT_EQ(uv_run(&loop, UV_RUN_DEFAULT), 0);
    ASSERT_EQ(uv_loop_close(&loop), 0);
}

TEST(Dispatch, StreamDoneFreesStreamIdForReuse)
{
    taz_dispatch_t d;
    taz_dispatch_init(&d);
    WriteCtx wctx;

    // Simulate an async handler's stream having just completed.
    const uint32_t sid = 42U;
    d.active_streams[0] = sid;
    d.active_count = 1U;
    taz_dispatch_stream_done(&d, sid);
    EXPECT_EQ(d.active_count, 0U);

    // The id is immediately usable again by an ordinary request.
    const taz_frame_header_t h =
        MakeHeader(static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_REQUEST),
                   static_cast<uint16_t>(taz_v1_Opcode_OPCODE_VERSION), sid);
    taz_dispatch_frame(&d, &h, nullptr, TAZ_FRAME_OK, capture_write, &wctx);

    ASSERT_EQ(wctx.frames.size(), 1U);
    EXPECT_EQ(UnpackHeader(wctx.frames[0]).type,
              static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_RESPONSE));
    EXPECT_EQ(d.active_count, 0U);
}

TEST(Dispatch, SetStreamExecIsNoOpWhenStreamNotActive)
{
    taz_dispatch_t d;
    taz_dispatch_init(&d);

    // No stream is active, so this must not create one or dereference the
    // bogus pointer; cancel_all must then have nothing to walk.
    taz_dispatch_set_stream_exec(&d, 99U, reinterpret_cast<taz_exec_t *>(1));
    EXPECT_EQ(d.active_count, 0U);
    taz_dispatch_cancel_all(&d);
}

void RecordCancelled(const taz_exec_result_t *result, void *arg)
{
    auto *cancelled = static_cast<bool *>(arg);
    *cancelled = result->cancelled;
}

TEST(Dispatch, CancelAllCancelsEveryRegisteredExec)
{
    uv_loop_t loop;
    ASSERT_EQ(uv_loop_init(&loop), 0);

    taz_dispatch_t d;
    taz_dispatch_init(&d);
    d.loop = &loop;

    taz_exec_spec_t spec{};
    spec.file = TAZ_TEST_SLEEPER_PATH;
    spec.max_output_bytes = 1024;

    bool cancelled = false;
    taz_exec_t *x = nullptr;
    ASSERT_EQ(taz_exec_start(&loop, &spec, RecordCancelled, &cancelled, &x), 0);

    // Simulate what an async handler does once taz_exec_start succeeds:
    // open the stream, then register the exec against it.
    d.active_streams[0] = 7U;
    d.active_count = 1U;
    taz_dispatch_set_stream_exec(&d, 7U, x);

    taz_dispatch_cancel_all(&d);

    ASSERT_EQ(uv_run(&loop, UV_RUN_DEFAULT), 0);
    ASSERT_EQ(uv_loop_close(&loop), 0);

    EXPECT_TRUE(cancelled);
}

// ---------------------------------------------------------------------------
// taz_dispatch_conn_ref/_unref: connection-lifetime hooks for an in-flight
// exec (step-03:conn-lifetime). connection.c wires these to the real
// taz_conn_t; here they are exercised directly against a plain counter.
// ---------------------------------------------------------------------------

void CountRef(void *ctx)
{
    *static_cast<int *>(ctx) += 1;
}

void CountUnref(void *ctx)
{
    *static_cast<int *>(ctx) -= 1;
}

TEST(Dispatch, ConnRefUnrefNoOpWhenUnset)
{
    taz_dispatch_t d;
    taz_dispatch_init(
        &d); // conn_ref/conn_unref NULL: a pure unit-test dispatch.

    // Must not crash or dereference the NULL conn_ctx.
    taz_dispatch_conn_ref(&d);
    taz_dispatch_conn_unref(&d);
}

TEST(Dispatch, ConnRefUnrefCallThroughWithCtx)
{
    taz_dispatch_t d;
    taz_dispatch_init(&d);
    int counter = 0;
    d.conn_ref = CountRef;
    d.conn_unref = CountUnref;
    d.conn_ctx = &counter;

    taz_dispatch_conn_ref(&d);
    taz_dispatch_conn_ref(&d);
    EXPECT_EQ(counter, 2);
    taz_dispatch_conn_unref(&d);
    EXPECT_EQ(counter, 1);
}

TEST(Dispatch, CommandExecRefsConnWhileInFlightAndUnrefsOnDone)
{
    uv_loop_t loop;
    ASSERT_EQ(uv_loop_init(&loop), 0);

    taz_dispatch_t d;
    taz_dispatch_init(&d);
    d.loop = &loop;
    int counter = 0;
    d.conn_ref = CountRef;
    d.conn_unref = CountUnref;
    d.conn_ctx = &counter;
    WriteCtx wctx;

    const std::string exe = SelfExePath();
    ASSERT_FALSE(exe.empty());
    const auto payload =
        CommandExecRequestBytes(exe, {"--gtest_filter=NoSuchSuite.*"});

    const taz_frame_header_t h =
        MakeHeader(static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_REQUEST),
                   static_cast<uint16_t>(taz_v1_Opcode_OPCODE_COMMAND_EXEC),
                   51U, static_cast<uint32_t>(payload.size()));
    taz_dispatch_frame(&d, &h, payload.data(), TAZ_FRAME_OK, capture_write,
                       &wctx);

    // handle_command_exec refs the connection right after the spawn
    // succeeds, before the process has had any chance to exit.
    EXPECT_EQ(counter, 1);

    ASSERT_EQ(uv_run(&loop, UV_RUN_DEFAULT), 0);
    ASSERT_EQ(uv_loop_close(&loop), 0);

    // on_exec_done unrefs exactly once, after the response is sent and the
    // stream is closed.
    EXPECT_EQ(counter, 0);
    EXPECT_EQ(d.active_count, 0U);
    ASSERT_EQ(wctx.frames.size(), 1U);
}

TEST(Dispatch, FileChunkOnCommandExecStreamIsDroppedAndExecStillCompletes)
{
    uv_loop_t loop;
    ASSERT_EQ(uv_loop_init(&loop), 0);

    taz_dispatch_t d;
    taz_dispatch_init(&d);
    d.loop = &loop;
    WriteCtx wctx;

    const std::string exe = SelfExePath();
    ASSERT_FALSE(exe.empty());
    const auto payload =
        CommandExecRequestBytes(exe, {"--gtest_filter=NoSuchSuite.*"});

    const taz_frame_header_t h =
        MakeHeader(static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_REQUEST),
                   static_cast<uint16_t>(taz_v1_Opcode_OPCODE_COMMAND_EXEC),
                   54U, static_cast<uint32_t>(payload.size()));
    taz_dispatch_frame(&d, &h, payload.data(), TAZ_FRAME_OK, capture_write,
                       &wctx);
    ASSERT_EQ(d.active_count, 1U);

    // handlers/command.c never calls taz_dispatch_set_stream_ops, so a
    // FILE_CHUNK for its stream has no sink: dropped silently per
    // taz_dispatch_frame's routing rule, not routed anywhere, and the
    // in-flight exec is unaffected.
    const taz_frame_header_t chunk_h =
        MakeHeader(static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_FILE_CHUNK),
                   0U, 54U, 3U);
    const std::vector<uint8_t> chunk_payload{'a', 'b', 'c'};
    taz_dispatch_frame(&d, &chunk_h, chunk_payload.data(), TAZ_FRAME_OK,
                       capture_write, &wctx);

    EXPECT_TRUE(wctx.frames.empty());
    EXPECT_EQ(d.active_count, 1U);

    ASSERT_EQ(uv_run(&loop, UV_RUN_DEFAULT), 0);
    ASSERT_EQ(uv_loop_close(&loop), 0);

    EXPECT_EQ(d.active_count, 0U);
    ASSERT_EQ(wctx.frames.size(), 1U);
}

TEST(Dispatch, CommandExecSpawnFailureDoesNotRefConn)
{
    uv_loop_t loop;
    ASSERT_EQ(uv_loop_init(&loop), 0);

    taz_dispatch_t d;
    taz_dispatch_init(&d);
    d.loop = &loop;
    int counter = 0;
    d.conn_ref = CountRef;
    d.conn_unref = CountUnref;
    d.conn_ctx = &counter;
    WriteCtx wctx;

    const auto payload =
        CommandExecRequestBytes("/no/such/taz-test-binary-xyz", {});

    const taz_frame_header_t h =
        MakeHeader(static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_REQUEST),
                   static_cast<uint16_t>(taz_v1_Opcode_OPCODE_COMMAND_EXEC),
                   52U, static_cast<uint32_t>(payload.size()));
    taz_dispatch_frame(&d, &h, payload.data(), TAZ_FRAME_OK, capture_write,
                       &wctx);

    // uv_spawn fails synchronously: no exec ever started, so no ref taken.
    EXPECT_EQ(counter, 0);

    ASSERT_EQ(uv_run(&loop, UV_RUN_DEFAULT), 0);
    ASSERT_EQ(uv_loop_close(&loop), 0);
}

TEST(Dispatch, CancelAllUnrefsConnViaOnDoneAfterCancellation)
{
    uv_loop_t loop;
    ASSERT_EQ(uv_loop_init(&loop), 0);

    taz_dispatch_t d;
    taz_dispatch_init(&d);
    d.loop = &loop;
    int counter = 0;
    d.conn_ref = CountRef;
    d.conn_unref = CountUnref;
    d.conn_ctx = &counter;
    WriteCtx wctx;

    // A real COMMAND_EXEC request for the long-lived sleeper helper, routed
    // through taz_dispatch_frame exactly as handle_command_exec sees it in
    // production.
    const auto payload = CommandExecRequestBytes(TAZ_TEST_SLEEPER_PATH, {});
    const taz_frame_header_t h =
        MakeHeader(static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_REQUEST),
                   static_cast<uint16_t>(taz_v1_Opcode_OPCODE_COMMAND_EXEC),
                   53U, static_cast<uint32_t>(payload.size()));
    taz_dispatch_frame(&d, &h, payload.data(), TAZ_FRAME_OK, capture_write,
                       &wctx);

    EXPECT_EQ(counter, 1);
    EXPECT_EQ(d.active_count, 1U);

    // Mirrors conn_close(): connection.c cancels every in-flight exec
    // before tearing the TCP handle down.
    taz_dispatch_cancel_all(&d);

    ASSERT_EQ(uv_run(&loop, UV_RUN_DEFAULT), 0);
    ASSERT_EQ(uv_loop_close(&loop), 0);

    // on_done still fired exactly once for the cancelled exec, closing the
    // stream and releasing the connection reference the real handler took.
    EXPECT_EQ(d.active_count, 0U);
    EXPECT_EQ(counter, 0);
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
// taz_stream_ops_t: FILE_CHUNK routing, set_stream_ops, cancel_stream,
// cancel_all abort, notify_writable, conn_write_queue_size/pause/resume
// ---------------------------------------------------------------------------

namespace
{

struct ChunkSink
{
    bool called = false;
    bool payload_was_null = false;
    taz_frame_header_t header{};
    std::vector<uint8_t> payload;
};

void RecordChunk(void *user, const taz_frame_header_t *header,
                 const uint8_t *payload)
{
    auto *sink = static_cast<ChunkSink *>(user);
    sink->called = true;
    sink->header = *header;
    if (payload == nullptr)
    {
        sink->payload_was_null = true;
    }
    else
    {
        sink->payload.assign(payload, payload + header->length);
    }
}

int RefuseCancel(void * /*user*/, taz_stream_done_fn_t /*done*/,
                 void * /*done_arg*/)
{
    return 0;
}

void NoopAbort(void * /*user*/)
{
}

taz_frame_header_t MakeFileChunkHeader(uint32_t stream_id, uint32_t length,
                                       uint8_t flags = 0U)
{
    taz_frame_header_t h =
        MakeHeader(static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_FILE_CHUNK),
                   0U, stream_id, length);
    h.flags = flags;
    return h;
}

} // namespace

TEST(Dispatch, FileChunkRoutesToRegisteredSink)
{
    taz_dispatch_t d;
    taz_dispatch_init(&d);
    d.active_streams[0] = 5U;
    d.active_count = 1U;

    ChunkSink sink;
    taz_stream_ops_t ops{};
    ops.on_chunk = RecordChunk;
    ops.cancel = RefuseCancel;
    ops.abort = NoopAbort;
    taz_dispatch_set_stream_ops(&d, 5U, &ops, &sink);

    const std::vector<uint8_t> bytes = {1U, 2U, 3U, 4U};
    const auto h = MakeFileChunkHeader(
        5U, static_cast<uint32_t>(bytes.size()),
        static_cast<uint8_t>(taz_v1_FrameFlag_FRAME_FLAG_CONTINUATION));
    WriteCtx wctx;
    taz_dispatch_frame(&d, &h, bytes.data(), TAZ_FRAME_OK, capture_write,
                       &wctx);

    EXPECT_TRUE(sink.called);
    EXPECT_FALSE(sink.payload_was_null);
    EXPECT_EQ(sink.payload, bytes);
    EXPECT_EQ(sink.header.flags, h.flags);
    EXPECT_EQ(sink.header.length, h.length);
    EXPECT_EQ(sink.header.stream_id, 5U);
    EXPECT_TRUE(wctx.frames.empty());
    EXPECT_EQ(d.active_count, 1U);
}

TEST(Dispatch, FileChunkWithZeroLengthPassesNullPayload)
{
    taz_dispatch_t d;
    taz_dispatch_init(&d);
    d.active_streams[0] = 5U;
    d.active_count = 1U;

    ChunkSink sink;
    taz_stream_ops_t ops{};
    ops.on_chunk = RecordChunk;
    ops.cancel = RefuseCancel;
    ops.abort = NoopAbort;
    taz_dispatch_set_stream_ops(&d, 5U, &ops, &sink);

    // Non-null payload pointer with length == 0: dispatch itself must null
    // it out, not merely forward whatever the caller passed.
    const uint8_t bogus = 0xAAU;
    const auto h = MakeFileChunkHeader(5U, 0U);
    WriteCtx wctx;
    taz_dispatch_frame(&d, &h, &bogus, TAZ_FRAME_OK, capture_write, &wctx);

    EXPECT_TRUE(sink.called);
    EXPECT_TRUE(sink.payload_was_null);
}

TEST(Dispatch, FileChunkForUnknownStreamIsDroppedSilently)
{
    taz_dispatch_t d;
    taz_dispatch_init(&d);
    WriteCtx wctx;

    const auto h = MakeFileChunkHeader(99U, 4U);
    const std::vector<uint8_t> bytes = {1U, 2U, 3U, 4U};
    taz_dispatch_frame(&d, &h, bytes.data(), TAZ_FRAME_OK, capture_write,
                       &wctx);

    EXPECT_TRUE(wctx.frames.empty());
    EXPECT_EQ(d.active_count, 0U);
}

TEST(Dispatch, FileChunkForActiveStreamWithoutOpsIsDroppedSilently)
{
    taz_dispatch_t d;
    taz_dispatch_init(&d);
    d.active_streams[0] = 5U;
    d.active_count = 1U;
    WriteCtx wctx;

    const auto h = MakeFileChunkHeader(5U, 4U);
    const std::vector<uint8_t> bytes = {1U, 2U, 3U, 4U};
    taz_dispatch_frame(&d, &h, bytes.data(), TAZ_FRAME_OK, capture_write,
                       &wctx);

    EXPECT_TRUE(wctx.frames.empty());
    EXPECT_EQ(d.active_count, 1U);
}

TEST(Dispatch, SetStreamOpsIsNoOpWhenStreamNotActive)
{
    taz_dispatch_t d;
    taz_dispatch_init(&d);

    ChunkSink sink;
    taz_stream_ops_t ops{};
    ops.on_chunk = RecordChunk;
    ops.cancel = RefuseCancel;
    ops.abort = NoopAbort;
    // No stream is active: must not create one or crash.
    taz_dispatch_set_stream_ops(&d, 42U, &ops, &sink);
    EXPECT_EQ(d.active_count, 0U);

    WriteCtx wctx;
    const auto h = MakeFileChunkHeader(42U, 1U);
    const uint8_t byte = 1U;
    taz_dispatch_frame(&d, &h, &byte, TAZ_FRAME_OK, capture_write, &wctx);
    EXPECT_FALSE(sink.called);
}

TEST(Dispatch, SwapRemoveKeepsStreamOpsAligned)
{
    taz_dispatch_t d;
    taz_dispatch_init(&d);
    d.active_streams[0] = 10U;
    d.active_streams[1] = 11U;
    d.active_streams[2] = 12U;
    d.active_count = 3U;

    ChunkSink sink10;
    ChunkSink sink11;
    ChunkSink sink12;
    taz_stream_ops_t ops10{};
    ops10.on_chunk = RecordChunk;
    ops10.cancel = RefuseCancel;
    ops10.abort = NoopAbort;
    const taz_stream_ops_t ops11 = ops10;
    const taz_stream_ops_t ops12 = ops10;
    taz_dispatch_set_stream_ops(&d, 10U, &ops10, &sink10);
    taz_dispatch_set_stream_ops(&d, 11U, &ops11, &sink11);
    taz_dispatch_set_stream_ops(&d, 12U, &ops12, &sink12);

    // Swap-remove moves the last active entry (12) into slot 0.
    taz_dispatch_stream_done(&d, 10U);
    ASSERT_EQ(d.active_count, 2U);

    WriteCtx wctx;
    const uint8_t byte = 7U;
    auto h11 = MakeFileChunkHeader(11U, 1U);
    taz_dispatch_frame(&d, &h11, &byte, TAZ_FRAME_OK, capture_write, &wctx);
    auto h12 = MakeFileChunkHeader(12U, 1U);
    taz_dispatch_frame(&d, &h12, &byte, TAZ_FRAME_OK, capture_write, &wctx);

    EXPECT_TRUE(sink11.called);
    EXPECT_TRUE(sink12.called);
    EXPECT_FALSE(sink10.called);
}

void MarkDone(void *arg)
{
    *static_cast<bool *>(arg) = true;
}

TEST(Dispatch, CancelStreamInactiveReturnsZeroAndDoesNotCallDone)
{
    taz_dispatch_t d;
    taz_dispatch_init(&d);

    bool done_called = false;
    EXPECT_EQ(taz_dispatch_cancel_stream(&d, 123U, MarkDone, &done_called), 0);
    EXPECT_FALSE(done_called);
}

TEST(Dispatch, CancelStreamOnActiveStreamWithoutOpsReturnsZero)
{
    taz_dispatch_t d;
    taz_dispatch_init(&d);
    d.active_streams[0] = 5U;
    d.active_count = 1U;

    bool done_called = false;
    EXPECT_EQ(taz_dispatch_cancel_stream(&d, 5U, MarkDone, &done_called), 0);
    EXPECT_FALSE(done_called);
}

struct CancelSpy
{
    int calls = 0;
    int accept_result = 1;
};

int SpyCancelAccept(void *user, taz_stream_done_fn_t done, void *done_arg)
{
    auto *spy = static_cast<CancelSpy *>(user);
    spy->calls++;
    done(done_arg);
    return 1;
}

int SpyCancelRefuse(void *user, taz_stream_done_fn_t /*done*/,
                    void * /*done_arg*/)
{
    auto *spy = static_cast<CancelSpy *>(user);
    spy->calls++;
    return 0;
}

TEST(Dispatch, CancelStreamRefusedReturnsZeroAndDoneNeverCalled)
{
    taz_dispatch_t d;
    taz_dispatch_init(&d);
    d.active_streams[0] = 5U;
    d.active_count = 1U;

    CancelSpy spy;
    taz_stream_ops_t ops{};
    ops.on_chunk = RecordChunk;
    ops.cancel = SpyCancelRefuse;
    ops.abort = NoopAbort;
    taz_dispatch_set_stream_ops(&d, 5U, &ops, &spy);

    bool done_called = false;
    EXPECT_EQ(taz_dispatch_cancel_stream(&d, 5U, MarkDone, &done_called), 0);
    EXPECT_EQ(spy.calls, 1);
    EXPECT_FALSE(done_called);
}

TEST(Dispatch, CancelStreamAcceptedReturnsOneAndCallsDoneExactlyOnce)
{
    taz_dispatch_t d;
    taz_dispatch_init(&d);
    d.active_streams[0] = 5U;
    d.active_count = 1U;

    CancelSpy spy;
    taz_stream_ops_t ops{};
    ops.on_chunk = RecordChunk;
    ops.cancel = SpyCancelAccept;
    ops.abort = NoopAbort;
    taz_dispatch_set_stream_ops(&d, 5U, &ops, &spy);

    bool done_called = false;
    EXPECT_EQ(taz_dispatch_cancel_stream(&d, 5U, MarkDone, &done_called), 1);
    EXPECT_EQ(spy.calls, 1);
    EXPECT_TRUE(done_called);
}

struct AbortSpy
{
    int calls = 0;
};

void SpyAbort(void *user)
{
    static_cast<AbortSpy *>(user)->calls++;
}

TEST(Dispatch, CancelAllAbortsEveryOpsStreamAndStillCancelsExecs)
{
    uv_loop_t loop;
    ASSERT_EQ(uv_loop_init(&loop), 0);

    taz_dispatch_t d;
    taz_dispatch_init(&d);
    d.loop = &loop;

    taz_exec_spec_t spec{};
    spec.file = TAZ_TEST_SLEEPER_PATH;
    spec.max_output_bytes = 1024;

    bool cancelled = false;
    taz_exec_t *x = nullptr;
    ASSERT_EQ(taz_exec_start(&loop, &spec, RecordCancelled, &cancelled, &x), 0);

    d.active_streams[0] = 7U;
    d.active_count = 1U;
    taz_dispatch_set_stream_exec(&d, 7U, x);

    d.active_streams[1] = 8U;
    d.active_count = 2U;
    AbortSpy abort_spy;
    taz_stream_ops_t ops{};
    ops.on_chunk = RecordChunk;
    ops.cancel = RefuseCancel;
    ops.abort = SpyAbort;
    taz_dispatch_set_stream_ops(&d, 8U, &ops, &abort_spy);

    taz_dispatch_cancel_all(&d);

    ASSERT_EQ(uv_run(&loop, UV_RUN_DEFAULT), 0);
    ASSERT_EQ(uv_loop_close(&loop), 0);

    EXPECT_TRUE(cancelled);
    EXPECT_EQ(abort_spy.calls, 1);
}

// ---------------------------------------------------------------------------
// CANCEL opcode (handlers/cancel.c): decode, target_stream_id == 0 / own
// stream, and dispatch-level refusal reasons (unknown target, a stream with
// no ops such as COMMAND_EXEC). The "accept" path (target with ops that
// agrees to cancel) is exercised end to end against handlers/file_transfer.c
// in test_file_transfer.cpp, where a real FILE_PUT transfer is available to
// target.
// ---------------------------------------------------------------------------

TEST(Dispatch, CancelOpcodeIsMarkedAsync)
{
    EXPECT_TRUE(taz_dispatch_opcode_is_async(
        static_cast<uint16_t>(taz_v1_Opcode_OPCODE_CANCEL)));
}

TEST(Dispatch, CancelUndecodablePayloadIsInvalidRequest)
{
    uv_loop_t loop;
    ASSERT_EQ(uv_loop_init(&loop), 0);
    taz_dispatch_t d;
    taz_dispatch_init(&d);
    d.loop = &loop;
    WriteCtx wctx;

    const std::vector<uint8_t> garbage = {0xFFU, 0xFFU, 0xFFU};
    const taz_frame_header_t h =
        MakeHeader(static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_REQUEST),
                   static_cast<uint16_t>(taz_v1_Opcode_OPCODE_CANCEL), 60U,
                   static_cast<uint32_t>(garbage.size()));
    taz_dispatch_frame(&d, &h, garbage.data(), TAZ_FRAME_OK, capture_write,
                       &wctx);

    ASSERT_EQ(wctx.frames.size(), 1U);
    const auto resp = UnpackHeader(wctx.frames[0]);
    EXPECT_EQ(resp.type,
              static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_ERROR));
    taz_v1_ErrorInfo err = taz_v1_ErrorInfo_init_zero;
    ASSERT_TRUE(DecodeErrorInfo(wctx.frames[0], &err));
    EXPECT_EQ(err.code, taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST);
    EXPECT_EQ(d.active_count, 0U);

    ASSERT_EQ(uv_run(&loop, UV_RUN_DEFAULT), 0);
    ASSERT_EQ(uv_loop_close(&loop), 0);
}

TEST(Dispatch, CancelTargetZeroReturnsCancelledFalseSynchronously)
{
    uv_loop_t loop;
    ASSERT_EQ(uv_loop_init(&loop), 0);
    taz_dispatch_t d;
    taz_dispatch_init(&d);
    d.loop = &loop;
    WriteCtx wctx;

    const auto payload = CancelRequestBytes(0U);
    const taz_frame_header_t h =
        MakeHeader(static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_REQUEST),
                   static_cast<uint16_t>(taz_v1_Opcode_OPCODE_CANCEL), 61U,
                   static_cast<uint32_t>(payload.size()));
    taz_dispatch_frame(&d, &h, payload.data(), TAZ_FRAME_OK, capture_write,
                       &wctx);

    ASSERT_EQ(wctx.frames.size(), 1U);
    taz_v1_CancelResponse resp = taz_v1_CancelResponse_init_zero;
    ASSERT_TRUE(DecodeCancelResponse(wctx.frames[0], &resp));
    EXPECT_FALSE(resp.cancelled);
    EXPECT_EQ(d.active_count, 0U);

    ASSERT_EQ(uv_run(&loop, UV_RUN_DEFAULT), 0);
    ASSERT_EQ(uv_loop_close(&loop), 0);
}

TEST(Dispatch, CancelOwnStreamReturnsCancelledFalseSynchronously)
{
    uv_loop_t loop;
    ASSERT_EQ(uv_loop_init(&loop), 0);
    taz_dispatch_t d;
    taz_dispatch_init(&d);
    d.loop = &loop;
    WriteCtx wctx;

    const auto payload = CancelRequestBytes(62U);
    const taz_frame_header_t h =
        MakeHeader(static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_REQUEST),
                   static_cast<uint16_t>(taz_v1_Opcode_OPCODE_CANCEL), 62U,
                   static_cast<uint32_t>(payload.size()));
    taz_dispatch_frame(&d, &h, payload.data(), TAZ_FRAME_OK, capture_write,
                       &wctx);

    ASSERT_EQ(wctx.frames.size(), 1U);
    taz_v1_CancelResponse resp = taz_v1_CancelResponse_init_zero;
    ASSERT_TRUE(DecodeCancelResponse(wctx.frames[0], &resp));
    EXPECT_FALSE(resp.cancelled);
    EXPECT_EQ(d.active_count, 0U);

    ASSERT_EQ(uv_run(&loop, UV_RUN_DEFAULT), 0);
    ASSERT_EQ(uv_loop_close(&loop), 0);
}

TEST(Dispatch, CancelUnknownTargetReturnsCancelledFalseSynchronously)
{
    uv_loop_t loop;
    ASSERT_EQ(uv_loop_init(&loop), 0);
    taz_dispatch_t d;
    taz_dispatch_init(&d);
    d.loop = &loop;
    WriteCtx wctx;

    const auto payload = CancelRequestBytes(999U);
    const taz_frame_header_t h =
        MakeHeader(static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_REQUEST),
                   static_cast<uint16_t>(taz_v1_Opcode_OPCODE_CANCEL), 63U,
                   static_cast<uint32_t>(payload.size()));
    taz_dispatch_frame(&d, &h, payload.data(), TAZ_FRAME_OK, capture_write,
                       &wctx);

    ASSERT_EQ(wctx.frames.size(), 1U);
    taz_v1_CancelResponse resp = taz_v1_CancelResponse_init_zero;
    ASSERT_TRUE(DecodeCancelResponse(wctx.frames[0], &resp));
    EXPECT_FALSE(resp.cancelled);
    EXPECT_EQ(d.active_count, 0U);

    ASSERT_EQ(uv_run(&loop, UV_RUN_DEFAULT), 0);
    ASSERT_EQ(uv_loop_close(&loop), 0);
}

TEST(Dispatch, CancelCommandExecStreamReturnsFalseAndExecIsUnaffected)
{
    uv_loop_t loop;
    ASSERT_EQ(uv_loop_init(&loop), 0);

    taz_dispatch_t d;
    taz_dispatch_init(&d);
    d.loop = &loop;
    WriteCtx wctx;

    // A real COMMAND_EXEC request for the long-lived sleeper helper: it has
    // no stream_ops registered (only handlers/file_transfer.c's transfers
    // do), so taz_dispatch_cancel_stream must refuse it regardless of the
    // exec itself.
    const auto exec_payload =
        CommandExecRequestBytes(TAZ_TEST_SLEEPER_PATH, {});
    const taz_frame_header_t exec_h =
        MakeHeader(static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_REQUEST),
                   static_cast<uint16_t>(taz_v1_Opcode_OPCODE_COMMAND_EXEC),
                   64U, static_cast<uint32_t>(exec_payload.size()));
    taz_dispatch_frame(&d, &exec_h, exec_payload.data(), TAZ_FRAME_OK,
                       capture_write, &wctx);
    ASSERT_EQ(d.active_count, 1U);

    const auto cancel_payload = CancelRequestBytes(64U);
    const taz_frame_header_t cancel_h =
        MakeHeader(static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_REQUEST),
                   static_cast<uint16_t>(taz_v1_Opcode_OPCODE_CANCEL), 65U,
                   static_cast<uint32_t>(cancel_payload.size()));
    taz_dispatch_frame(&d, &cancel_h, cancel_payload.data(), TAZ_FRAME_OK,
                       capture_write, &wctx);

    ASSERT_EQ(wctx.frames.size(), 1U);
    taz_v1_CancelResponse resp = taz_v1_CancelResponse_init_zero;
    ASSERT_TRUE(DecodeCancelResponse(wctx.frames[0], &resp));
    EXPECT_FALSE(resp.cancelled);
    // Only the CANCEL stream closed synchronously; the exec's stream is
    // still active and the sleeper has not been touched.
    EXPECT_EQ(d.active_count, 1U);

    // Clean up the still-sleeping exec the same way connection close would.
    taz_dispatch_cancel_all(&d);
    ASSERT_EQ(uv_run(&loop, UV_RUN_DEFAULT), 0);
    ASSERT_EQ(uv_loop_close(&loop), 0);
    EXPECT_EQ(d.active_count, 0U);
}

struct WritableSpy
{
    int calls = 0;
};

void SpyOnWritable(void *user)
{
    static_cast<WritableSpy *>(user)->calls++;
}

TEST(Dispatch, NotifyWritableCallsOnlyStreamsWithOnWritableSet)
{
    taz_dispatch_t d;
    taz_dispatch_init(&d);
    d.active_streams[0] = 1U;
    d.active_streams[1] = 2U;
    d.active_streams[2] = 3U;
    d.active_count = 3U;

    WritableSpy with_hook;
    taz_stream_ops_t ops_with_hook{};
    ops_with_hook.on_chunk = RecordChunk;
    ops_with_hook.cancel = RefuseCancel;
    ops_with_hook.abort = NoopAbort;
    ops_with_hook.on_writable = SpyOnWritable;
    taz_dispatch_set_stream_ops(&d, 1U, &ops_with_hook, &with_hook);

    taz_stream_ops_t ops_without_hook{};
    ops_without_hook.on_chunk = RecordChunk;
    ops_without_hook.cancel = RefuseCancel;
    ops_without_hook.abort = NoopAbort;
    ops_without_hook.on_writable = nullptr;
    taz_dispatch_set_stream_ops(&d, 2U, &ops_without_hook, nullptr);

    // Stream 3 has no ops at all.

    taz_dispatch_notify_writable(&d);

    EXPECT_EQ(with_hook.calls, 1);
}

TEST(Dispatch, ConnWriteQueueSizePauseResumeNoOpWhenHooksUnset)
{
    taz_dispatch_t d;
    taz_dispatch_init(&d); // hooks NULL: a pure unit-test dispatch.

    EXPECT_EQ(taz_dispatch_conn_write_queue_size(&d), 0U);
    taz_dispatch_conn_pause_reads(&d);
    taz_dispatch_conn_resume_reads(&d);
}

size_t FakeQueueSize(void *ctx)
{
    return *static_cast<size_t *>(ctx);
}

TEST(Dispatch, ConnWriteQueueSizeIsForwardedWithConnCtx)
{
    taz_dispatch_t d;
    taz_dispatch_init(&d);
    size_t fake_value = 777U;
    d.conn_write_queue_size = FakeQueueSize;
    d.conn_ctx = &fake_value;

    EXPECT_EQ(taz_dispatch_conn_write_queue_size(&d), 777U);
}

void CountingPause(void *ctx)
{
    *static_cast<int *>(ctx) += 1;
}

void CountingResume(void *ctx)
{
    *static_cast<int *>(ctx) += 10;
}

TEST(Dispatch, ConnPauseResumeForwardWithConnCtx)
{
    taz_dispatch_t d;
    taz_dispatch_init(&d);
    int counter = 0;
    d.conn_pause_reads = CountingPause;
    d.conn_resume_reads = CountingResume;
    d.conn_ctx = &counter;

    taz_dispatch_conn_pause_reads(&d);
    EXPECT_EQ(counter, 1);
    taz_dispatch_conn_resume_reads(&d);
    EXPECT_EQ(counter, 11);
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
