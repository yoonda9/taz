// Unit tests for handlers/process.c's PROCESS_LIST handler and its exported
// taz_process_list_send batch encoder, driven end to end through
// taz_dispatch_frame with a real uv_loop_t (see file_test_support.h for the
// shared fixture, also used by test_file.cpp/test_dir.cpp).

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <pb_decode.h>
#include <pb_encode.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#include <fstream>

#include <unistd.h>
#endif

#include "file_test_support.h"
#include "handlers/process.h"
#include "taz/v1/common.pb.h"
#include "taz/v1/process.pb.h"

namespace
{

uint32_t SelfPid()
{
#ifdef _WIN32
    return static_cast<uint32_t>(GetCurrentProcessId());
#else
    return static_cast<uint32_t>(getpid());
#endif
}

// Mirrors test_process_platform.cpp's ExpectedSelfName(): not hardcoded to
// "taz_tests" because under valgrind /proc/self/comm for every process is
// the wrapping tool's own name (e.g. "memcheck-amd64-"), since valgrind's
// client runs inside the tool's own process image.
std::string ExpectedSelfName()
{
#ifndef _WIN32
    std::ifstream f("/proc/self/comm");
    std::string comm;
    std::getline(f, comm);
    return comm;
#else
    return "taz_tests.exe";
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

std::vector<uint8_t> encode_process_list_request(const std::string &filter)
{
    taz_v1_ProcessListRequest req = taz_v1_ProcessListRequest_init_zero;
    if (!filter.empty())
    {
        (void)strncpy(req.filter, filter.c_str(), sizeof(req.filter) - 1U);
    }
    std::vector<uint8_t> buf(taz_v1_ProcessListRequest_size);
    pb_ostream_t ostream = pb_ostream_from_buffer(buf.data(), buf.size());
    EXPECT_TRUE(pb_encode(&ostream, taz_v1_ProcessListRequest_fields, &req));
    buf.resize(ostream.bytes_written);
    return buf;
}

// Callback-based mirror of taz_v1_ProcessListResponse with no static-array
// cap on entry count (same pattern as test_dir.cpp's DirListResponseCb): a
// single RESPONSE frame may legitimately pack more entries than one encode
// batch (128) holds, since the frame splitter only cares about byte size
// for an oversized buffer, and taz_process_list_send sends each batch as
// its own frame regardless of size - decoding with the real
// taz_v1_ProcessListResponse (array of 128) would overflow if a test ever
// fed it more.
struct ProcessListResponseCb
{
    pb_callback_t processes;
};

// clang-format off
#define ProcessListResponseCb_FIELDLIST(X, a) \
    X(a, CALLBACK, REPEATED, MESSAGE, processes, 1)
// clang-format on

#define ProcessListResponseCb_DEFAULT           NULL
#define ProcessListResponseCb_CALLBACK          pb_default_field_callback
#define ProcessListResponseCb_processes_MSGTYPE taz_v1_ProcessInfo

PB_BIND(ProcessListResponseCb, ProcessListResponseCb, AUTO)

bool collect_process_entries(pb_istream_t *stream, const pb_field_iter_t *field,
                             void **arg)
{
    (void)field;
    auto *entries = static_cast<std::vector<taz_v1_ProcessInfo> *>(*arg);
    taz_v1_ProcessInfo entry = taz_v1_ProcessInfo_init_zero;
    if (!pb_decode(stream, taz_v1_ProcessInfo_fields, &entry))
    {
        return false;
    }
    entries->push_back(entry);
    return true;
}

// Decodes every captured frame as a ProcessListResponse via the callback
// variant above and returns the concatenated entries in frame order.
std::vector<taz_v1_ProcessInfo>
decode_all_process_entries(const std::vector<std::vector<uint8_t>> &frames)
{
    std::vector<taz_v1_ProcessInfo> entries;
    for (const auto &frame : frames)
    {
        const std::vector<uint8_t> body = frame_payload(frame);
        ProcessListResponseCb resp{};
        resp.processes.funcs.decode = collect_process_entries;
        resp.processes.arg = &entries;
        pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
        EXPECT_TRUE(pb_decode(&istream, &ProcessListResponseCb_msg, &resp));
    }
    return entries;
}

bool FrameHasContinuation(const std::vector<uint8_t> &frame)
{
    return (unpack_header(frame).flags &
            static_cast<uint8_t>(taz_v1_FrameFlag_FRAME_FLAG_CONTINUATION)) !=
           0U;
}

} // namespace

// ---------------------------------------------------------------------------
// handle_process_list, via taz_dispatch_frame
// ---------------------------------------------------------------------------

TEST_F(FileHandlerTest, ProcessListEmptyFilterContainsSelfWithValidFields)
{
    DispatchRequest(taz_v1_Opcode_OPCODE_PROCESS_LIST,
                    encode_process_list_request(""), 1U);

    ASSERT_GE(Frames().size(), 1U);
    for (const auto &frame : Frames())
    {
        EXPECT_EQ(unpack_header(frame).type,
                  static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_RESPONSE));
    }

    const std::vector<taz_v1_ProcessInfo> entries =
        decode_all_process_entries(Frames());
    ASSERT_GT(entries.size(), 0U);

