// Unit tests for handlers/process_monitor.c's PROCESS_MONITOR handler:
// validation, the update stream, the clamp, and exit detection via the
// pidfd poll and via the timer-only fallback. Driven end to end through
// taz_dispatch_frame with a real uv_loop_t (see file_test_support.h for the
// shared fixture, also used by test_process.cpp/test_file.cpp/test_dir.cpp).
//
// Cancel, abort-under-load and shutdown races are covered below (CANCEL
// rows, abort rows, shutdown rows).

#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <vector>

#include <gtest/gtest.h>
#include <pb_decode.h>
#include <pb_encode.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#include <fstream>

#include <sys/wait.h>
#include <unistd.h>
#endif

#include "file_test_support.h"
#include "handlers/process_monitor.h"
#include "taz/v1/advanced.pb.h"
#include "taz/v1/common.pb.h"
#include "taz/v1/process.pb.h"
#include "taz/work.h"

namespace
{

// A pid that cannot exist: on Linux, one past the kernel's configured
// ceiling (varies by host, so read rather than hardcoded); on Windows, a
// pid aligned to 4 that is implausibly large.
uint32_t ImpossiblePid()
{
#ifndef _WIN32
    std::ifstream f("/proc/sys/kernel/pid_max");
    unsigned long long pid_max = 0ULL;
    f >> pid_max;
    return static_cast<uint32_t>(pid_max + 1ULL);
#else
    return 0x7FFFFFFCU;
#endif
}

bool IsValidUtf8(const std::string &s)
{
    size_t i = 0U;
    while (i < s.size())
    {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        size_t extra;
        if (c <= 0x7FU)
        {
            extra = 0U;
        }
        else if ((c & 0xE0U) == 0xC0U)
        {
            extra = 1U;
        }
        else if ((c & 0xF0U) == 0xE0U)
        {
            extra = 2U;
        }
        else if ((c & 0xF8U) == 0xF0U)
        {
            extra = 3U;
        }
        else
        {
            return false;
        }
        if (i + extra >= s.size())
        {
            return false;
        }
        for (size_t k = 1U; k <= extra; k++)
        {
            const unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0U) != 0x80U)
            {
                return false;
            }
        }
        i += extra + 1U;
    }
    return true;
}

std::vector<uint8_t> encode_monitor_request(uint32_t pid, uint32_t interval_ms)
{
    taz_v1_ProcessMonitorRequest req = taz_v1_ProcessMonitorRequest_init_zero;
    req.pid = pid;
    req.interval_ms = interval_ms;

    std::vector<uint8_t> buf(taz_v1_ProcessMonitorRequest_size);
    pb_ostream_t ostream = pb_ostream_from_buffer(buf.data(), buf.size());
    EXPECT_TRUE(pb_encode(&ostream, taz_v1_ProcessMonitorRequest_fields, &req));
    buf.resize(ostream.bytes_written);
    return buf;
}

taz_v1_ProcessMonitorResponse
decode_monitor_response(const std::vector<uint8_t> &frame)
{
    taz_v1_ProcessMonitorResponse resp =
        taz_v1_ProcessMonitorResponse_init_zero;
    const std::vector<uint8_t> body = frame_payload(frame);
    pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
    EXPECT_TRUE(
        pb_decode(&istream, taz_v1_ProcessMonitorResponse_fields, &resp));
    return resp;
}

bool FrameHasContinuation(const std::vector<uint8_t> &frame)
{
    return (unpack_header(frame).flags &
            static_cast<uint8_t>(taz_v1_FrameFlag_FRAME_FLAG_CONTINUATION)) !=
           0U;
}

// Duplicated from test_file_transfer.cpp's own copy (separate TU, same
// precedent as that file's note on test_process_platform.cpp): both are
// `static`/anonymous-namespace, so no ODR clash across the shared
// taz_tests binary.
std::vector<uint8_t> encode_cancel_request(uint32_t target_stream_id)
{
    taz_v1_CancelRequest req = taz_v1_CancelRequest_init_zero;
    req.target_stream_id = target_stream_id;
    std::vector<uint8_t> buf(taz_v1_CancelRequest_size);
    pb_ostream_t ostream = pb_ostream_from_buffer(buf.data(), buf.size());
    EXPECT_TRUE(pb_encode(&ostream, taz_v1_CancelRequest_fields, &req));
    buf.resize(ostream.bytes_written);
    return buf;
}

void ExpectCancelled(const std::vector<uint8_t> &frame, bool cancelled)
{
    const taz_frame_header_t h = unpack_header(frame);
    EXPECT_EQ(h.type,
              static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_RESPONSE));
    EXPECT_EQ(h.opcode, static_cast<uint16_t>(taz_v1_Opcode_OPCODE_CANCEL));

    taz_v1_CancelResponse resp = taz_v1_CancelResponse_init_zero;
    const std::vector<uint8_t> body = frame_payload(frame);
    pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
    EXPECT_TRUE(pb_decode(&istream, taz_v1_CancelResponse_fields, &resp));
    EXPECT_EQ(resp.cancelled, cancelled);
}

// Mirrors test_process.cpp's SpawnOutcome/OnSleeperExit/SpawnSleeper
// (duplicated, not shared - a separate TU, same precedent as that file's
// own note on test_process_platform.cpp): spawns taz_test_sleeper (60 s, no
// shell) so the handler has a real, harmless child to monitor and kill.
struct SpawnOutcome
{
    bool called = false;
    int64_t exit_status = -1;
    int term_signal = 0;
};

void OnSleeperExit(uv_process_t *process, int64_t exit_status, int term_signal)
{
    auto *outcome = static_cast<SpawnOutcome *>(process->data);
    outcome->called = true;
    outcome->exit_status = exit_status;
    outcome->term_signal = term_signal;
}

