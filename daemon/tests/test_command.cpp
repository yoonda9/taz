// Unit tests for handlers/command.c: CommandExecRequest/-Response wire
// round-trips, the exec-result-to-RESPONSE encoder, the connection-timeout
// snapshot taken before taz_exec_start, and the as_user identity
// precedence taz_command_resolve_as_user computes.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <pb_decode.h>
#include <pb_encode.h>
#include <uv.h>

#ifndef _WIN32
#include <unistd.h>
#endif

#include "handlers/command.h"
#include "run_as_test_support.h"
#include "taz/exec.h"
#include "taz/frame.h"
#include "taz/run_as.h"
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

// Builds a CommandExecRequest naming command with the given per-call
// timeout_ms (0 = none, so handle_command_exec falls back to the
// connection default) and no args/env.
std::vector<uint8_t> encode_exec_request(const std::string &command,
                                         uint32_t timeout_ms)
{
    // Heap-allocated (not on the test's stack): the real struct is ~40 KiB.
    // Value-initialized, which zeroes every field.
    auto req = std::make_unique<taz_v1_CommandExecRequest>();
    (void)strncpy(req->command, command.c_str(), sizeof(req->command) - 1U);
    req->timeout_ms = timeout_ms;

    std::vector<uint8_t> buf(taz_v1_CommandExecRequest_size);
    pb_ostream_t ostream = pb_ostream_from_buffer(buf.data(), buf.size());
    EXPECT_TRUE(
        pb_encode(&ostream, taz_v1_CommandExecRequest_fields, req.get()));
    buf.resize(ostream.bytes_written);
    return buf;
}

// Builds a CommandExecRequest naming command with the given as_user and no
// per-call timeout - for the as_user precedence wiring tests, which never
// reach taz_exec_start on the rows that exercise this helper (NOT_SUPPORTED/
// NOT_FOUND return before the spawn).
std::vector<uint8_t>
encode_exec_request_with_as_user(const std::string &command,
                                 const std::string &as_user)
{
    auto req = std::make_unique<taz_v1_CommandExecRequest>();
    (void)strncpy(req->command, command.c_str(), sizeof(req->command) - 1U);
    (void)strncpy(req->as_user, as_user.c_str(), sizeof(req->as_user) - 1U);

    std::vector<uint8_t> buf(taz_v1_CommandExecRequest_size);
    pb_ostream_t ostream = pb_ostream_from_buffer(buf.data(), buf.size());
    EXPECT_TRUE(
        pb_encode(&ostream, taz_v1_CommandExecRequest_fields, req.get()));
    buf.resize(ostream.bytes_written);
    return buf;
}

// RAII scratch /etc/passwd-style file in a fresh temp directory, removed on
// scope exit - used only by the real-spawn PERMISSION_DENIED row below
// (CommandResolveAsUserTest's fixture owns its own copy of this setup for
// its per-test SetUp/TearDown lifecycle; this one-off does not need it).
class TempPasswdFile
{
  public:
    explicit TempPasswdFile(const std::string &contents)
    {
        char tmpdir[1024];
        size_t tmpdir_len = sizeof(tmpdir) - 1U;
        EXPECT_EQ(uv_os_tmpdir(tmpdir, &tmpdir_len), 0);
        const std::string tpl_str =
            std::string(tmpdir, tmpdir_len) + "/taz_spawn_test_XXXXXX";
        std::vector<char> tpl(tpl_str.begin(), tpl_str.end());
        tpl.push_back('\0');
        uv_fs_t req;
        EXPECT_EQ(uv_fs_mkdtemp(nullptr, &req, tpl.data(), nullptr), 0);
        dir_ = req.path;
        uv_fs_req_cleanup(&req);
        path_ = dir_ + "/passwd";
        std::ofstream out(path_, std::ios::binary | std::ios::trunc);
        out << contents;
    }
    ~TempPasswdFile()
    {
        uv_fs_t unlink_req;
        (void)uv_fs_unlink(nullptr, &unlink_req, path_.c_str(), nullptr);
        uv_fs_req_cleanup(&unlink_req);
        uv_fs_t rmdir_req;
        (void)uv_fs_rmdir(nullptr, &rmdir_req, dir_.c_str(), nullptr);
        uv_fs_req_cleanup(&rmdir_req);
    }
    TempPasswdFile(const TempPasswdFile &) = delete;
    TempPasswdFile &operator=(const TempPasswdFile &) = delete;

    const std::string &Path() const
    {
        return path_;
    }