    const uint32_t self_pid = SelfPid();
    const taz_v1_ProcessInfo *self_entry = nullptr;
    for (size_t i = 0U; i < entries.size(); i++)
    {
        EXPECT_TRUE(IsValidUtf8(entries[i].name)) << "entry " << i;
        EXPECT_TRUE(IsValidUtf8(entries[i].user)) << "entry " << i;
        EXPECT_TRUE(IsValidUtf8(entries[i].state)) << "entry " << i;
        if (entries[i].pid == self_pid)
        {
            self_entry = &entries[i];
        }
        if (i > 0U)
        {
            EXPECT_LT(entries[i - 1U].pid, entries[i].pid)
                << "entries must be strictly ascending by pid";
        }
    }
    ASSERT_NE(self_entry, nullptr);
    EXPECT_EQ(self_entry->name, ExpectedSelfName());

    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), 1);
    EXPECT_EQ(UnrefCount(), 1);
}

TEST_F(FileHandlerTest, ProcessListFilterMatchesSelfBySubstring)
{
    // A strict (non-full-string) prefix of the expected name, so this
    // exercises substring matching rather than an exact match.
    const std::string filter = ExpectedSelfName().substr(0, 4);

    DispatchRequest(taz_v1_Opcode_OPCODE_PROCESS_LIST,
                    encode_process_list_request(filter), 2U);

    const std::vector<taz_v1_ProcessInfo> entries =
        decode_all_process_entries(Frames());
    const uint32_t self_pid = SelfPid();
    bool found = false;
    for (const auto &entry : entries)
    {
        found = found || (entry.pid == self_pid);
    }
    EXPECT_TRUE(found);
}

TEST_F(FileHandlerTest, ProcessListNoMatchFilterReturnsOneEmptyResponse)
{
    DispatchRequest(taz_v1_Opcode_OPCODE_PROCESS_LIST,
                    encode_process_list_request("no-such-7f3a"), 3U);

    ASSERT_EQ(Frames().size(), 1U);
    EXPECT_FALSE(FrameHasContinuation(Frames()[0]));
    EXPECT_EQ(decode_all_process_entries(Frames()).size(), 0U);
}

TEST_F(FileHandlerTest, ProcessListFilterOf255BytesDecodesToEmptyResponse)
{
    DispatchRequest(taz_v1_Opcode_OPCODE_PROCESS_LIST,
                    encode_process_list_request(std::string(255, 'a')), 4U);

    ASSERT_EQ(Frames().size(), 1U);
    EXPECT_EQ(decode_all_process_entries(Frames()).size(), 0U);
    EXPECT_EQ(RefCount(), 1);
    EXPECT_EQ(UnrefCount(), 1);
}

TEST_F(FileHandlerTest, ProcessListFilterOf256BytesIsInvalidRequest)
{
    // Hand-built wire bytes: field 1 (filter), wire type 2 (LEN), length
    // varint 256 (0x80 0x02), followed by 256 'a' bytes - one byte longer
    // than taz_v1_ProcessListRequest.filter's fixed array can decode
    // (max_size:256 means 255 bytes + NUL), so this exercises the overflow
    // path rather than anything taz_v1_ProcessListRequest_init_zero could
    // ever hold.
    std::vector<uint8_t> payload{0x0AU, 0x80U, 0x02U};
    payload.insert(payload.end(), 256U, static_cast<uint8_t>('a'));

    DispatchRequest(taz_v1_Opcode_OPCODE_PROCESS_LIST, payload, 5U);

    ASSERT_EQ(Frames().size(), 1U);
    taz_v1_ErrorInfo err = taz_v1_ErrorInfo_init_zero;
    const std::vector<uint8_t> body = frame_payload(Frames()[0]);
    pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
    ASSERT_TRUE(pb_decode(&istream, taz_v1_ErrorInfo_fields, &err));
    EXPECT_EQ(err.code, taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST);

    EXPECT_EQ(RefCount(), 0);
    EXPECT_EQ(ActiveStreamCount(), 0U);
}

TEST_F(FileHandlerTest, ProcessListUndecodablePayloadIsInvalidRequest)
{
    const std::vector<uint8_t> payload{0x0AU, 0xC8U, 0x01U};

    DispatchRequest(taz_v1_Opcode_OPCODE_PROCESS_LIST, payload, 6U);

    ASSERT_EQ(Frames().size(), 1U);
    taz_v1_ErrorInfo err = taz_v1_ErrorInfo_init_zero;
    const std::vector<uint8_t> body = frame_payload(Frames()[0]);
    pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
    ASSERT_TRUE(pb_decode(&istream, taz_v1_ErrorInfo_fields, &err));
    EXPECT_EQ(err.code, taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST);

    EXPECT_EQ(RefCount(), 0);
    EXPECT_EQ(ActiveStreamCount(), 0U);
}

TEST_F(FileHandlerTest, ProcessListConnectionClosingWhileInFlightSendsNothing)
{
    DispatchRequest(taz_v1_Opcode_OPCODE_PROCESS_LIST,
                    encode_process_list_request(""), 7U,
                    [this]() { SetConnClosing(1); });

    EXPECT_EQ(Frames().size(), 0U);
    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), 1);
    EXPECT_EQ(UnrefCount(), 1);
}