uint32_t SpawnSleeper(uv_loop_t *loop, uv_process_t *process,
                      SpawnOutcome *outcome)
{
    char *const args[] = {const_cast<char *>(TAZ_TEST_SLEEPER_PATH), nullptr};
    uv_process_options_t options;

    std::memset(&options, 0, sizeof(options));
    options.exit_cb = OnSleeperExit;
    options.file = TAZ_TEST_SLEEPER_PATH;
    options.args = const_cast<char **>(args);

    process->data = outcome;
    if (uv_spawn(loop, process, &options) != 0)
    {
        return 0U;
    }
    return static_cast<uint32_t>(uv_process_get_pid(process));
}

// Pumps loop (UV_RUN_ONCE) until pred() is true or timeout_ms elapses. A
// fatal failure here only returns from this function (googletest's usual
// ASSERT_ caveat); every caller immediately re-checks the condition it was
// waiting for, so a timeout is never silently treated as success.
void RunLoopUntil(uv_loop_t *loop, const std::function<bool()> &pred,
                  int timeout_ms)
{
    const uint64_t deadline =
        uv_hrtime() + (static_cast<uint64_t>(timeout_ms) * UINT64_C(1000000));
    while (!pred())
    {
        ASSERT_LT(uv_hrtime(), deadline)
            << "condition not met within " << timeout_ms << " ms";
        uv_run(loop, UV_RUN_ONCE);
    }
}

// Same as RunLoopUntil but pumps UV_RUN_NOWAIT instead of UV_RUN_ONCE, for
// observing a transient mid-flight state (e.g. a sample's ref count having
// risen) without blocking on the timer/poll wait the ONCE variant would
// use - NOWAIT still runs the timer phase of each iteration, which is what
// lets a zero-timeout uv_timer_t fire.
void RunLoopNowaitUntil(uv_loop_t *loop, const std::function<bool()> &pred,
                        int timeout_ms)
{
    const uint64_t deadline =
        uv_hrtime() + (static_cast<uint64_t>(timeout_ms) * UINT64_C(1000000));
    while (!pred())
    {
        ASSERT_LT(uv_hrtime(), deadline)
            << "condition not met within " << timeout_ms << " ms";
        uv_run(loop, UV_RUN_NOWAIT);
    }
}

// Unit-test hook, restored on scope exit (global state across the test
// binary - every use is scoped so tests stay independent).
class ScopedPidfdDisabled
{
  public:
    ScopedPidfdDisabled()
    {
        taz_process_monitor_set_pidfd_enabled(0);
    }
    ~ScopedPidfdDisabled()
    {
        taz_process_monitor_set_pidfd_enabled(1);
    }
    ScopedPidfdDisabled(const ScopedPidfdDisabled &) = delete;
    ScopedPidfdDisabled &operator=(const ScopedPidfdDisabled &) = delete;
};

// A sample that fails for a reason other than "the process is gone".
int FailingSample(const taz_process_watch_t * /*w*/, taz_process_sample_t *out,
                  taz_v1_ErrorCode *code, const char **detail)
{
    std::memset(out, 0, sizeof(*out));
    *code = taz_v1_ErrorCode_ERROR_CODE_INTERNAL;
    *detail = "injected sample failure";
    return -1;
}

// Unit-test hook, restored on scope exit like ScopedPidfdDisabled.
class ScopedSampleFn
{
  public:
    explicit ScopedSampleFn(taz_process_sample_fn_t fn)
    {
        taz_process_monitor_set_sample_fn(fn);
    }
    ~ScopedSampleFn()
    {
        taz_process_monitor_set_sample_fn(nullptr);
    }
    ScopedSampleFn(const ScopedSampleFn &) = delete;
    ScopedSampleFn &operator=(const ScopedSampleFn &) = delete;
};

#ifndef _WIN32
// A raw fork+execv child (not uv_spawn, which libuv reaps itself via its
// own SIGCHLD watcher): stays a zombie after being killed until waitpid'd,
// which this helper only does in its destructor, at the very end of the
// test - mirrors test_process_platform.cpp's ProcessWatchZombie fixture.
class ForkSleeper
{
  public:
    // Callers must ASSERT_TRUE(Ok()) before using Pid(): a failed fork
    // leaves pid_ at -1, and kill() on that would signal every process the
    // user owns.
    ForkSleeper()
    {
        pid_ = fork();
        if (pid_ == 0)
        {
            char *const args[] = {const_cast<char *>(TAZ_TEST_SLEEPER_PATH),
                                  nullptr};
            execv(TAZ_TEST_SLEEPER_PATH, args);
            _exit(127);
        }
    }
    ~ForkSleeper()
    {
        if (pid_ > 0)
        {
            int status = 0;
            (void)waitpid(pid_, &status, 0);
        }
    }
    ForkSleeper(const ForkSleeper &) = delete;
    ForkSleeper &operator=(const ForkSleeper &) = delete;

    bool Ok() const
    {
        return pid_ > 0;
    }

    uint32_t Pid() const
    {
        return static_cast<uint32_t>(pid_);
    }

    // SIGKILL for the forked child only; refuses (returns -1) when the fork
    // failed, so a bad pid can never reach kill().
    int Kill() const
    {
        return (pid_ > 0) ? kill(pid_, SIGKILL) : -1;
    }

  private:
    pid_t pid_ = -1;
};
#endif // !_WIN32

} // namespace