  private:
    std::string dir_;
    std::string path_;
};

// Fixture for taz_command_resolve_as_user: a scratch /etc/passwd-style file
// the privileged rows inject via ScopedPasswdPath. Mirrors test_fsutil.cpp's
// PasswdLookupByName fixture.
class CommandResolveAsUserTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        char tmpdir[1024];
        size_t tmpdir_len = sizeof(tmpdir) - 1U;
        ASSERT_EQ(uv_os_tmpdir(tmpdir, &tmpdir_len), 0);
        const std::string tpl_str =
            std::string(tmpdir, tmpdir_len) + "/taz_command_test_XXXXXX";
        std::vector<char> tpl(tpl_str.begin(), tpl_str.end());
        tpl.push_back('\0');

        uv_fs_t req;
        ASSERT_EQ(uv_fs_mkdtemp(nullptr, &req, tpl.data(), nullptr), 0);
        dir_ = req.path;
        uv_fs_req_cleanup(&req);
        passwd_path_ = dir_ + "/passwd";
    }

    void TearDown() override
    {
        uv_fs_t unlink_req;
        (void)uv_fs_unlink(nullptr, &unlink_req, passwd_path_.c_str(), nullptr);
        uv_fs_req_cleanup(&unlink_req);
        uv_fs_t rmdir_req;
        (void)uv_fs_rmdir(nullptr, &rmdir_req, dir_.c_str(), nullptr);
        uv_fs_req_cleanup(&rmdir_req);
    }

    void WritePasswd(const std::string &contents) const
    {
        std::ofstream out(passwd_path_, std::ios::binary | std::ios::trunc);
        out << contents;
    }

    const std::string &PasswdPath() const
    {
        return passwd_path_;
    }

  private:
    std::string dir_;
    std::string passwd_path_;
};

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

// ---------------------------------------------------------------------------
// handle_command_exec: the effective-timeout snapshot (connection default
// vs per-call override, taken before taz_exec_start so a later TIMEOUT_SET
// never retargets an in-flight exec). taz_test_sleeper (built from
// sleeper.c) sleeps for 60 s with no shell involved, so these tests have a
// child that would still be running long after the assertions below if the
// snapshot were wrong.
// ---------------------------------------------------------------------------

// Minimal fixture: a real loop and a taz_dispatch_t with no connection
// behind it (conn_ref/conn_unref/conn_closing stay NULL, which
// taz_dispatch_conn_ref/_unref/_closing treat as no-ops - see dispatch.h),
// driving COMMAND_EXEC straight through taz_dispatch_frame so the
// snapshot runs exactly as handle_command_exec computes it, not via a
// direct spec/taz_exec_start call that would bypass it.
class CommandExecSnapshotTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        ASSERT_EQ(uv_loop_init(&loop_), 0);
        taz_dispatch_init(&d_);
        d_.loop = &loop_;
    }

    void TearDown() override
    {
        ASSERT_EQ(uv_loop_close(&loop_), 0);
    }

    void SetConnTimeoutMs(uint32_t ms)
    {
        d_.conn_timeout_ms = ms;
    }

    uint32_t ConnTimeoutMs() const
    {
        return d_.conn_timeout_ms;
    }

    // Dispatches one COMMAND_EXEC REQUEST and runs the loop to completion.
    // check_in_flight, if given, runs right after dispatch but before the
    // loop - to mutate connection state while the exec is already running.
    void DispatchExec(const std::vector<uint8_t> &payload, uint32_t stream_id,
                      const std::function<void()> &check_in_flight = nullptr)
    {
        taz_frame_header_t header{};
        header.type = static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_REQUEST);
        header.flags = static_cast<uint8_t>(taz_v1_FrameFlag_FRAME_FLAG_NONE);
        header.opcode =
            static_cast<uint16_t>(taz_v1_Opcode_OPCODE_COMMAND_EXEC);
        header.length = static_cast<uint32_t>(payload.size());
        header.stream_id = stream_id;

        taz_dispatch_frame(&d_, &header,
                           payload.empty() ? nullptr : payload.data(),
                           TAZ_FRAME_OK, capture_write, &wctx_);
        if (check_in_flight)
        {
            check_in_flight();
        }
        ASSERT_EQ(uv_run(&loop_, UV_RUN_DEFAULT), 0);
    }

    const std::vector<std::vector<uint8_t>> &Frames() const
    {
        return wctx_.frames;
    }

  private:
    uv_loop_t loop_{};
    taz_dispatch_t d_{};
    WriteCtx wctx_{};
};