TEST_F(FileHandlerTest, ProcessListShutdownRequestedWhileInFlightSendsNothing)
{
    DispatchRequest(taz_v1_Opcode_OPCODE_PROCESS_LIST,
                    encode_process_list_request(""), 8U,
                    [this]() { RequestShutdown(); });

    EXPECT_EQ(Frames().size(), 0U);
    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());

    taz_work_reset_for_tests();
}

// ---------------------------------------------------------------------------
// taz_process_list_send, called directly with synthetic entries (no live
// processes needed to exercise the batching/framing boundaries)
// ---------------------------------------------------------------------------

namespace
{

std::vector<taz_process_entry_t> MakeSyntheticEntries(size_t count)
{
    std::vector<taz_process_entry_t> entries(count);
    for (size_t i = 0U; i < count; i++)
    {
        taz_process_entry_t &e = entries[i];
        std::memset(&e, 0, sizeof(e));
        e.pid = static_cast<uint32_t>(i + 1U);
        (void)std::snprintf(e.name, sizeof(e.name), "p%04zu", i + 1U);
        (void)strncpy(e.user, "user", sizeof(e.user) - 1U);
        (void)strncpy(e.state, "running", sizeof(e.state) - 1U);
        e.cpu_percent = 0.0F;
        e.memory_bytes = 0U;
    }
    return entries;
}

} // namespace

TEST(ProcessListSend, ThreeHundredEntriesSpanAtLeastThreeFramesInOrder)
{
    const std::vector<taz_process_entry_t> entries = MakeSyntheticEntries(300U);
    WriteCtx wctx;

    taz_process_list_send(
        capture_write, &wctx, 42U,
        static_cast<uint16_t>(taz_v1_Opcode_OPCODE_PROCESS_LIST),
        entries.data(), entries.size());

    ASSERT_GE(wctx.frames.size(), 3U);
    for (size_t fi = 0U; fi < wctx.frames.size(); fi++)
    {
        const bool is_last = (fi + 1U == wctx.frames.size());
        EXPECT_EQ(FrameHasContinuation(wctx.frames[fi]), !is_last)
            << "frame " << fi;
    }

    const std::vector<taz_v1_ProcessInfo> decoded =
        decode_all_process_entries(wctx.frames);
    ASSERT_EQ(decoded.size(), 300U);
    for (size_t fi = 0U; fi < wctx.frames.size(); fi++)
    {
        ProcessListResponseCb resp{};
        std::vector<taz_v1_ProcessInfo> frame_entries;
        resp.processes.funcs.decode = collect_process_entries;
        resp.processes.arg = &frame_entries;
        const std::vector<uint8_t> body = frame_payload(wctx.frames[fi]);
        pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
        ASSERT_TRUE(pb_decode(&istream, &ProcessListResponseCb_msg, &resp));
        EXPECT_LE(frame_entries.size(), 128U) << "frame " << fi;
    }
    for (size_t i = 0U; i < decoded.size(); i++)
    {
        char expected[16];
        (void)std::snprintf(expected, sizeof(expected), "p%04zu", i + 1U);
        EXPECT_STREQ(decoded[i].name, expected) << "entry " << i;
        EXPECT_EQ(decoded[i].pid, static_cast<uint32_t>(i + 1U))
            << "entry " << i;
    }
}

TEST(ProcessListSend, ExactlyOneBatchYieldsOneFrame)
{
    const std::vector<taz_process_entry_t> entries = MakeSyntheticEntries(128U);
    WriteCtx wctx;

    taz_process_list_send(
        capture_write, &wctx, 1U,
        static_cast<uint16_t>(taz_v1_Opcode_OPCODE_PROCESS_LIST),
        entries.data(), entries.size());

    ASSERT_EQ(wctx.frames.size(), 1U);
    EXPECT_FALSE(FrameHasContinuation(wctx.frames[0]));
    EXPECT_EQ(decode_all_process_entries(wctx.frames).size(), 128U);
}

TEST(ProcessListSend, OneOverABatchYieldsTwoFrames)
{
    const std::vector<taz_process_entry_t> entries = MakeSyntheticEntries(129U);
    WriteCtx wctx;

    taz_process_list_send(
        capture_write, &wctx, 1U,
        static_cast<uint16_t>(taz_v1_Opcode_OPCODE_PROCESS_LIST),
        entries.data(), entries.size());

    ASSERT_EQ(wctx.frames.size(), 2U);
    EXPECT_TRUE(FrameHasContinuation(wctx.frames[0]));
    EXPECT_FALSE(FrameHasContinuation(wctx.frames[1]));
    EXPECT_EQ(decode_all_process_entries(wctx.frames).size(), 129U);
}

TEST(ProcessListSend, ZeroEntriesYieldsOneEmptyFrame)
{
    WriteCtx wctx;

    taz_process_list_send(
        capture_write, &wctx, 1U,
        static_cast<uint16_t>(taz_v1_Opcode_OPCODE_PROCESS_LIST), nullptr, 0U);

    ASSERT_EQ(wctx.frames.size(), 1U);
    EXPECT_FALSE(FrameHasContinuation(wctx.frames[0]));
    EXPECT_EQ(decode_all_process_entries(wctx.frames).size(), 0U);
}