TEST(ProcessMonitorInterval, ClampsBelowOneHundredToOneHundred)
{
    EXPECT_EQ(taz_process_monitor_interval_ms(0U), 100U);
    EXPECT_EQ(taz_process_monitor_interval_ms(99U), 100U);
    EXPECT_EQ(taz_process_monitor_interval_ms(100U), 100U);
    EXPECT_EQ(taz_process_monitor_interval_ms(101U), 101U);
    EXPECT_EQ(taz_process_monitor_interval_ms(1000U), 1000U);
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

TEST_F(FileHandlerTest, MonitorPidZeroIsInvalidRequest)
{
    DispatchRequest(taz_v1_Opcode_OPCODE_PROCESS_MONITOR,
                    encode_monitor_request(0U, 1000U), 50U);

    ASSERT_EQ(Frames().size(), 1U);
    taz_v1_ErrorInfo err = taz_v1_ErrorInfo_init_zero;
    const std::vector<uint8_t> body = frame_payload(Frames()[0]);
    pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
    ASSERT_TRUE(pb_decode(&istream, taz_v1_ErrorInfo_fields, &err));
    EXPECT_EQ(err.code, taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST);

    EXPECT_EQ(RefCount(), 0);
    EXPECT_EQ(UnrefCount(), 0);
    EXPECT_EQ(ActiveStreamCount(), 0U);
}

TEST_F(FileHandlerTest, MonitorPidAboveInt32MaxIsInvalidRequest)
{
    DispatchRequest(taz_v1_Opcode_OPCODE_PROCESS_MONITOR,
                    encode_monitor_request(2147483648U, 1000U), 51U);

    ASSERT_EQ(Frames().size(), 1U);
    taz_v1_ErrorInfo err = taz_v1_ErrorInfo_init_zero;
    const std::vector<uint8_t> body = frame_payload(Frames()[0]);
    pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
    ASSERT_TRUE(pb_decode(&istream, taz_v1_ErrorInfo_fields, &err));
    EXPECT_EQ(err.code, taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST);

    EXPECT_EQ(RefCount(), 0);
    EXPECT_EQ(ActiveStreamCount(), 0U);
}

TEST_F(FileHandlerTest, MonitorUndecodablePayloadIsInvalidRequest)
{
    // Field 1 tagged as length-delimited (0x0A = field 1 << 3 | 2) with
    // length 255 (0xFF) but no data - decode fails, same trick as
    // test_process.cpp's ProcessKillUndecodablePayloadIsInvalidRequest.
    const std::vector<uint8_t> payload{0x0AU, 0xFFU};

    DispatchRequest(taz_v1_Opcode_OPCODE_PROCESS_MONITOR, payload, 52U);

    ASSERT_EQ(Frames().size(), 1U);
    taz_v1_ErrorInfo err = taz_v1_ErrorInfo_init_zero;
    const std::vector<uint8_t> body = frame_payload(Frames()[0]);
    pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
    ASSERT_TRUE(pb_decode(&istream, taz_v1_ErrorInfo_fields, &err));
    EXPECT_EQ(err.code, taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST);

    EXPECT_EQ(RefCount(), 0);
    EXPECT_EQ(ActiveStreamCount(), 0U);
}

TEST_F(FileHandlerTest, MonitorEmptyPayloadIsInvalidRequest)
{
    // An empty payload decodes to pid 0, which validation then rejects.
    DispatchRequest(taz_v1_Opcode_OPCODE_PROCESS_MONITOR, {}, 53U);

    ASSERT_EQ(Frames().size(), 1U);
    taz_v1_ErrorInfo err = taz_v1_ErrorInfo_init_zero;
    const std::vector<uint8_t> body = frame_payload(Frames()[0]);
    pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
    ASSERT_TRUE(pb_decode(&istream, taz_v1_ErrorInfo_fields, &err));
    EXPECT_EQ(err.code, taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST);

    EXPECT_EQ(RefCount(), 0);
    EXPECT_EQ(ActiveStreamCount(), 0U);
}

// On both platforms an impossible pid is rejected synchronously by
// taz_process_watch_open itself (Linux: pidfd_open ESRCH; Windows:
// OpenProcess ERROR_INVALID_PARAMETER) - before any timer/poll handle, and
// so before taz_dispatch_frame even returns.
TEST_F(FileHandlerTest, MonitorImpossiblePidIsNotFoundSynchronously)
{
    const uint32_t pid = ImpossiblePid();

    DispatchRequest(taz_v1_Opcode_OPCODE_PROCESS_MONITOR,
                    encode_monitor_request(pid, 1000U), 54U);

    ASSERT_EQ(Frames().size(), 1U);
    taz_v1_ErrorInfo err = taz_v1_ErrorInfo_init_zero;
    const std::vector<uint8_t> body = frame_payload(Frames()[0]);
    pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
    ASSERT_TRUE(pb_decode(&istream, taz_v1_ErrorInfo_fields, &err));
    EXPECT_EQ(err.code, taz_v1_ErrorCode_ERROR_CODE_NOT_FOUND);

    EXPECT_EQ(RefCount(), 0);
    EXPECT_EQ(ActiveStreamCount(), 0U);
}

#ifndef _WIN32
// With pidfd disabled, Linux's taz_process_watch_open(pid, 0, ...) never
// validates the pid (no syscall at all) - the stream opens, and the first
// sample's /proc/<pid>/stat read is what discovers ENOENT, one loop
// iteration later. Windows always validates at open regardless of the
// pidfd flag (OpenProcess is unconditional), so this row has no Windows
// equivalent.
TEST_F(FileHandlerTest,
       MonitorImpossiblePidIsNotFoundAfterOneSampleWithoutPidfd)
{
    const ScopedPidfdDisabled guard;
    const uint32_t pid = ImpossiblePid();

    DispatchRequestNoRun(taz_v1_Opcode_OPCODE_PROCESS_MONITOR,
                         encode_monitor_request(pid, 100U), 55U);
    ASSERT_EQ(Frames().size(), 0U);

    RunLoopUntil(Loop(), [&]() { return !Frames().empty(); }, 5000);
    ASSERT_EQ(Frames().size(), 1U);

    taz_v1_ErrorInfo err = taz_v1_ErrorInfo_init_zero;
    const std::vector<uint8_t> body = frame_payload(Frames()[0]);
    pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
    ASSERT_TRUE(pb_decode(&istream, taz_v1_ErrorInfo_fields, &err));
    EXPECT_EQ(err.code, taz_v1_ErrorCode_ERROR_CODE_NOT_FOUND);

    RunLoop();
    EXPECT_EQ(RefCount(), UnrefCount());
    EXPECT_EQ(ActiveStreamCount(), 0U);
}
#endif // !_WIN32

// ---------------------------------------------------------------------------
// Updates and exit via kill
// ---------------------------------------------------------------------------

TEST_F(FileHandlerTest, MonitorStreamsUpdatesThenExitsOnKill)
{
    uv_process_t process{};
    SpawnOutcome outcome;
    const uint32_t pid = SpawnSleeper(Loop(), &process, &outcome);
    ASSERT_NE(pid, 0U);

    DispatchRequestNoRun(taz_v1_Opcode_OPCODE_PROCESS_MONITOR,
                         encode_monitor_request(pid, 100U), 60U);

    // The first update arrives essentially immediately (the timer's
    // initial timeout is 0): bounded generously, not asserted as exactly
    // one RunLoopOnce, since that depends on exactly when the threadpool's
    // completion signal is observed within the loop's phases.
    RunLoopUntil(Loop(), [&]() { return !Frames().empty(); }, 1000);
    ASSERT_EQ(Frames().size(), 1U);
    EXPECT_TRUE(FrameHasContinuation(Frames()[0]));
    {
        const taz_v1_ProcessMonitorResponse resp =
            decode_monitor_response(Frames()[0]);
        EXPECT_FALSE(resp.exited);
        EXPECT_STREQ(resp.reason, "");
        EXPECT_EQ(resp.info.pid, pid);
        EXPECT_TRUE(IsValidUtf8(resp.info.name));
        EXPECT_GT(resp.info.memory_bytes, 0U);
        EXPECT_STRNE(resp.info.state, "");
        EXPECT_FLOAT_EQ(resp.info.cpu_percent, 0.0F);
    }

    std::vector<std::chrono::steady_clock::time_point> stamps;
    stamps.push_back(std::chrono::steady_clock::now());
    size_t seen = 1U;
    RunLoopUntil(
        Loop(),
        [&]()
        {
            while (seen < Frames().size())
            {
                stamps.push_back(std::chrono::steady_clock::now());
                seen++;
            }
            return seen >= 3U;
        },
        10000);
    ASSERT_GE(Frames().size(), 3U);
    for (size_t i = 1U; i < 3U; i++)
    {
        EXPECT_TRUE(FrameHasContinuation(Frames()[i]));
        const taz_v1_ProcessMonitorResponse resp =
            decode_monitor_response(Frames()[i]);
        EXPECT_FALSE(resp.exited);
        EXPECT_EQ(resp.info.pid, pid);
        EXPECT_GE(resp.info.cpu_percent, 0.0F);
    }
    const auto gap_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            stamps[1] - stamps[0])
                            .count();
    EXPECT_GE(gap_ms, 90);
    EXPECT_LT(gap_ms, 1000);

    const size_t before_kill = Frames().size();
    ASSERT_EQ(uv_process_kill(&process, SIGKILL), 0);

    RunLoopUntil(
        Loop(), [&]() { return Frames().size() > before_kill; }, 10000);
    ASSERT_EQ(Frames().size(), before_kill + 1U);
    const std::vector<uint8_t> &final_frame = Frames().back();
    EXPECT_FALSE(FrameHasContinuation(final_frame));
    {
        const taz_v1_ProcessMonitorResponse resp =
            decode_monitor_response(final_frame);
        EXPECT_TRUE(resp.exited);
        EXPECT_STREQ(resp.reason, "exited");
        EXPECT_EQ(resp.info.pid, pid);
        // The last known fields, not a blank entry.
        EXPECT_STRNE(resp.info.name, "");
#ifdef _WIN32
        EXPECT_TRUE(resp.exit_code_known);
        EXPECT_EQ(resp.exit_code, 1);
#else
        EXPECT_FALSE(resp.exit_code_known);
#endif
    }

    // The timer/poll handles are already closed and the ctx freed - running
    // the loop again proves no more frames can arrive, deterministically
    // (nothing remains that could produce one), rather than by waiting out
    // a few more intervals.
    RunLoop();
    EXPECT_EQ(Frames().size(), before_kill + 1U);
    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());

    uv_close(reinterpret_cast<uv_handle_t *>(&process), nullptr);
    RunLoop();
}

