// Unit tests for handlers/command.c: CommandExecRequest/-Response wire
// round-trips and the exec-result-to-RESPONSE encoder.

#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include <gtest/gtest.h>
#include <pb_decode.h>
#include <pb_encode.h>

#include "handlers/command.h"
#include "taz/exec.h"
#include "taz/frame.h"
#include "taz/v1/command.pb.h"
#include "taz/v1/common.pb.h"

namespace
{

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------

struct WriteCtx
{
    std::vector<std::vector<uint8_t>> frames;
};

void capture_write(const uint8_t *data, size_t len, void *ctx)
{
    auto *wctx = static_cast<WriteCtx *>(ctx);
    wctx->frames.emplace_back(data, data + len);
}

taz_frame_header_t unpack_header(const std::vector<uint8_t> &frame)
{
    taz_frame_header_t h{};
    if (frame.size() >= static_cast<size_t>(TAZ_FRAME_HEADER_SIZE))
    {
        taz_frame_unpack_header(frame.data(), &h);
    }
    return h;
}

bool decode_exec_response(const std::vector<uint8_t> &frame,
                          taz_v1_CommandExecResponse *out)
{
    // A frame with an empty payload (every field at its proto3 default) is
    // valid: only reject frames too short to even carry a header.
    if (frame.size() < static_cast<size_t>(TAZ_FRAME_HEADER_SIZE))
    {
        return false;
    }
    pb_istream_t stream = pb_istream_from_buffer(
        frame.data() + TAZ_FRAME_HEADER_SIZE,
        frame.size() - static_cast<size_t>(TAZ_FRAME_HEADER_SIZE));
    return pb_decode(&stream, taz_v1_CommandExecResponse_fields, out);
}

// Decode callback that collects bytes into a vector<uint8_t>.
bool collect_bytes_cb(pb_istream_t *stream, const pb_field_iter_t * /*f*/,
                      void **arg)
{
    auto *out = static_cast<std::vector<uint8_t> *>(*arg);
    uint8_t buf[256];
    while (stream->bytes_left > 0U)
    {
        const size_t n =
            stream->bytes_left < sizeof(buf) ? stream->bytes_left : sizeof(buf);
        if (!pb_read(stream, buf, n))
        {
            return false;
        }
        out->insert(out->end(), buf, buf + n);
    }
    return true;
}

// Tracks whether a decode callback ever fired, to check field absence.
bool mark_present_cb(pb_istream_t *stream, const pb_field_iter_t * /*f*/,
                     void **arg)
{
    *static_cast<bool *>(*arg) = true;
    // Still need to consume the bytes to leave the stream valid.
    uint8_t buf[256];
    while (stream->bytes_left > 0U)
    {
        const size_t n =
            stream->bytes_left < sizeof(buf) ? stream->bytes_left : sizeof(buf);
        if (!pb_read(stream, buf, n))
        {
            return false;
        }
    }
    return true;
}

taz_exec_result_t make_result(const uint8_t *out, size_t out_len,
                              const uint8_t *err, size_t err_len)
{
    taz_exec_result_t r{};
    r.exit_status = 0;
    r.term_signal = 0;
    r.out = out;
    r.out_len = out_len;
    r.err = err;
    r.err_len = err_len;
    r.timed_out = false;
    r.truncated = false;
    r.cancelled = false;
    return r;
}

// ---------------------------------------------------------------------------
// Request round-trip
// ---------------------------------------------------------------------------

TEST(CommandRequest, RoundTripEncodeDecode)
{
    // Heap-allocated (not on the test's stack): the real struct is ~40 KiB.
    // Value-initialized, which zeroes every field exactly like
    // taz_v1_CommandExecRequest_init_zero.
    auto req = std::make_unique<taz_v1_CommandExecRequest>();

    (void)strncpy(req->command, "/usr/bin/env", sizeof(req->command) - 1U);
    req->args_count = 2;
    (void)strncpy(req->args[0], "-i", sizeof(req->args[0]) - 1U);
    (void)strncpy(req->args[1], "true", sizeof(req->args[1]) - 1U);
    req->env_count = 2;
    (void)strncpy(req->env[0].key, "FOO", sizeof(req->env[0].key) - 1U);
    (void)strncpy(req->env[0].value, "bar", sizeof(req->env[0].value) - 1U);
    (void)strncpy(req->env[1].key, "BAZ", sizeof(req->env[1].key) - 1U);
    (void)strncpy(req->env[1].value, "qux", sizeof(req->env[1].value) - 1U);
    (void)strncpy(req->working_dir, "/tmp", sizeof(req->working_dir) - 1U);
    req->timeout_ms = 1500;

    std::vector<uint8_t> buf(taz_v1_CommandExecRequest_size);
    pb_ostream_t ostream = pb_ostream_from_buffer(buf.data(), buf.size());
    ASSERT_TRUE(
        pb_encode(&ostream, taz_v1_CommandExecRequest_fields, req.get()));

    auto decoded = std::make_unique<taz_v1_CommandExecRequest>();
    pb_istream_t istream =
        pb_istream_from_buffer(buf.data(), ostream.bytes_written);
    ASSERT_TRUE(
        pb_decode(&istream, taz_v1_CommandExecRequest_fields, decoded.get()));

    EXPECT_STREQ(decoded->command, "/usr/bin/env");
    ASSERT_EQ(decoded->args_count, 2);
    EXPECT_STREQ(decoded->args[0], "-i");
    EXPECT_STREQ(decoded->args[1], "true");
    ASSERT_EQ(decoded->env_count, 2);
    EXPECT_STREQ(decoded->env[0].key, "FOO");
    EXPECT_STREQ(decoded->env[0].value, "bar");
    EXPECT_STREQ(decoded->env[1].key, "BAZ");
    EXPECT_STREQ(decoded->env[1].value, "qux");
    EXPECT_STREQ(decoded->working_dir, "/tmp");
    EXPECT_EQ(decoded->timeout_ms, 1500U);
    EXPECT_STREQ(decoded->as_user, "");
}

// ---------------------------------------------------------------------------
// taz_command_send_exec_response: small payload, single frame
// ---------------------------------------------------------------------------

TEST(CommandSendExecResponse, SmallOutputRoundTrips)
{
    const std::vector<uint8_t> out_data{'h', 'e', 'l', 'l', 'o'};
    const std::vector<uint8_t> err_data{'o', 'o', 'p', 's'};

    taz_exec_result_t result = make_result(out_data.data(), out_data.size(),
                                           err_data.data(), err_data.size());
    result.exit_status = 7;
    result.truncated = true;

    WriteCtx wctx;
    taz_command_send_exec_response(
        capture_write, &wctx, 9U,
        static_cast<uint16_t>(taz_v1_Opcode_OPCODE_COMMAND_EXEC), &result);

    ASSERT_EQ(wctx.frames.size(), 1U);
    const taz_frame_header_t h = unpack_header(wctx.frames[0]);
    EXPECT_EQ(h.type,
              static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_RESPONSE));
    EXPECT_EQ(h.flags & static_cast<uint8_t>(
                            taz_v1_FrameFlag_FRAME_FLAG_CONTINUATION),
              0U);
    EXPECT_EQ(h.stream_id, 9U);
    EXPECT_EQ(h.opcode,
              static_cast<uint16_t>(taz_v1_Opcode_OPCODE_COMMAND_EXEC));