// ---------------------------------------------------------------------------
// PROCESS_KILL and PROCESS_INFO handlers, via taz_dispatch_frame
// ---------------------------------------------------------------------------

namespace
{

std::vector<uint8_t> encode_process_kill_request(uint32_t pid, int32_t signal)
{
    taz_v1_ProcessKillRequest req = taz_v1_ProcessKillRequest_init_zero;
    req.pid = pid;
    req.signal = signal;
    std::vector<uint8_t> buf(taz_v1_ProcessKillRequest_size);
    pb_ostream_t ostream = pb_ostream_from_buffer(buf.data(), buf.size());
    EXPECT_TRUE(pb_encode(&ostream, taz_v1_ProcessKillRequest_fields, &req));
    buf.resize(ostream.bytes_written);
    return buf;
}

std::vector<uint8_t> encode_process_info_request(uint32_t pid)
{
    taz_v1_ProcessInfoRequest req = taz_v1_ProcessInfoRequest_init_zero;
    req.pid = pid;
    std::vector<uint8_t> buf(taz_v1_ProcessInfoRequest_size);
    pb_ostream_t ostream = pb_ostream_from_buffer(buf.data(), buf.size());
    EXPECT_TRUE(pb_encode(&ostream, taz_v1_ProcessInfoRequest_fields, &req));
    buf.resize(ostream.bytes_written);
    return buf;
}

uint32_t read_pid_max()
{
#ifdef _WIN32
    return 0x7FFFFFFCU;
#else
    FILE *f = fopen("/proc/sys/kernel/pid_max", "r");
    uint32_t pid_max = 32768;
    if (f != NULL)
    {
        char buf[64];
        if (fgets(buf, sizeof(buf), f) != NULL)
        {
            char *end;
            const long val = strtol(buf, &end, 10);
            if (end != buf && val > 0 && val < INT_MAX)
            {
                pid_max = static_cast<uint32_t>(val);
            }
        }
        (void)fclose(f);
    }
    return pid_max;
#endif
}

// Mirrors test_process_platform.cpp's SpawnSleeper/OnSleeperExit/
// SpawnOutcome (duplicated, not shared, like that file's own
// ExpectedSelfName/SelfPid - this TU is a separate translation unit, so an
// anonymous-namespace name clash across the two is not possible): spawns
// taz_test_sleeper (60 s, no shell) so handle_process_kill has a real,
// harmless child to terminate through taz_dispatch_frame.
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

} // namespace

TEST_F(FileHandlerTest, ProcessKillPidZeroIsInvalidRequest)
{
    // Validation must reject pid 0 before taz_process_kill ever runs: on
    // POSIX, kill(0, sig) signals the whole process group, which includes
    // this test binary and the ralph loop that launched it. Using SIGCONT
    // here (harmless even if delivered) instead of 0 - which
    // taz_process_kill maps to SIGTERM - means a validation-order
    // regression stops this process briefly rather than killing the group.
#ifdef _WIN32
    const int32_t harmless_signal = 0;
#else
    const int32_t harmless_signal = SIGCONT;
#endif
    const std::vector<uint8_t> payload =
        encode_process_kill_request(0U, harmless_signal);

    DispatchRequest(taz_v1_Opcode_OPCODE_PROCESS_KILL, payload, 10U);

    ASSERT_EQ(Frames().size(), 1U);
    taz_v1_ErrorInfo err = taz_v1_ErrorInfo_init_zero;
    const std::vector<uint8_t> body = frame_payload(Frames()[0]);
    pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
    ASSERT_TRUE(pb_decode(&istream, taz_v1_ErrorInfo_fields, &err));
    EXPECT_EQ(err.code, taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST);

    EXPECT_EQ(RefCount(), 0);
    EXPECT_EQ(ActiveStreamCount(), 0U);
}

TEST_F(FileHandlerTest, ProcessKillPidTooLargeIsInvalidRequest)
{
    const std::vector<uint8_t> payload =
        encode_process_kill_request(2147483648U, 0);

    DispatchRequest(taz_v1_Opcode_OPCODE_PROCESS_KILL, payload, 11U);

    ASSERT_EQ(Frames().size(), 1U);
    taz_v1_ErrorInfo err = taz_v1_ErrorInfo_init_zero;
    const std::vector<uint8_t> body = frame_payload(Frames()[0]);
    pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
    ASSERT_TRUE(pb_decode(&istream, taz_v1_ErrorInfo_fields, &err));
    EXPECT_EQ(err.code, taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST);

    EXPECT_EQ(RefCount(), 0);
}