TEST_F(FileHandlerTest, MonitorIntervalZeroIsClampedAndFirstGapIsAtLeast90Ms)
{
    uv_process_t process{};
    SpawnOutcome outcome;
    const uint32_t pid = SpawnSleeper(Loop(), &process, &outcome);
    ASSERT_NE(pid, 0U);

    DispatchRequestNoRun(taz_v1_Opcode_OPCODE_PROCESS_MONITOR,
                         encode_monitor_request(pid, 0U), 61U);

    std::vector<std::chrono::steady_clock::time_point> stamps;
    size_t seen = 0U;
    RunLoopUntil(
        Loop(),
        [&]()
        {
            while (seen < Frames().size())
            {
                stamps.push_back(std::chrono::steady_clock::now());
                seen++;
            }
            return seen >= 2U;
        },
        5000);
    ASSERT_GE(stamps.size(), 2U);
    const auto gap_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            stamps[1] - stamps[0])
                            .count();
    EXPECT_GE(gap_ms, 90);

    ASSERT_EQ(uv_process_kill(&process, SIGKILL), 0);
    const size_t before_kill = Frames().size();
    RunLoopUntil(
        Loop(), [&]() { return Frames().size() > before_kill; }, 10000);
    RunLoop();
    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());

    uv_close(reinterpret_cast<uv_handle_t *>(&process), nullptr);
    RunLoop();
}