    taz_v1_CommandExecResponse decoded = taz_v1_CommandExecResponse_init_zero;
    std::vector<uint8_t> out_decoded;
    std::vector<uint8_t> err_decoded;
    decoded.stdout_data.funcs.decode = collect_bytes_cb;
    decoded.stdout_data.arg = &out_decoded;
    decoded.stderr_data.funcs.decode = collect_bytes_cb;
    decoded.stderr_data.arg = &err_decoded;
    ASSERT_TRUE(decode_exec_response(wctx.frames[0], &decoded));

    EXPECT_EQ(decoded.exit_code, 7);
    EXPECT_FALSE(decoded.timed_out);
    EXPECT_TRUE(decoded.truncated);
    EXPECT_EQ(out_decoded, out_data);
    EXPECT_EQ(err_decoded, err_data);
}

TEST(CommandSendExecResponse, ExitCodeFromExitStatusWhenNoSignal)
{
    taz_exec_result_t result = make_result(nullptr, 0U, nullptr, 0U);
    result.exit_status = 42;
    result.term_signal = 0;

    WriteCtx wctx;
    taz_command_send_exec_response(
        capture_write, &wctx, 1U,
        static_cast<uint16_t>(taz_v1_Opcode_OPCODE_COMMAND_EXEC), &result);

    ASSERT_EQ(wctx.frames.size(), 1U);
    taz_v1_CommandExecResponse decoded = taz_v1_CommandExecResponse_init_zero;
    ASSERT_TRUE(decode_exec_response(wctx.frames[0], &decoded));
    EXPECT_EQ(decoded.exit_code, 42);
}