TEST_F(FileHandlerTest, ProcessKillImpossiblePidReturnsNotFound)
{
    const uint32_t pid_max = read_pid_max();
    const uint32_t impossible_pid =
        (pid_max >= (uint32_t)INT32_MAX) ? (uint32_t)INT32_MAX : (pid_max + 1U);

    const std::vector<uint8_t> payload =
        encode_process_kill_request(impossible_pid, 0);

    DispatchRequest(taz_v1_Opcode_OPCODE_PROCESS_KILL, payload, 12U);

    ASSERT_EQ(Frames().size(), 1U);
    taz_v1_ErrorInfo err = taz_v1_ErrorInfo_init_zero;
    const std::vector<uint8_t> body = frame_payload(Frames()[0]);
    pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
    ASSERT_TRUE(pb_decode(&istream, taz_v1_ErrorInfo_fields, &err));
    EXPECT_EQ(err.code, taz_v1_ErrorCode_ERROR_CODE_NOT_FOUND);

    EXPECT_EQ(RefCount(), 0);
}

TEST_F(FileHandlerTest, ProcessKillMaxInt32PidReturnsNotFound)
{
    // Exact boundary for the pid > INT32_MAX validation: 2147483647 must
    // pass validation (it does not exceed INT32_MAX) and reach
    // taz_process_kill, which reports NOT_FOUND since no such process
    // exists. ProcessKillImpossiblePidReturnsNotFound alone cannot catch an
    // off-by-one (e.g. >=) here, since pid_max + 1 is far below INT32_MAX
    // on every real host.
    const std::vector<uint8_t> payload =
        encode_process_kill_request(2147483647U, 0);

    DispatchRequest(taz_v1_Opcode_OPCODE_PROCESS_KILL, payload, 15U);

    ASSERT_EQ(Frames().size(), 1U);
    taz_v1_ErrorInfo err = taz_v1_ErrorInfo_init_zero;
    const std::vector<uint8_t> body = frame_payload(Frames()[0]);
    pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
    ASSERT_TRUE(pb_decode(&istream, taz_v1_ErrorInfo_fields, &err));
    EXPECT_EQ(err.code, taz_v1_ErrorCode_ERROR_CODE_NOT_FOUND);

    EXPECT_EQ(RefCount(), 0);
}

TEST_F(FileHandlerTest, ProcessKillNegativeSignalIsInvalidRequest)
{
    const uint32_t self_pid = SelfPid();
    const std::vector<uint8_t> payload =
        encode_process_kill_request(self_pid, -1);

    DispatchRequest(taz_v1_Opcode_OPCODE_PROCESS_KILL, payload, 13U);

    ASSERT_EQ(Frames().size(), 1U);
    taz_v1_ErrorInfo err = taz_v1_ErrorInfo_init_zero;
    const std::vector<uint8_t> body = frame_payload(Frames()[0]);
    pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
    ASSERT_TRUE(pb_decode(&istream, taz_v1_ErrorInfo_fields, &err));
    EXPECT_EQ(err.code, taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST);

    EXPECT_EQ(RefCount(), 0);
}

#ifndef _WIN32
TEST_F(FileHandlerTest, ProcessKillInvalidSignalIsInvalidRequest)
{
    const uint32_t self_pid = SelfPid();
    const std::vector<uint8_t> payload =
        encode_process_kill_request(self_pid, 999999);

    DispatchRequest(taz_v1_Opcode_OPCODE_PROCESS_KILL, payload, 14U);

    ASSERT_EQ(Frames().size(), 1U);
    taz_v1_ErrorInfo err = taz_v1_ErrorInfo_init_zero;
    const std::vector<uint8_t> body = frame_payload(Frames()[0]);
    pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
    ASSERT_TRUE(pb_decode(&istream, taz_v1_ErrorInfo_fields, &err));
    EXPECT_EQ(err.code, taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST);

    EXPECT_EQ(RefCount(), 0);
}
#endif

TEST_F(FileHandlerTest, ProcessKillUndecodablePayloadIsInvalidRequest)
{
    // Payload: field 1 as string type (0x0A = field 1 << 3 | 2) with length
    // 255 (0xFF), but no actual string data - decode will fail.
    const std::vector<uint8_t> payload{0x0AU, 0xFFU};

    DispatchRequest(taz_v1_Opcode_OPCODE_PROCESS_KILL, payload, 16U);

    ASSERT_EQ(Frames().size(), 1U);
    taz_v1_ErrorInfo err = taz_v1_ErrorInfo_init_zero;
    const std::vector<uint8_t> body = frame_payload(Frames()[0]);
    pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
    ASSERT_TRUE(pb_decode(&istream, taz_v1_ErrorInfo_fields, &err));
    EXPECT_EQ(err.code, taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST);

    EXPECT_EQ(RefCount(), 0);
}