// No two consecutive update frames closer than 50 ms apart at interval
// 100: the skipped-tick rule (a tick that fires while a sample is already
// in flight is skipped, not queued) means bursts never happen. Ends the
// stream via connection close rather than a kill, incidentally exercising
// the abort op this task also implements in full.
TEST_F(FileHandlerTest, MonitorNoTwoUpdateFramesCloserThan50Ms)
{
    uv_process_t process{};
    SpawnOutcome outcome;
    const uint32_t pid = SpawnSleeper(Loop(), &process, &outcome);
    ASSERT_NE(pid, 0U);

    DispatchRequestNoRun(taz_v1_Opcode_OPCODE_PROCESS_MONITOR,
                         encode_monitor_request(pid, 100U), 62U);

    std::vector<std::chrono::steady_clock::time_point> stamps;
    size_t seen = 0U;
    RunLoopUntil(
        Loop(),
        [&]()
        {
            while (seen < Frames().size())
            {
                stamps.push_back(std::chrono::steady_clock::now());
                seen++;
            }
            return seen >= 4U;
        },
        10000);
    ASSERT_GE(stamps.size(), 4U);
    for (size_t i = 1U; i < stamps.size(); i++)
    {
        const auto gap_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                stamps[i] - stamps[i - 1U])
                .count();
        EXPECT_GE(gap_ms, 50) << "frames " << (i - 1U) << " and " << i;
    }

    // Kill and let libuv reap the sleeper before RunLoop(): otherwise
    // UV_RUN_DEFAULT would block until the sleeper's own 60 s uv_sleep
    // elapses, since a live uv_process_t keeps the loop alive on its own,
    // independent of the monitor stream abort below.
    ASSERT_EQ(uv_process_kill(&process, SIGKILL), 0);
    RunLoopUntil(Loop(), [&]() { return outcome.called; }, 5000);

    CloseConnectionAndCancelAll();
    RunLoop();
    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());

    uv_close(reinterpret_cast<uv_handle_t *>(&process), nullptr);
    RunLoop();
}

// ---------------------------------------------------------------------------
// Exit detection: pidfd-disabled fallback, and zombies (both pidfd modes)
// ---------------------------------------------------------------------------

#ifndef _WIN32
TEST_F(FileHandlerTest, MonitorExitWithPidfdDisabledDetectedAfterReap)
{
    const ScopedPidfdDisabled guard;

    uv_process_t process{};
    SpawnOutcome outcome;
    const uint32_t pid = SpawnSleeper(Loop(), &process, &outcome);
    ASSERT_NE(pid, 0U);

    DispatchRequestNoRun(taz_v1_Opcode_OPCODE_PROCESS_MONITOR,
                         encode_monitor_request(pid, 100U), 63U);
    RunLoopUntil(Loop(), [&]() { return !Frames().empty(); }, 1000);
    ASSERT_GE(Frames().size(), 1U);

    ASSERT_EQ(uv_process_kill(&process, SIGKILL), 0);

    RunLoopUntil(
        Loop(), [&]()
        { return !Frames().empty() && !FrameHasContinuation(Frames().back()); },
        5000);
    const taz_v1_ProcessMonitorResponse resp =
        decode_monitor_response(Frames().back());
    EXPECT_TRUE(resp.exited);
    EXPECT_STREQ(resp.reason, "exited");
    EXPECT_FALSE(resp.exit_code_known);

    RunLoop();
    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());

    uv_close(reinterpret_cast<uv_handle_t *>(&process), nullptr);
    RunLoop();
}

TEST_F(FileHandlerTest, MonitorZombieWithPidfdDisabledReportsExitedViaState)
{
    const ScopedPidfdDisabled guard;
    const ForkSleeper child;
    ASSERT_TRUE(child.Ok());
    const uint32_t pid = child.Pid();

    DispatchRequestNoRun(taz_v1_Opcode_OPCODE_PROCESS_MONITOR,
                         encode_monitor_request(pid, 100U), 64U);
    RunLoopUntil(Loop(), [&]() { return !Frames().empty(); }, 1000);
    ASSERT_GE(Frames().size(), 1U);

    ASSERT_EQ(child.Kill(), 0);

    RunLoopUntil(
        Loop(), [&]()
        { return !Frames().empty() && !FrameHasContinuation(Frames().back()); },
        5000);
    const taz_v1_ProcessMonitorResponse resp =
        decode_monitor_response(Frames().back());
    EXPECT_TRUE(resp.exited);
    EXPECT_STREQ(resp.reason, "exited");
    EXPECT_FALSE(resp.exit_code_known);

    RunLoop();
    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());
}

TEST_F(FileHandlerTest, MonitorZombieWithPidfdEnabledReportsExitedViaPoll)
{
    const ForkSleeper child;
    ASSERT_TRUE(child.Ok());
    const uint32_t pid = child.Pid();

    DispatchRequestNoRun(taz_v1_Opcode_OPCODE_PROCESS_MONITOR,
                         encode_monitor_request(pid, 100U), 65U);
    RunLoopUntil(Loop(), [&]() { return !Frames().empty(); }, 1000);
    ASSERT_GE(Frames().size(), 1U);

    ASSERT_EQ(child.Kill(), 0);

    RunLoopUntil(
        Loop(), [&]()
        { return !Frames().empty() && !FrameHasContinuation(Frames().back()); },
        5000);
    const taz_v1_ProcessMonitorResponse resp =
        decode_monitor_response(Frames().back());
    EXPECT_TRUE(resp.exited);
    EXPECT_STREQ(resp.reason, "exited");
    EXPECT_FALSE(resp.exit_code_known);

    RunLoop();
    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());
}
#endif // !_WIN32

// ---------------------------------------------------------------------------
// CANCEL
// ---------------------------------------------------------------------------

TEST_F(FileHandlerTest,
       MonitorCancelAfterTwoUpdatesSendsFinalThenCancelResponseTrue)
{
    uv_process_t process{};
    SpawnOutcome outcome;
    const uint32_t pid = SpawnSleeper(Loop(), &process, &outcome);
    ASSERT_NE(pid, 0U);

    DispatchRequestNoRun(taz_v1_Opcode_OPCODE_PROCESS_MONITOR,
                         encode_monitor_request(pid, 100U), 70U);
    RunLoopUntil(Loop(), [&]() { return Frames().size() >= 2U; }, 5000);
    const size_t before_cancel = Frames().size();
    ASSERT_GE(before_cancel, 2U);

    DispatchRequestNoRun(taz_v1_Opcode_OPCODE_CANCEL,
                         encode_cancel_request(70U), 71U);

    ASSERT_EQ(Frames().size(), before_cancel + 2U);
    EXPECT_FALSE(FrameHasContinuation(Frames()[before_cancel]));
    {
        const taz_v1_ProcessMonitorResponse resp =
            decode_monitor_response(Frames()[before_cancel]);
        EXPECT_FALSE(resp.exited);
        EXPECT_STREQ(resp.reason, "cancelled");
        EXPECT_EQ(resp.info.pid, pid);
    }
    ExpectCancelled(Frames()[before_cancel + 1U], true);

    // Nothing more can ever arrive: by this point the monitor's timer and
    // poll handle are already scheduled to close (synchronously, since no
    // sample was in flight) - deterministic, not a timed wait.
    ASSERT_EQ(uv_process_kill(&process, SIGKILL), 0);
    RunLoopUntil(Loop(), [&]() { return outcome.called; }, 5000);
    const size_t after_cancel = Frames().size();
    RunLoop();
    EXPECT_EQ(Frames().size(), after_cancel);
    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());

    uv_close(reinterpret_cast<uv_handle_t *>(&process), nullptr);
    RunLoop();
}