TEST_F(CommandExecSnapshotTest,
       ConnectionDefaultAppliesWhenPerCallTimeoutIsZero)
{
    SetConnTimeoutMs(150U);

    const auto start = std::chrono::steady_clock::now();
    DispatchExec(encode_exec_request(TAZ_TEST_SLEEPER_PATH, 0U), 1U);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    ASSERT_EQ(Frames().size(), 1U);
    taz_v1_CommandExecResponse resp = taz_v1_CommandExecResponse_init_zero;
    ASSERT_TRUE(decode_exec_response(Frames()[0], &resp));
    EXPECT_TRUE(resp.timed_out);
    EXPECT_LT(elapsed, std::chrono::seconds(10));
}

TEST_F(CommandExecSnapshotTest, PerCallTimeoutOverridesConnectionDefault)
{
    // Large enough that this test would hang (not merely fail) if the
    // connection default were used instead of the per-call value below.
    SetConnTimeoutMs(600000U);

    const auto start = std::chrono::steady_clock::now();
    DispatchExec(encode_exec_request(TAZ_TEST_SLEEPER_PATH, 150U), 1U);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    ASSERT_EQ(Frames().size(), 1U);
    taz_v1_CommandExecResponse resp = taz_v1_CommandExecResponse_init_zero;
    ASSERT_TRUE(decode_exec_response(Frames()[0], &resp));
    EXPECT_TRUE(resp.timed_out);
    EXPECT_LT(elapsed, std::chrono::seconds(10));
}

TEST_F(CommandExecSnapshotTest, LaterTimeoutSetDoesNotRetargetInFlightExec)
{
    SetConnTimeoutMs(150U);

    const auto start = std::chrono::steady_clock::now();
    // Mirrors a TIMEOUT_SET landing on the connection while this exec is
    // already running: handle_command_exec must have snapshotted 150 into
    // the spec before this mutation, not read conn_timeout_ms again later.
    DispatchExec(encode_exec_request(TAZ_TEST_SLEEPER_PATH, 0U), 1U,
                 [this]() { SetConnTimeoutMs(600000U); });
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_EQ(ConnTimeoutMs(), 600000U);
    ASSERT_EQ(Frames().size(), 1U);
    taz_v1_CommandExecResponse resp = taz_v1_CommandExecResponse_init_zero;
    ASSERT_TRUE(decode_exec_response(Frames()[0], &resp));
    EXPECT_TRUE(resp.timed_out);
    EXPECT_LT(elapsed, std::chrono::seconds(10));
}

// ---------------------------------------------------------------------------
// taz_command_resolve_as_user: the precedence between a per-call
// as_user override, the connection's RUN_AS identity, and no switch at
// all. Pure - never spawns - so these build a bare taz_dispatch_t/
// taz_exec_spec_t/CommandExecRequest and call it directly, without going
// through dispatch or a real loop.
// ---------------------------------------------------------------------------

TEST_F(CommandResolveAsUserTest,
       PerCallOverrideNotPrivilegedReturnsNotSupported)
{
    const ScopedPrivilege unprivileged(0);
    auto req = std::make_unique<taz_v1_CommandExecRequest>();
    (void)strncpy(req->as_user, "alice", sizeof(req->as_user) - 1U);

    const taz_dispatch_t d{};
    taz_exec_spec_t spec{};
    taz_v1_ErrorCode error_code = taz_v1_ErrorCode_ERROR_CODE_UNKNOWN;
    const char *error_message = nullptr;

    EXPECT_NE(taz_command_resolve_as_user(&d, req.get(), &spec, &error_code,
                                          &error_message),
              0);
    EXPECT_EQ(error_code, taz_v1_ErrorCode_ERROR_CODE_NOT_SUPPORTED);
    EXPECT_NE(error_message, nullptr);
    EXPECT_FALSE(spec.switch_identity);
}

// taz_run_as_set_privileged_for_tests is a documented no-op on _WIN32
// (RUN_AS is deferred there regardless, so taz_run_as_privileged stays
// hard-0), so the two rows below cannot run there - the per-call override
// would see NOT_SUPPORTED, matching PerCallOverrideNotPrivilegedReturns-
// NotSupported above instead of the privileged behaviour under test.
#ifndef _WIN32