// KILL is inline (no taz_work_submit), so terminating a real sleeper must
// reply with a RESPONSE before taz_dispatch_frame even returns - RefCount()
// stays 0 throughout, unlike every PROCESS_LIST/_INFO success path. Reusing
// the sleeper after reaping (never uv_close'd until the end) then exercises
// the plan's "kill again -> NOT_FOUND" and "info -> NOT_FOUND" rows through
// the real handlers, not just taz_process_kill/taz_process_inspect directly
// (already covered at the platform level by
// ProcessSpawnTest.KillZeroSendsTerminateAndReportsNotFoundAfterReap).
TEST_F(FileHandlerTest, ProcessKillSleeperSucceedsThenNotFoundAfterReap)
{
    uv_process_t process{};
    SpawnOutcome outcome;
    const uint32_t pid = SpawnSleeper(Loop(), &process, &outcome);
    ASSERT_NE(pid, 0U);

    DispatchRequest(taz_v1_Opcode_OPCODE_PROCESS_KILL,
                    encode_process_kill_request(pid, 0), 21U);

    ASSERT_EQ(Frames().size(), 1U);
    EXPECT_EQ(unpack_header(Frames()[0]).type,
              static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_RESPONSE));
    {
        taz_v1_ProcessKillResponse resp = taz_v1_ProcessKillResponse_init_zero;
        const std::vector<uint8_t> body = frame_payload(Frames()[0]);
        pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
        ASSERT_TRUE(
            pb_decode(&istream, taz_v1_ProcessKillResponse_fields, &resp));
        EXPECT_TRUE(resp.success);
    }
    EXPECT_EQ(RefCount(), 0);
    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_TRUE(outcome.called);

    DispatchRequest(taz_v1_Opcode_OPCODE_PROCESS_KILL,
                    encode_process_kill_request(pid, 0), 22U);
    ASSERT_EQ(Frames().size(), 2U);
    {
        taz_v1_ErrorInfo err = taz_v1_ErrorInfo_init_zero;
        const std::vector<uint8_t> body = frame_payload(Frames()[1]);
        pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
        ASSERT_TRUE(pb_decode(&istream, taz_v1_ErrorInfo_fields, &err));
        EXPECT_EQ(err.code, taz_v1_ErrorCode_ERROR_CODE_NOT_FOUND);
    }

    DispatchRequest(taz_v1_Opcode_OPCODE_PROCESS_INFO,
                    encode_process_info_request(pid), 23U);
    ASSERT_EQ(Frames().size(), 3U);
    {
        taz_v1_ErrorInfo err = taz_v1_ErrorInfo_init_zero;
        const std::vector<uint8_t> body = frame_payload(Frames()[2]);
        pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
        ASSERT_TRUE(pb_decode(&istream, taz_v1_ErrorInfo_fields, &err));
        EXPECT_EQ(err.code, taz_v1_ErrorCode_ERROR_CODE_NOT_FOUND);
    }

    uv_close(reinterpret_cast<uv_handle_t *>(&process), nullptr);
    RunLoop();
}

TEST_F(FileHandlerTest, ProcessInfoPidZeroIsInvalidRequest)
{
    const std::vector<uint8_t> payload = encode_process_info_request(0U);

    DispatchRequest(taz_v1_Opcode_OPCODE_PROCESS_INFO, payload, 17U);

    ASSERT_EQ(Frames().size(), 1U);
    taz_v1_ErrorInfo err = taz_v1_ErrorInfo_init_zero;
    const std::vector<uint8_t> body = frame_payload(Frames()[0]);
    pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
    ASSERT_TRUE(pb_decode(&istream, taz_v1_ErrorInfo_fields, &err));
    EXPECT_EQ(err.code, taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST);

    EXPECT_EQ(RefCount(), 0);
    EXPECT_EQ(ActiveStreamCount(), 0U);
}

TEST_F(FileHandlerTest, ProcessInfoImpossiblePidReturnsNotFound)
{
    const uint32_t pid_max = read_pid_max();
    const uint32_t impossible_pid =
        (pid_max >= (uint32_t)INT32_MAX) ? (uint32_t)INT32_MAX : (pid_max + 1U);

    const std::vector<uint8_t> payload =
        encode_process_info_request(impossible_pid);

    DispatchRequest(taz_v1_Opcode_OPCODE_PROCESS_INFO, payload, 18U);

    ASSERT_GE(Frames().size(), 1U);
    taz_v1_ErrorInfo err = taz_v1_ErrorInfo_init_zero;
    const std::vector<uint8_t> body = frame_payload(Frames()[0]);
    pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
    ASSERT_TRUE(pb_decode(&istream, taz_v1_ErrorInfo_fields, &err));
    EXPECT_EQ(err.code, taz_v1_ErrorCode_ERROR_CODE_NOT_FOUND);

    EXPECT_EQ(ActiveStreamCount(), 0U);
}

TEST_F(FileHandlerTest, ProcessInfoUndecodablePayloadIsInvalidRequest)
{
    // Payload: field 1 as string type (0x0A = field 1 << 3 | 2) with length
    // 255 (0xFF), but no actual string data - decode will fail.
    const std::vector<uint8_t> payload{0x0AU, 0xFFU};

    DispatchRequest(taz_v1_Opcode_OPCODE_PROCESS_INFO, payload, 20U);

    ASSERT_EQ(Frames().size(), 1U);
    taz_v1_ErrorInfo err = taz_v1_ErrorInfo_init_zero;
    const std::vector<uint8_t> body = frame_payload(Frames()[0]);
    pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
    ASSERT_TRUE(pb_decode(&istream, taz_v1_ErrorInfo_fields, &err));
    EXPECT_EQ(err.code, taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST);

    EXPECT_EQ(RefCount(), 0);
}