TEST_F(FileHandlerTest, MonitorCancelSameTargetTwiceInARowReturnsTrueThenFalse)
{
    uv_process_t process{};
    SpawnOutcome outcome;
    const uint32_t pid = SpawnSleeper(Loop(), &process, &outcome);
    ASSERT_NE(pid, 0U);

    DispatchRequestNoRun(taz_v1_Opcode_OPCODE_PROCESS_MONITOR,
                         encode_monitor_request(pid, 100U), 72U);
    RunLoopUntil(Loop(), [&]() { return !Frames().empty(); }, 1000);
    ASSERT_EQ(Frames().size(), 1U);

    DispatchRequestNoRun(taz_v1_Opcode_OPCODE_CANCEL,
                         encode_cancel_request(72U), 73U);
    ASSERT_EQ(Frames().size(), 3U);
    EXPECT_FALSE(FrameHasContinuation(Frames()[1]));
    ExpectCancelled(Frames()[2], true);

    // Same target again, now MON_CLOSING (its own close callbacks have not
    // even run yet): refused synchronously, same as CancelOnDrainingPut...
    // in test_file_transfer.cpp.
    DispatchRequestNoRun(taz_v1_Opcode_OPCODE_CANCEL,
                         encode_cancel_request(72U), 74U);
    ASSERT_EQ(Frames().size(), 4U);
    ExpectCancelled(Frames()[3], false);

    ASSERT_EQ(uv_process_kill(&process, SIGKILL), 0);
    RunLoopUntil(Loop(), [&]() { return outcome.called; }, 5000);
    RunLoop();
    EXPECT_EQ(Frames().size(), 4U);
    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());

    uv_close(reinterpret_cast<uv_handle_t *>(&process), nullptr);
    RunLoop();
}

// A client that sends CANCEL and disconnects can have both arrive in one
// read, so the stream ends and the connection closes in the same loop
// iteration. The monitor is still in the dispatch table until its close
// callbacks run, so abort reaches it with its handles already closing:
// abort must do nothing (libuv asserts on stopping a closing handle), and
// the stream must still be released.
TEST_F(FileHandlerTest, MonitorAbortAfterCancelInSameIterationIsNoop)
{
    uv_process_t process{};
    SpawnOutcome outcome;
    const uint32_t pid = SpawnSleeper(Loop(), &process, &outcome);
    ASSERT_NE(pid, 0U);

    DispatchRequestNoRun(taz_v1_Opcode_OPCODE_PROCESS_MONITOR,
                         encode_monitor_request(pid, 100U), 120U);
    RunLoopUntil(Loop(), [&]() { return !Frames().empty(); }, 1000);
    ASSERT_EQ(Frames().size(), 1U);

    DispatchRequestNoRun(taz_v1_Opcode_OPCODE_CANCEL,
                         encode_cancel_request(120U), 121U);
    ASSERT_EQ(Frames().size(), 3U);

    CloseConnectionAndCancelAll();
    ASSERT_EQ(uv_process_kill(&process, SIGKILL), 0);
    RunLoopUntil(Loop(), [&]() { return outcome.called; }, 5000);
    RunLoop();
    EXPECT_EQ(Frames().size(), 3U);
    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());

    uv_close(reinterpret_cast<uv_handle_t *>(&process), nullptr);
    RunLoop();
}

// Once updates have flowed, a failure ends the stream the way api.md §3.4
// says: a final RESPONSE with reason "error", not an ERROR frame.
TEST_F(FileHandlerTest, MonitorSampleFailureAfterAnUpdateEndsWithReasonError)
{
    uv_process_t process{};
    SpawnOutcome outcome;
    const uint32_t pid = SpawnSleeper(Loop(), &process, &outcome);
    ASSERT_NE(pid, 0U);

    DispatchRequestNoRun(taz_v1_Opcode_OPCODE_PROCESS_MONITOR,
                         encode_monitor_request(pid, 100U), 122U);
    RunLoopUntil(Loop(), [&]() { return !Frames().empty(); }, 1000);
    ASSERT_EQ(Frames().size(), 1U);

    {
        const ScopedSampleFn failing(FailingSample);
        RunLoopUntil(Loop(), [&]() { return Frames().size() > 1U; }, 5000);
    }
    ASSERT_EQ(Frames().size(), 2U);
    EXPECT_EQ(unpack_header(Frames()[1]).type,
              static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_RESPONSE));
    EXPECT_FALSE(FrameHasContinuation(Frames()[1]));
    {
        const taz_v1_ProcessMonitorResponse resp =
            decode_monitor_response(Frames()[1]);
        EXPECT_STREQ(resp.reason, "error");
        EXPECT_FALSE(resp.exited);
        EXPECT_EQ(resp.info.pid, pid);
    }

    ASSERT_EQ(uv_process_kill(&process, SIGKILL), 0);
    RunLoopUntil(Loop(), [&]() { return outcome.called; }, 5000);
    RunLoop();
    EXPECT_EQ(Frames().size(), 2U);
    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());

    uv_close(reinterpret_cast<uv_handle_t *>(&process), nullptr);
    RunLoop();
}