#ifndef _WIN32
TEST(CommandSendExecResponse, ExitCodeIsNegatedSignalWhenSignalled)
{
    taz_exec_result_t result = make_result(nullptr, 0U, nullptr, 0U);
    result.exit_status = 0;
    result.term_signal = 9; // SIGKILL

    WriteCtx wctx;
    taz_command_send_exec_response(
        capture_write, &wctx, 1U,
        static_cast<uint16_t>(taz_v1_Opcode_OPCODE_COMMAND_EXEC), &result);

    ASSERT_EQ(wctx.frames.size(), 1U);
    taz_v1_CommandExecResponse decoded = taz_v1_CommandExecResponse_init_zero;
    ASSERT_TRUE(decode_exec_response(wctx.frames[0], &decoded));
    EXPECT_EQ(decoded.exit_code, -9);
}
#endif

TEST(CommandSendExecResponse, EmptyOutputsOmitFieldsButStayValid)
{
    taz_exec_result_t result = make_result(nullptr, 0U, nullptr, 0U);
    result.exit_status = 0;

    WriteCtx wctx;
    taz_command_send_exec_response(
        capture_write, &wctx, 3U,
        static_cast<uint16_t>(taz_v1_Opcode_OPCODE_COMMAND_EXEC), &result);

    ASSERT_EQ(wctx.frames.size(), 1U);

    bool out_present = false;
    bool err_present = false;
    taz_v1_CommandExecResponse decoded = taz_v1_CommandExecResponse_init_zero;
    decoded.stdout_data.funcs.decode = mark_present_cb;
    decoded.stdout_data.arg = &out_present;
    decoded.stderr_data.funcs.decode = mark_present_cb;
    decoded.stderr_data.arg = &err_present;
    ASSERT_TRUE(decode_exec_response(wctx.frames[0], &decoded));

    EXPECT_FALSE(out_present);
    EXPECT_FALSE(err_present);
    EXPECT_EQ(decoded.exit_code, 0);
    EXPECT_FALSE(decoded.timed_out);
    EXPECT_FALSE(decoded.truncated);
}

// ---------------------------------------------------------------------------
// taz_command_send_exec_response: large payload, several RESPONSE frames
// ---------------------------------------------------------------------------

TEST(CommandSendExecResponse, LargeOutputSplitsAcrossFrames)
{
    static const size_t kOutSize = 300000U;
    std::vector<uint8_t> out_data(kOutSize);
    for (size_t i = 0U; i < kOutSize; i++)
    {
        out_data[i] = static_cast<uint8_t>(i % 256U);
    }

    taz_exec_result_t result =
        make_result(out_data.data(), out_data.size(), nullptr, 0U);
    result.exit_status = 0;

    WriteCtx wctx;
    taz_command_send_exec_response(
        capture_write, &wctx, 5U,
        static_cast<uint16_t>(taz_v1_Opcode_OPCODE_COMMAND_EXEC), &result);

    ASSERT_GT(wctx.frames.size(), 1U);

    for (size_t i = 0U; i + 1U < wctx.frames.size(); i++)
    {
        const taz_frame_header_t h = unpack_header(wctx.frames[i]);
        EXPECT_NE(h.flags & static_cast<uint8_t>(
                                taz_v1_FrameFlag_FRAME_FLAG_CONTINUATION),
                  0U)
            << "frame " << i << " must have CONTINUATION";
    }
    {
        const taz_frame_header_t h =
            unpack_header(wctx.frames[wctx.frames.size() - 1U]);
        EXPECT_EQ(h.flags & static_cast<uint8_t>(
                                taz_v1_FrameFlag_FRAME_FLAG_CONTINUATION),
                  0U)
            << "last frame must not have CONTINUATION";
    }

    std::vector<uint8_t> merged_out;
    taz_v1_CommandExecResponse last = taz_v1_CommandExecResponse_init_zero;
    for (const auto &frame : wctx.frames)
    {
        taz_v1_CommandExecResponse partial =
            taz_v1_CommandExecResponse_init_zero;
        partial.stdout_data.funcs.decode = collect_bytes_cb;
        partial.stdout_data.arg = &merged_out;
        ASSERT_TRUE(decode_exec_response(frame, &partial));
        last = partial;
    }

    ASSERT_EQ(merged_out.size(), kOutSize);
    EXPECT_EQ(merged_out, out_data);
    EXPECT_EQ(last.exit_code, 0);
    EXPECT_FALSE(last.truncated);
}

} // namespace