TEST_F(CommandResolveAsUserTest,
       PerCallOverridePrivilegedUnknownUserReturnsNotFound)
{
    const ScopedPrivilege privileged(1);
    WritePasswd("alice:x:4242:4243:Alice:/home/alice:/bin/bash\n");
    const ScopedPasswdPath passwd(PasswdPath());

    auto req = std::make_unique<taz_v1_CommandExecRequest>();
    (void)strncpy(req->as_user, "ghost", sizeof(req->as_user) - 1U);

    const taz_dispatch_t d{};
    taz_exec_spec_t spec{};
    taz_v1_ErrorCode error_code = taz_v1_ErrorCode_ERROR_CODE_UNKNOWN;
    const char *error_message = nullptr;

    EXPECT_NE(taz_command_resolve_as_user(&d, req.get(), &spec, &error_code,
                                          &error_message),
              0);
    EXPECT_EQ(error_code, taz_v1_ErrorCode_ERROR_CODE_NOT_FOUND);
    EXPECT_FALSE(spec.switch_identity);
}

TEST_F(CommandResolveAsUserTest,
       PerCallOverridePrivilegedKnownUserWinsOverConnectionIdentity)
{
    const ScopedPrivilege privileged(1);
    WritePasswd("alice:x:4242:4243:Alice:/home/alice:/bin/bash\n");
    const ScopedPasswdPath passwd(PasswdPath());

    auto req = std::make_unique<taz_v1_CommandExecRequest>();
    (void)strncpy(req->as_user, "alice", sizeof(req->as_user) - 1U);

    taz_dispatch_t d{};
    // A different connection identity is active; the per-call override
    // must win outright, not merge with or defer to it.
    d.run_as_active = 1;
    d.run_as_uid = 9999U;
    d.run_as_gid = 9999U;

    taz_exec_spec_t spec{};
    taz_v1_ErrorCode error_code = taz_v1_ErrorCode_ERROR_CODE_UNKNOWN;
    const char *error_message = nullptr;

    EXPECT_EQ(taz_command_resolve_as_user(&d, req.get(), &spec, &error_code,
                                          &error_message),
              0);
    EXPECT_TRUE(spec.switch_identity);
    EXPECT_EQ(spec.uid, 4242U);
    EXPECT_EQ(spec.gid, 4243U);
    EXPECT_STREQ(spec.identity_user, "alice");
    EXPECT_STREQ(spec.identity_home, "/home/alice");
}

#endif /* !_WIN32 */

TEST(CommandResolveAsUser, ConnectionIdentityAppliesWhenAsUserEmpty)
{
    auto req = std::make_unique<taz_v1_CommandExecRequest>(); // as_user == ""

    taz_dispatch_t d{};
    d.run_as_active = 1;
    // taz_exec_spec_t::uid/gid is uv_uid_t/uv_gid_t, a 1-byte stub type on
    // Windows (unused there, but this TU still compiles for that target),
    // so these values must fit an unsigned char on every platform.
    d.run_as_uid = 77U;
    d.run_as_gid = 88U;
    (void)snprintf(d.run_as_user, sizeof(d.run_as_user), "%s", "carol");
    (void)snprintf(d.run_as_home, sizeof(d.run_as_home), "%s", "/home/carol");

    taz_exec_spec_t spec{};
    taz_v1_ErrorCode error_code = taz_v1_ErrorCode_ERROR_CODE_UNKNOWN;
    const char *error_message = nullptr;

    EXPECT_EQ(taz_command_resolve_as_user(&d, req.get(), &spec, &error_code,
                                          &error_message),
              0);
    EXPECT_TRUE(spec.switch_identity);
    EXPECT_EQ(spec.uid, 77U);
    EXPECT_EQ(spec.gid, 88U);
    EXPECT_STREQ(spec.identity_user, "carol");
    EXPECT_STREQ(spec.identity_home, "/home/carol");
}

TEST(CommandResolveAsUser, NoSwitchWhenNeitherAsUserNorConnectionIdentitySet)
{
    auto req = std::make_unique<taz_v1_CommandExecRequest>();

    const taz_dispatch_t d{};
    taz_exec_spec_t spec{};
    taz_v1_ErrorCode error_code = taz_v1_ErrorCode_ERROR_CODE_UNKNOWN;
    const char *error_message = nullptr;

    EXPECT_EQ(taz_command_resolve_as_user(&d, req.get(), &spec, &error_code,
                                          &error_message),
              0);
    EXPECT_FALSE(spec.switch_identity);
}

// ---------------------------------------------------------------------------
// handle_command_exec wiring: the as_user precedence's error rows surface
// as the expected ERROR frame through the real dispatch path (not just the
// pure resolve function above). Both rows return before taz_exec_start, so
// no process is ever spawned here.
// ---------------------------------------------------------------------------