// Before the first update the request itself failed, so an ERROR frame
// carries the code, as it does for a nonexistent pid.
TEST_F(FileHandlerTest, MonitorSampleFailureBeforeAnyUpdateIsAnError)
{
    uv_process_t process{};
    SpawnOutcome outcome;
    const uint32_t pid = SpawnSleeper(Loop(), &process, &outcome);
    ASSERT_NE(pid, 0U);

    {
        const ScopedSampleFn failing(FailingSample);
        DispatchRequestNoRun(taz_v1_Opcode_OPCODE_PROCESS_MONITOR,
                             encode_monitor_request(pid, 100U), 124U);
        RunLoopUntil(Loop(), [&]() { return !Frames().empty(); }, 1000);
    }
    ASSERT_EQ(Frames().size(), 1U);
    EXPECT_EQ(unpack_header(Frames()[0]).type,
              static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_ERROR));
    taz_v1_ErrorInfo err = taz_v1_ErrorInfo_init_zero;
    const std::vector<uint8_t> body = frame_payload(Frames()[0]);
    pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
    ASSERT_TRUE(pb_decode(&istream, taz_v1_ErrorInfo_fields, &err));
    EXPECT_EQ(err.code, taz_v1_ErrorCode_ERROR_CODE_INTERNAL);

    ASSERT_EQ(uv_process_kill(&process, SIGKILL), 0);
    RunLoopUntil(Loop(), [&]() { return outcome.called; }, 5000);
    RunLoop();
    EXPECT_EQ(Frames().size(), 1U);
    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());

    uv_close(reinterpret_cast<uv_handle_t *>(&process), nullptr);
    RunLoop();
}

// Also covers "CANCEL targeting a monitor whose final exited frame is
// already sent": the same state check (state != MON_RUNNING) refuses both.
TEST_F(FileHandlerTest, MonitorCancelAfterNaturalExitReturnsFalse)
{
    uv_process_t process{};
    SpawnOutcome outcome;
    const uint32_t pid = SpawnSleeper(Loop(), &process, &outcome);
    ASSERT_NE(pid, 0U);

    DispatchRequestNoRun(taz_v1_Opcode_OPCODE_PROCESS_MONITOR,
                         encode_monitor_request(pid, 100U), 75U);
    RunLoopUntil(Loop(), [&]() { return !Frames().empty(); }, 1000);

    ASSERT_EQ(uv_process_kill(&process, SIGKILL), 0);
    RunLoopUntil(
        Loop(), [&]()
        { return !Frames().empty() && !FrameHasContinuation(Frames().back()); },
        5000);
    ASSERT_TRUE(decode_monitor_response(Frames().back()).exited);
    const size_t before_cancel = Frames().size();

    DispatchRequestNoRun(taz_v1_Opcode_OPCODE_CANCEL,
                         encode_cancel_request(75U), 76U);
    ASSERT_EQ(Frames().size(), before_cancel + 1U);
    ExpectCancelled(Frames().back(), false);

    RunLoop();
    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());

    uv_close(reinterpret_cast<uv_handle_t *>(&process), nullptr);
    RunLoop();
}

TEST_F(FileHandlerTest,
       MonitorCancelWhileSampleInFlightSendsCancelledThenResponse)
{
    uv_process_t process{};
    SpawnOutcome outcome;
    const uint32_t pid = SpawnSleeper(Loop(), &process, &outcome);
    ASSERT_NE(pid, 0U);

    DispatchRequestNoRun(taz_v1_Opcode_OPCODE_PROCESS_MONITOR,
                         encode_monitor_request(pid, 100U), 77U);

    // Pump without blocking until the first tick has submitted its sample:
    // RefCount rises by the step's own ref on top of the monitor's
    // permanent one (taz_work_submit_step calls taz_dispatch_conn_ref
    // synchronously, before the pool thread even starts).
    RunLoopNowaitUntil(Loop(), [&]() { return RefCount() >= 2; }, 2000);
    ASSERT_EQ(RefCount(), 2);

    DispatchRequestNoRun(taz_v1_Opcode_OPCODE_CANCEL,
                         encode_cancel_request(77U), 78U);
    EXPECT_EQ(Frames().size(), 0U); // Deferred: the sample is still in flight.

    RunLoopUntil(Loop(), [&]() { return Frames().size() >= 2U; }, 5000);
    ASSERT_EQ(Frames().size(), 2U);
    EXPECT_FALSE(FrameHasContinuation(Frames()[0]));
    {
        const taz_v1_ProcessMonitorResponse resp =
            decode_monitor_response(Frames()[0]);
        EXPECT_FALSE(resp.exited); // A pending cancel wins regardless.
        EXPECT_STREQ(resp.reason, "cancelled");
    }
    ExpectCancelled(Frames()[1], true);

    ASSERT_EQ(uv_process_kill(&process, SIGKILL), 0);
    RunLoopUntil(Loop(), [&]() { return outcome.called; }, 5000);
    RunLoop();
    EXPECT_EQ(Frames().size(),
              2U); // no update frame slipped in after the final.
    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());

    uv_close(reinterpret_cast<uv_handle_t *>(&process), nullptr);
    RunLoop();
}

// ---------------------------------------------------------------------------
// Abort (connection closing)
// ---------------------------------------------------------------------------

TEST_F(FileHandlerTest, MonitorAbortWhileSampleInFlightSendsNothingAndReleases)
{
    uv_process_t process{};
    SpawnOutcome outcome;
    const uint32_t pid = SpawnSleeper(Loop(), &process, &outcome);
    ASSERT_NE(pid, 0U);

    DispatchRequestNoRun(taz_v1_Opcode_OPCODE_PROCESS_MONITOR,
                         encode_monitor_request(pid, 100U), 79U);
    RunLoopNowaitUntil(Loop(), [&]() { return RefCount() >= 2; }, 2000);
    ASSERT_EQ(RefCount(), 2);

    CloseConnectionAndCancelAll();

    ASSERT_EQ(uv_process_kill(&process, SIGKILL), 0);
    RunLoopUntil(Loop(), [&]() { return outcome.called; }, 5000);
    RunLoop();

    EXPECT_EQ(Frames().size(), 0U);
    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());

    uv_close(reinterpret_cast<uv_handle_t *>(&process), nullptr);
    RunLoop();
}