TEST_F(FileHandlerTest, ProcessInfoSelfReturnsRealDetail)
{
    const std::string scratch = JoinDir("process_info_open_file.txt");
    WriteFile(scratch, "x");

    uv_fs_t open_req;
    const uv_file fd = uv_fs_open(nullptr, &open_req, scratch.c_str(),
                                  UV_FS_O_RDONLY, 0, nullptr);
    uv_fs_req_cleanup(&open_req);
    ASSERT_GE(fd, 0);

    const uint64_t before = static_cast<uint64_t>(::time(nullptr));
    DispatchRequest(taz_v1_Opcode_OPCODE_PROCESS_INFO,
                    encode_process_info_request(SelfPid()), 26U);
    const uint64_t after = static_cast<uint64_t>(::time(nullptr));

    ASSERT_GE(Frames().size(), 1U);

    taz_v1_ProcessInfo info = taz_v1_ProcessInfo_init_zero;
    std::string command_line;
    uint64_t start_time = 0U;
    std::vector<std::string> open_files;

    for (const auto &frame : Frames())
    {
        taz_v1_ProcessInfoResponse resp = taz_v1_ProcessInfoResponse_init_zero;
        const std::vector<uint8_t> body = frame_payload(frame);
        pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
        ASSERT_TRUE(
            pb_decode(&istream, taz_v1_ProcessInfoResponse_fields, &resp));
        if (resp.info.pid != 0U)
        {
            info = resp.info;
        }
        if (resp.command_line[0] != '\0')
        {
            command_line = resp.command_line;
        }
        if (resp.start_time != 0U)
        {
            start_time = resp.start_time;
        }
        for (pb_size_t i = 0U; i < resp.open_files_count; i++)
        {
            open_files.emplace_back(resp.open_files[i]);
        }
    }

    EXPECT_EQ(info.pid, SelfPid());
    EXPECT_EQ(std::string(info.name), ExpectedSelfName());
    EXPECT_NE(command_line.find("taz_tests"), std::string::npos);
    EXPECT_GE(start_time, (before >= 60U) ? (before - 60U) : 0U);
    EXPECT_LE(start_time, after);

#ifndef _WIN32
    bool found_scratch = false;
    for (const auto &f : open_files)
    {
        if (f.find("process_info_open_file.txt") != std::string::npos)
        {
            found_scratch = true;
            break;
        }
    }
    EXPECT_TRUE(found_scratch);
#else
    EXPECT_EQ(open_files.size(), 0U);
#endif

    uv_fs_t close_req;
    (void)uv_fs_close(nullptr, &close_req, fd, nullptr);
    uv_fs_req_cleanup(&close_req);

    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), 1);
    EXPECT_EQ(UnrefCount(), 1);
}

TEST_F(FileHandlerTest, ProcessInfoConnectionClosingWhileInFlightSendsNothing)
{
    DispatchRequest(taz_v1_Opcode_OPCODE_PROCESS_INFO,
                    encode_process_info_request(SelfPid()), 24U,
                    [this]() { SetConnClosing(1); });

    EXPECT_EQ(Frames().size(), 0U);
    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), 1);
    EXPECT_EQ(UnrefCount(), 1);
}

TEST_F(FileHandlerTest, ProcessInfoShutdownRequestedWhileInFlightSendsNothing)
{
    DispatchRequest(taz_v1_Opcode_OPCODE_PROCESS_INFO,
                    encode_process_info_request(SelfPid()), 25U,
                    [this]() { RequestShutdown(); });

    EXPECT_EQ(Frames().size(), 0U);
    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());

    taz_work_reset_for_tests();
}

namespace
{

// Builds a synthetic detail with open_files_count entries ("/path/to/file_N"
// each), a fixed command_line/start_time, freeable via
// taz_process_detail_free like a real one.
taz_process_detail_t MakeSyntheticDetail(size_t open_files_count)
{
    taz_process_detail_t detail = taz_process_detail_t();
    detail.info.pid = 42U;
    (void)strncpy(detail.info.name, "test", sizeof(detail.info.name) - 1U);
    (void)strncpy(detail.info.user, "user", sizeof(detail.info.user) - 1U);
    (void)strncpy(detail.info.state, "running", sizeof(detail.info.state) - 1U);
    detail.info.cpu_percent = 1.5F;
    detail.info.memory_bytes = 1024U;

    detail.command_line = (char *)malloc(256U);
    if (detail.command_line != NULL)
    {
        (void)strncpy(detail.command_line, "/bin/test arg1 arg2", 256U - 1U);
    }

    detail.start_time = 1234567890U;

    detail.open_files_count = open_files_count;
    if (open_files_count > 0U)
    {
        detail.open_files = (char (*)[1024])malloc(
            open_files_count * sizeof(detail.open_files[0]));
        if (detail.open_files != NULL)
        {
            for (size_t i = 0U; i < open_files_count; i++)
            {
                (void)snprintf(detail.open_files[i], 1024U, "/path/to/file_%zu",
                               i);
            }
        }
    }
    return detail;
}

} // namespace