TEST_F(CommandExecSnapshotTest, AsUserNotPrivilegedReturnsNotSupported)
{
    const ScopedPrivilege unprivileged(0);
    DispatchExec(
        encode_exec_request_with_as_user(TAZ_TEST_SLEEPER_PATH, "alice"), 1U);

    ASSERT_EQ(Frames().size(), 1U);
    EXPECT_EQ(unpack_header(Frames()[0]).type,
              static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_ERROR));
    taz_v1_ErrorInfo err = taz_v1_ErrorInfo_init_zero;
    pb_istream_t istream = pb_istream_from_buffer(
        Frames()[0].data() + TAZ_FRAME_HEADER_SIZE,
        Frames()[0].size() - static_cast<size_t>(TAZ_FRAME_HEADER_SIZE));
    ASSERT_TRUE(pb_decode(&istream, taz_v1_ErrorInfo_fields, &err));
    EXPECT_EQ(err.code, taz_v1_ErrorCode_ERROR_CODE_NOT_SUPPORTED);
}

// taz_run_as_set_privileged_for_tests is a documented no-op on _WIN32
// (RUN_AS is deferred there regardless, so taz_run_as_privileged stays
// hard-0), so the two rows below cannot run there - as_user would see
// NOT_SUPPORTED instead of the privileged behaviour under test, and the
// real-spawn row's identity switch (taz_exec_apply_identity) is itself a
// no-op on Windows.
#ifndef _WIN32

TEST_F(CommandExecSnapshotTest, AsUserPrivilegedUnknownUserReturnsNotFound)
{
    const ScopedPrivilege privileged(1);
    taz_run_as_set_passwd_path_for_tests("/nonexistent/taz-test-passwd");

    DispatchExec(
        encode_exec_request_with_as_user(TAZ_TEST_SLEEPER_PATH, "ghost"), 1U);

    taz_run_as_set_passwd_path_for_tests(nullptr);

    ASSERT_EQ(Frames().size(), 1U);
    EXPECT_EQ(unpack_header(Frames()[0]).type,
              static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_ERROR));
    taz_v1_ErrorInfo err = taz_v1_ErrorInfo_init_zero;
    pb_istream_t istream = pb_istream_from_buffer(
        Frames()[0].data() + TAZ_FRAME_HEADER_SIZE,
        Frames()[0].size() - static_cast<size_t>(TAZ_FRAME_HEADER_SIZE));
    ASSERT_TRUE(pb_decode(&istream, taz_v1_ErrorInfo_fields, &err));
    EXPECT_EQ(err.code, taz_v1_ErrorCode_ERROR_CODE_NOT_FOUND);
}

TEST_F(CommandExecSnapshotTest,
       AsUserPrivilegedRealSpawnFailsWithPermissionDeniedNotInternal)
{
    if (geteuid() == 0)
    {
        GTEST_SKIP() << "root can setuid(0), so the spawn this row needs to "
                        "fail would succeed";
    }
    const ScopedPrivilege privileged(1);
    const TempPasswdFile passwd_file("root:x:0:0:root:/:/bin/sh\n");
    const ScopedPasswdPath passwd(passwd_file.Path());

    // "root" resolves to uid/gid 0: setuid(0)/setgid(0) from this process
    // (non-root, every gate) fails with EPERM, so this exercises a real
    // uv_spawn failure (not a pre-spawn precedence rejection like the two
    // rows above) and proves map_spawn_error maps UV_EPERM to
    // PERMISSION_DENIED instead of falling through to INTERNAL.
    DispatchExec(
        encode_exec_request_with_as_user(TAZ_TEST_SLEEPER_PATH, "root"), 1U);

    ASSERT_EQ(Frames().size(), 1U);
    EXPECT_EQ(unpack_header(Frames()[0]).type,
              static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_ERROR));
    taz_v1_ErrorInfo err = taz_v1_ErrorInfo_init_zero;
    pb_istream_t istream = pb_istream_from_buffer(
        Frames()[0].data() + TAZ_FRAME_HEADER_SIZE,
        Frames()[0].size() - static_cast<size_t>(TAZ_FRAME_HEADER_SIZE));
    ASSERT_TRUE(pb_decode(&istream, taz_v1_ErrorInfo_fields, &err));
    EXPECT_EQ(err.code, taz_v1_ErrorCode_ERROR_CODE_PERMISSION_DENIED);
}

#endif /* !_WIN32 */

} // namespace