TEST_F(FileHandlerTest, MonitorAbortWhileIdleSendsNothingAndReleases)
{
    uv_process_t process{};
    SpawnOutcome outcome;
    const uint32_t pid = SpawnSleeper(Loop(), &process, &outcome);
    ASSERT_NE(pid, 0U);

    DispatchRequestNoRun(taz_v1_Opcode_OPCODE_PROCESS_MONITOR,
                         encode_monitor_request(pid, 100U), 80U);
    RunLoopUntil(Loop(), [&]() { return !Frames().empty(); }, 1000);
    const size_t before_abort = Frames().size();

    CloseConnectionAndCancelAll();

    ASSERT_EQ(uv_process_kill(&process, SIGKILL), 0);
    RunLoopUntil(Loop(), [&]() { return outcome.called; }, 5000);
    RunLoop();

    EXPECT_EQ(Frames().size(), before_abort);
    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());

    uv_close(reinterpret_cast<uv_handle_t *>(&process), nullptr);
    RunLoop();
}

// A CANCEL accepted while a sample is in flight, with the connection then
// closing before that sample returns: the CANCEL's own stream must still
// be released (its done fired from monitor_sample_done's closing branch),
// and nothing is ever written (the final "cancelled" frame and the
// CancelResponse are both suppressed by the closing connection).
TEST_F(FileHandlerTest,
       MonitorCancelAcceptedThenAbortBeforeSampleReturnsFiresDoneWithNoFrame)
{
    uv_process_t process{};
    SpawnOutcome outcome;
    const uint32_t pid = SpawnSleeper(Loop(), &process, &outcome);
    ASSERT_NE(pid, 0U);

    DispatchRequestNoRun(taz_v1_Opcode_OPCODE_PROCESS_MONITOR,
                         encode_monitor_request(pid, 100U), 81U);
    RunLoopNowaitUntil(Loop(), [&]() { return RefCount() >= 2; }, 2000);
    ASSERT_EQ(RefCount(), 2);

    DispatchRequestNoRun(taz_v1_Opcode_OPCODE_CANCEL,
                         encode_cancel_request(81U), 82U);
    EXPECT_EQ(Frames().size(), 0U);
    ASSERT_EQ(ActiveStreamCount(), 2U); // the monitor stream + CANCEL's own.

    CloseConnectionAndCancelAll();

    ASSERT_EQ(uv_process_kill(&process, SIGKILL), 0);
    RunLoopUntil(Loop(), [&]() { return outcome.called; }, 5000);
    RunLoop();

    EXPECT_EQ(Frames().size(), 0U);
    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());

    uv_close(reinterpret_cast<uv_handle_t *>(&process), nullptr);
    RunLoop();
}

// ---------------------------------------------------------------------------
// Shutdown (SIGTERM via taz_work_request_shutdown)
// ---------------------------------------------------------------------------

TEST_F(FileHandlerTest, MonitorShutdownWhileSampleInFlightStopsLoopThenDrains)
{
    uv_process_t process{};
    SpawnOutcome outcome;
    const uint32_t pid = SpawnSleeper(Loop(), &process, &outcome);
    ASSERT_NE(pid, 0U);

    DispatchRequestNoRun(taz_v1_Opcode_OPCODE_PROCESS_MONITOR,
                         encode_monitor_request(pid, 100U), 83U);
    RunLoopNowaitUntil(Loop(), [&]() { return RefCount() >= 2; }, 2000);
    ASSERT_EQ(RefCount(), 2);

    RequestShutdown();
    const size_t before = Frames().size();
    // Not the fixture's strict RunLoop(): the sleeper is still alive, and
    // uv_run only needs to drain the one outstanding sample, not every
    // handle, before taz_work_request_shutdown's own uv_stop takes effect.
    EXPECT_GE(uv_run(Loop(), UV_RUN_DEFAULT), 0);
    EXPECT_EQ(Frames().size(),
              before); // no frame after shutdown was requested.

    ASSERT_EQ(uv_process_kill(&process, SIGKILL), 0);
    RunLoopUntil(Loop(), [&]() { return outcome.called; }, 5000);
    CloseConnectionAndCancelAll(); // safety net: idempotent if already torn
                                   // down by the shutdown drain above.
    RunLoop();

    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());

    uv_close(reinterpret_cast<uv_handle_t *>(&process), nullptr);
    RunLoop();
    taz_work_reset_for_tests();
}

TEST_F(FileHandlerTest, MonitorShutdownWhileIdleStopsLoopImmediatelyThenDrains)
{
    uv_process_t process{};
    SpawnOutcome outcome;
    const uint32_t pid = SpawnSleeper(Loop(), &process, &outcome);
    ASSERT_NE(pid, 0U);

    DispatchRequestNoRun(taz_v1_Opcode_OPCODE_PROCESS_MONITOR,
                         encode_monitor_request(pid, 100U), 84U);
    RunLoopUntil(Loop(), [&]() { return !Frames().empty(); }, 1000);
    const size_t before = Frames().size();

    // No sample in flight: taz_work_request_shutdown calls uv_stop
    // synchronously, before this uv_run call even starts - it runs zero
    // iterations (the timer never fires again).
    RequestShutdown();
    EXPECT_GE(uv_run(Loop(), UV_RUN_DEFAULT), 0);
    EXPECT_EQ(Frames().size(), before);

    ASSERT_EQ(uv_process_kill(&process, SIGKILL), 0);
    RunLoopUntil(Loop(), [&]() { return outcome.called; }, 5000);
    CloseConnectionAndCancelAll();
    RunLoop();

    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());

    uv_close(reinterpret_cast<uv_handle_t *>(&process), nullptr);
    RunLoop();
    taz_work_reset_for_tests();
}