TEST(ProcessInfoSend, OpenFilesAreSentInBatchesOfThirtyTwo)
{
    /* 70 open files, to test batching across 3 frames (32 + 32 + 6). */
    taz_process_detail_t detail = MakeSyntheticDetail(70U);

    WriteCtx wctx;
    taz_process_info_send(
        capture_write, &wctx, 42U,
        static_cast<uint16_t>(taz_v1_Opcode_OPCODE_PROCESS_INFO), &detail);

    /* Expect 3 frames: 32 files, 32 files, 6 files. */
    ASSERT_EQ(wctx.frames.size(), 3U);
    EXPECT_TRUE(FrameHasContinuation(wctx.frames[0]));
    EXPECT_TRUE(FrameHasContinuation(wctx.frames[1]));
    EXPECT_FALSE(FrameHasContinuation(wctx.frames[2]));

    /* Verify the total count and start_time in the last frame. */
    {
        taz_v1_ProcessInfoResponse resp = taz_v1_ProcessInfoResponse_init_zero;
        const std::vector<uint8_t> body = frame_payload(wctx.frames[2]);
        pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
        ASSERT_TRUE(
            pb_decode(&istream, taz_v1_ProcessInfoResponse_fields, &resp));
        EXPECT_EQ(resp.start_time, 1234567890U);
    }

    taz_process_detail_free(&detail);
}

TEST(ProcessInfoSend, ExactlyOneBatchYieldsOneFrame)
{
    taz_process_detail_t detail = MakeSyntheticDetail(32U);

    WriteCtx wctx;
    taz_process_info_send(
        capture_write, &wctx, 1U,
        static_cast<uint16_t>(taz_v1_Opcode_OPCODE_PROCESS_INFO), &detail);

    ASSERT_EQ(wctx.frames.size(), 1U);
    EXPECT_FALSE(FrameHasContinuation(wctx.frames[0]));

    taz_v1_ProcessInfoResponse resp = taz_v1_ProcessInfoResponse_init_zero;
    const std::vector<uint8_t> body = frame_payload(wctx.frames[0]);
    pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
    ASSERT_TRUE(pb_decode(&istream, taz_v1_ProcessInfoResponse_fields, &resp));
    EXPECT_EQ(resp.open_files_count, 32U);
    EXPECT_EQ(resp.info.pid, 42U);
    EXPECT_EQ(resp.start_time, 1234567890U);

    taz_process_detail_free(&detail);
}

TEST(ProcessInfoSend, OneOverABatchYieldsTwoFrames)
{
    taz_process_detail_t detail = MakeSyntheticDetail(33U);

    WriteCtx wctx;
    taz_process_info_send(
        capture_write, &wctx, 1U,
        static_cast<uint16_t>(taz_v1_Opcode_OPCODE_PROCESS_INFO), &detail);

    ASSERT_EQ(wctx.frames.size(), 2U);
    EXPECT_TRUE(FrameHasContinuation(wctx.frames[0]));
    EXPECT_FALSE(FrameHasContinuation(wctx.frames[1]));

    taz_v1_ProcessInfoResponse first = taz_v1_ProcessInfoResponse_init_zero;
    {
        const std::vector<uint8_t> body = frame_payload(wctx.frames[0]);
        pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
        ASSERT_TRUE(
            pb_decode(&istream, taz_v1_ProcessInfoResponse_fields, &first));
    }
    taz_v1_ProcessInfoResponse second = taz_v1_ProcessInfoResponse_init_zero;
    {
        const std::vector<uint8_t> body = frame_payload(wctx.frames[1]);
        pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
        ASSERT_TRUE(
            pb_decode(&istream, taz_v1_ProcessInfoResponse_fields, &second));
    }
    EXPECT_EQ(first.open_files_count, 32U);
    EXPECT_EQ(second.open_files_count, 1U);
    // Strings are concatenated across frames, so command_line is sent once,
    // first; scalars come from the final frame, so info and start_time are
    // sent last (protocol §6.1).
    EXPECT_STREQ(first.command_line, "/bin/test arg1 arg2");
    EXPECT_STREQ(second.command_line, "");
    EXPECT_FALSE(first.has_info);
    EXPECT_EQ(first.start_time, 0U);
    ASSERT_TRUE(second.has_info);
    EXPECT_EQ(second.info.pid, 42U);
    EXPECT_EQ(second.start_time, 1234567890U);

    taz_process_detail_free(&detail);
}

TEST(ProcessInfoSend, ZeroOpenFilesYieldsOneFrame)
{
    taz_process_detail_t detail = MakeSyntheticDetail(0U);

    WriteCtx wctx;
    taz_process_info_send(
        capture_write, &wctx, 1U,
        static_cast<uint16_t>(taz_v1_Opcode_OPCODE_PROCESS_INFO), &detail);

    ASSERT_EQ(wctx.frames.size(), 1U);
    EXPECT_FALSE(FrameHasContinuation(wctx.frames[0]));

    taz_v1_ProcessInfoResponse resp = taz_v1_ProcessInfoResponse_init_zero;
    const std::vector<uint8_t> body = frame_payload(wctx.frames[0]);
    pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
    ASSERT_TRUE(pb_decode(&istream, taz_v1_ProcessInfoResponse_fields, &resp));
    EXPECT_EQ(resp.open_files_count, 0U);
    EXPECT_EQ(resp.info.pid, 42U);
    EXPECT_EQ(resp.start_time, 1234567890U);
    EXPECT_STREQ(resp.command_line, "/bin/test arg1 arg2");

    taz_process_detail_free(&detail);
}
