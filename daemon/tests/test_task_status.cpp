// Unit tests for handlers/task_status.c: TASK_STATUS (0x0044) non-subscribed
// polling (result split via taz_response_send, error attached on FAILED,
// NOT_FOUND for an unknown task_id) and the subscribed stream (interval
// clamp, CONTINUATION-tagged updates that never carry result, a final
// frame on a terminal state, CANCEL, and the close/free/evict races every
// other stream in this daemon must survive).

#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <pb_decode.h>
#include <pb_encode.h>
#include <uv.h>

#include "file_test_support.h"
#include "handlers/task_status.h"
#include "taz/task.h"
#include "taz/task_registry.h"
#include "taz/v1/advanced.pb.h"
#include "taz/v1/command.pb.h"
#include "taz/v1/common.pb.h"

namespace
{

// ---------------------------------------------------------------------------
// Request builders / response decoders
// ---------------------------------------------------------------------------

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

// Heap-allocated: the real struct is ~39 KiB (see handlers/detach.c).
std::vector<uint8_t>
DetachExecRequestBytes(const std::string &command,
                       const std::vector<std::string> &args = {},
                       bool monitor = true)
{
    auto req = std::make_unique<taz_v1_DetachRequest>();
    (void)std::memset(req.get(), 0, sizeof(*req));
    req->which_task = taz_v1_DetachRequest_exec_tag;
    (void)std::strncpy(req->task.exec.command, command.c_str(),
                       sizeof(req->task.exec.command) - 1U);
    req->task.exec.args_count = static_cast<pb_size_t>(args.size());
    for (size_t i = 0U; i < args.size(); i++)
    {
        (void)std::strncpy(req->task.exec.args[i], args[i].c_str(),
                           sizeof(req->task.exec.args[i]) - 1U);
    }
    req->monitor = monitor;

    std::vector<uint8_t> buf(taz_v1_DetachRequest_size);
    pb_ostream_t ostream = pb_ostream_from_buffer(buf.data(), buf.size());
    EXPECT_TRUE(pb_encode(&ostream, taz_v1_DetachRequest_fields, req.get()));
    buf.resize(ostream.bytes_written);
    return buf;
}

std::vector<uint8_t> TaskStatusRequestBytes(uint32_t task_id,
                                            bool subscribe = false,
                                            uint32_t interval_ms = 0U)
{
    taz_v1_TaskStatusRequest req = taz_v1_TaskStatusRequest_init_zero;
    req.task_id = task_id;
    req.subscribe = subscribe;
    req.interval_ms = interval_ms;

    std::vector<uint8_t> buf(taz_v1_TaskStatusRequest_size);
    pb_ostream_t ostream = pb_ostream_from_buffer(buf.data(), buf.size());
    EXPECT_TRUE(pb_encode(&ostream, taz_v1_TaskStatusRequest_fields, &req));
    buf.resize(ostream.bytes_written);
    return buf;
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

bool DecodeDetachResponse(const std::vector<uint8_t> &frame,
                          taz_v1_DetachResponse *out)
{
    const taz_frame_header_t h = unpack_header(frame);
    if (h.type != static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_RESPONSE))
    {
        return false;
    }
    const std::vector<uint8_t> payload = frame_payload(frame);
    pb_istream_t stream =
        pb_istream_from_buffer(payload.data(), payload.size());
    return pb_decode(&stream, taz_v1_DetachResponse_fields, out);
}

// out must already have any callback fields (e.g. result.funcs.decode) wired
// up by the caller - mirrors test_response.cpp's multi-frame decode pattern.
bool DecodeTaskStatusPayload(const std::vector<uint8_t> &frame,
                             taz_v1_TaskStatusResponse *out)
{
    const taz_frame_header_t h = unpack_header(frame);
    if (h.type != static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_RESPONSE))
    {
        return false;
    }
    const std::vector<uint8_t> payload = frame_payload(frame);
    pb_istream_t stream =
        pb_istream_from_buffer(payload.data(), payload.size());
    return pb_decode(&stream, taz_v1_TaskStatusResponse_fields, out);
}

bool DecodeCancelResponse(const std::vector<uint8_t> &frame,
                          taz_v1_CancelResponse *out)
{
    const taz_frame_header_t h = unpack_header(frame);
    if (h.type != static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_RESPONSE))
    {
        return false;
    }
    const std::vector<uint8_t> payload = frame_payload(frame);
    pb_istream_t stream =
        pb_istream_from_buffer(payload.data(), payload.size());
    return pb_decode(&stream, taz_v1_CancelResponse_fields, out);
}

bool DecodeErrorInfo(const std::vector<uint8_t> &frame, taz_v1_ErrorInfo *out)
{
    const taz_frame_header_t h = unpack_header(frame);
    if (h.type != static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_ERROR))
    {
        return false;
    }
    const std::vector<uint8_t> payload = frame_payload(frame);
    pb_istream_t stream =
        pb_istream_from_buffer(payload.data(), payload.size());
    return pb_decode(&stream, taz_v1_ErrorInfo_fields, out);
}

bool DecodeCommandExecResponseBytes(const std::vector<uint8_t> &raw,
                                    taz_v1_CommandExecResponse *out)
{
    pb_istream_t stream = pb_istream_from_buffer(raw.data(), raw.size());
    return pb_decode(&stream, taz_v1_CommandExecResponse_fields, out);
}

bool FrameHasContinuation(const std::vector<uint8_t> &frame)
{
    const taz_frame_header_t h = unpack_header(frame);
    return (h.flags & static_cast<uint8_t>(
                          taz_v1_FrameFlag_FRAME_FLAG_CONTINUATION)) != 0U;
}

// Callback that collects bytes into a vector<uint8_t> - same pattern as
// test_response.cpp's collect_bytes_cb, duplicated here since that one has
// internal linkage in its own TU.
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

// A task built directly via the table API (no DETACH).
void FreeTask(taz_task_table_t &table, uint32_t task_id)
{
    taz_task_free(&table, task_id);
}

// Mirrors test_detach.cpp's FreeFinishedTask.
void FreeFinishedTask(taz_task_table_t &table, uint32_t task_id)
{
    taz_task_free(&table, task_id);
}

// Same polling idiom as test_process_monitor.cpp's RunLoopUntil: runs the
// real loop (ONCE, so a real uv_timer_t actually ticks) until pred() is
// true or timeout_ms elapses.
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

// ---------------------------------------------------------------------------
// Interval clamp (pure function, no dispatch)
// ---------------------------------------------------------------------------

TEST(TaskStatusInterval, ClampsBelow100ToTheFloor)
{
    EXPECT_EQ(taz_task_status_interval_ms(0U), 100U);
    EXPECT_EQ(taz_task_status_interval_ms(99U), 100U);
    EXPECT_EQ(taz_task_status_interval_ms(100U), 100U);
    EXPECT_EQ(taz_task_status_interval_ms(101U), 101U);
    EXPECT_EQ(taz_task_status_interval_ms(1000U), 1000U);
}

// ---------------------------------------------------------------------------
// Validation / NOT_FOUND
// ---------------------------------------------------------------------------

TEST_F(FileHandlerTest, TaskStatusUnknownTaskIdIsNotFound)
{
    DispatchRequest(taz_v1_Opcode_OPCODE_TASK_STATUS,
                    TaskStatusRequestBytes(999999U), 600U);

    ASSERT_EQ(Frames().size(), 1U);
    taz_v1_ErrorInfo info = taz_v1_ErrorInfo_init_zero;
    ASSERT_TRUE(DecodeErrorInfo(Frames()[0], &info));
    EXPECT_EQ(info.code, taz_v1_ErrorCode_ERROR_CODE_NOT_FOUND);
    EXPECT_EQ(ActiveStreamCount(), 0U);
}

TEST_F(FileHandlerTest, TaskStatusEmptyPayloadIsNotFound)
{
    DispatchRequest(taz_v1_Opcode_OPCODE_TASK_STATUS, {}, 601U);

    ASSERT_EQ(Frames().size(), 1U);
    taz_v1_ErrorInfo info = taz_v1_ErrorInfo_init_zero;
    ASSERT_TRUE(DecodeErrorInfo(Frames()[0], &info));
    EXPECT_EQ(info.code, taz_v1_ErrorCode_ERROR_CODE_NOT_FOUND);
}

TEST_F(FileHandlerTest, TaskStatusUndecodablePayloadIsInvalidRequest)
{
    const std::vector<uint8_t> garbage{0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU};
    DispatchRequest(taz_v1_Opcode_OPCODE_TASK_STATUS, garbage, 602U);

    ASSERT_EQ(Frames().size(), 1U);
    taz_v1_ErrorInfo info = taz_v1_ErrorInfo_init_zero;
    ASSERT_TRUE(DecodeErrorInfo(Frames()[0], &info));
    EXPECT_EQ(info.code, taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST);
}

// ---------------------------------------------------------------------------
// Non-subscribed poll against a real DETACHed task
// ---------------------------------------------------------------------------

TEST_F(FileHandlerTest, TaskStatusPollRunningThenCompletedWithDecodableResult)
{
    const std::string exe = SelfExePath();
    ASSERT_FALSE(exe.empty());
    const auto detach_payload =
        DetachExecRequestBytes(exe, {"--gtest_filter=NoSuchSuite.*"});

    taz_v1_DetachResponse detach_resp = taz_v1_DetachResponse_init_zero;
    DispatchRequest(
        taz_v1_Opcode_OPCODE_DETACH, detach_payload, 610U,
        [&]()
        {
            ASSERT_EQ(Frames().size(), 1U);
            ASSERT_TRUE(DecodeDetachResponse(Frames()[0], &detach_resp));

            // Poll while the process is still (ostensibly) running -
            // TASK_STATUS's non-subscribed path is itself synchronous, so
            // this does not run the loop.
            DispatchRequestNoRun(taz_v1_Opcode_OPCODE_TASK_STATUS,
                                 TaskStatusRequestBytes(detach_resp.task_id),
                                 611U);
            ASSERT_EQ(Frames().size(), 2U);

            taz_v1_TaskStatusResponse running =
                taz_v1_TaskStatusResponse_init_zero;
            std::vector<uint8_t> running_result;
            running.result.funcs.decode = collect_bytes_cb;
            running.result.arg = &running_result;
            ASSERT_TRUE(DecodeTaskStatusPayload(Frames()[1], &running));
            EXPECT_EQ(running.state, taz_v1_TaskState_TASK_STATE_RUNNING);
            EXPECT_EQ(running.pid, detach_resp.pid);
            EXPECT_TRUE(running_result.empty());
            EXPECT_FALSE(running.has_error);
        });

    // The process has now finished (the outer DispatchRequest's RunLoop
    // reaped it); poll again.
    DispatchRequest(taz_v1_Opcode_OPCODE_TASK_STATUS,
                    TaskStatusRequestBytes(detach_resp.task_id), 612U);
    ASSERT_EQ(Frames().size(), 3U);

    taz_v1_TaskStatusResponse done = taz_v1_TaskStatusResponse_init_zero;
    std::vector<uint8_t> raw_result;
    done.result.funcs.decode = collect_bytes_cb;
    done.result.arg = &raw_result;
    ASSERT_TRUE(DecodeTaskStatusPayload(Frames().back(), &done));
    EXPECT_EQ(done.state, taz_v1_TaskState_TASK_STATE_COMPLETED);
    EXPECT_FALSE(done.has_error);

    taz_v1_CommandExecResponse exec_resp = taz_v1_CommandExecResponse_init_zero;
    ASSERT_TRUE(DecodeCommandExecResponseBytes(raw_result, &exec_resp));
    EXPECT_EQ(exec_resp.exit_code, 0);

    const detached_task_t *task =
        taz_task_find(&Dispatch().tasks, detach_resp.task_id);
    ASSERT_NE(task, nullptr);
    FreeFinishedTask(Dispatch().tasks, detach_resp.task_id);
}

TEST_F(FileHandlerTest, TaskStatusPollFailedCarriesErrorAndStillAttachesResult)
{
    const auto detach_payload =
        DetachExecRequestBytes(TAZ_TEST_EXITER_PATH, {"7"});
    DispatchRequest(taz_v1_Opcode_OPCODE_DETACH, detach_payload, 620U);
    ASSERT_EQ(Frames().size(), 1U);
    taz_v1_DetachResponse detach_resp = taz_v1_DetachResponse_init_zero;
    ASSERT_TRUE(DecodeDetachResponse(Frames()[0], &detach_resp));

    DispatchRequest(taz_v1_Opcode_OPCODE_TASK_STATUS,
                    TaskStatusRequestBytes(detach_resp.task_id), 621U);
    ASSERT_EQ(Frames().size(), 2U);

    taz_v1_TaskStatusResponse resp = taz_v1_TaskStatusResponse_init_zero;
    std::vector<uint8_t> raw_result;
    resp.result.funcs.decode = collect_bytes_cb;
    resp.result.arg = &raw_result;
    ASSERT_TRUE(DecodeTaskStatusPayload(Frames()[1], &resp));
    EXPECT_EQ(resp.state, taz_v1_TaskState_TASK_STATE_FAILED);
    ASSERT_TRUE(resp.has_error);
    EXPECT_EQ(resp.error.code, taz_v1_ErrorCode_ERROR_CODE_INTERNAL);
    EXPECT_STREQ(resp.error.message, "exit code 7");

    taz_v1_CommandExecResponse exec_resp = taz_v1_CommandExecResponse_init_zero;
    ASSERT_TRUE(DecodeCommandExecResponseBytes(raw_result, &exec_resp));
    EXPECT_EQ(exec_resp.exit_code, 7);

    const detached_task_t *task =
        taz_task_find(&Dispatch().tasks, detach_resp.task_id);
    ASSERT_NE(task, nullptr);
    FreeFinishedTask(Dispatch().tasks, detach_resp.task_id);
}

// ---------------------------------------------------------------------------
// Result splitting (large bytes field reassembled exactly)
// ---------------------------------------------------------------------------

TEST_F(FileHandlerTest, TaskStatusLargeResultSplitsAcrossFramesAndReassembles)
{
    static const size_t kDataSize = static_cast<size_t>(200U) * 1024U;
    std::vector<uint8_t> blob(kDataSize);
    for (size_t i = 0U; i < kDataSize; i++)
    {
        blob[i] = static_cast<uint8_t>(i & 0xFFU);
    }

    const uint32_t task_id = taz_task_alloc(
        &Dispatch().tasks, 0U, taz_v1_ErrorPriority_ERROR_PRIORITY_NONE, true);
    ASSERT_NE(task_id, 0U);
    taz_task_set_pid(&Dispatch().tasks, task_id, 4242U, 0U);
    taz_task_set_state(&Dispatch().tasks, task_id,
                       taz_v1_TaskState_TASK_STATE_COMPLETED);
    taz_task_store_result(&Dispatch().tasks, task_id, blob.data(), blob.size());

    DispatchRequest(taz_v1_Opcode_OPCODE_TASK_STATUS,
                    TaskStatusRequestBytes(task_id), 630U);

    ASSERT_GT(Frames().size(), 1U);
    for (size_t i = 0U; i + 1U < Frames().size(); i++)
    {
        EXPECT_TRUE(FrameHasContinuation(Frames()[i])) << "frame " << i;
    }
    EXPECT_FALSE(FrameHasContinuation(Frames().back()));

    std::vector<uint8_t> merged;
    taz_v1_TaskStatusResponse last = taz_v1_TaskStatusResponse_init_zero;
    for (size_t i = 0U; i < Frames().size(); i++)
    {
        taz_v1_TaskStatusResponse partial = taz_v1_TaskStatusResponse_init_zero;
        partial.result.funcs.decode = collect_bytes_cb;
        partial.result.arg = &merged;
        ASSERT_TRUE(DecodeTaskStatusPayload(Frames()[i], &partial))
            << "frame " << i;
        if (i + 1U == Frames().size())
        {
            last = partial;
        }
    }
    EXPECT_EQ(last.state, taz_v1_TaskState_TASK_STATE_COMPLETED);
    EXPECT_EQ(last.pid, 4242U);
    ASSERT_EQ(merged.size(), kDataSize);
    EXPECT_EQ(merged, blob);

    FreeTask(Dispatch().tasks, task_id);
}

// ---------------------------------------------------------------------------
// Subscribed stream: updates, interval clamp, terminal frame, CANCEL, races
// ---------------------------------------------------------------------------

TEST_F(FileHandlerTest,
       TaskStatusSubscribeStreamsUpdatesThenEndsOnTerminalStateWithNoResult)
{
    const uint32_t task_id = taz_task_alloc(
        &Dispatch().tasks, 0U, taz_v1_ErrorPriority_ERROR_PRIORITY_NONE, true);
    ASSERT_NE(task_id, 0U);
    taz_task_set_pid(&Dispatch().tasks, task_id, 4343U, 0U);
    taz_task_set_state(&Dispatch().tasks, task_id,
                       taz_v1_TaskState_TASK_STATE_RUNNING);

    DispatchRequestNoRun(
        taz_v1_Opcode_OPCODE_TASK_STATUS,
        TaskStatusRequestBytes(task_id, /*subscribe=*/true, 100U), 640U);

    // The timer's initial timeout is 0: the first update fires essentially
    // immediately.
    RunLoopUntil(Loop(), [&]() { return !Frames().empty(); }, 1000);
    ASSERT_EQ(Frames().size(), 1U);
    {
        taz_v1_TaskStatusResponse resp = taz_v1_TaskStatusResponse_init_zero;
        std::vector<uint8_t> result;
        resp.result.funcs.decode = collect_bytes_cb;
        resp.result.arg = &result;
        ASSERT_TRUE(DecodeTaskStatusPayload(Frames()[0], &resp));
        EXPECT_TRUE(FrameHasContinuation(Frames()[0]));
        EXPECT_EQ(resp.state, taz_v1_TaskState_TASK_STATE_RUNNING);
        EXPECT_EQ(resp.pid, 4343U);
        EXPECT_TRUE(result.empty());
        EXPECT_FALSE(resp.has_error);
    }

    // A nonempty result is now available (a poll would attach it) - proves
    // the next streamed frame still never carries it.
    const std::vector<uint8_t> blob{1U, 2U, 3U, 4U};
    taz_task_store_result(&Dispatch().tasks, task_id, blob.data(), blob.size());
    taz_task_set_state(&Dispatch().tasks, task_id,
                       taz_v1_TaskState_TASK_STATE_COMPLETED);

    const size_t before = Frames().size();
    RunLoopUntil(Loop(), [&]() { return Frames().size() > before; }, 5000);
    ASSERT_EQ(Frames().size(), before + 1U);
    {
        taz_v1_TaskStatusResponse resp = taz_v1_TaskStatusResponse_init_zero;
        std::vector<uint8_t> result;
        resp.result.funcs.decode = collect_bytes_cb;
        resp.result.arg = &result;
        ASSERT_TRUE(DecodeTaskStatusPayload(Frames().back(), &resp));
        EXPECT_FALSE(FrameHasContinuation(Frames().back()));
        EXPECT_EQ(resp.state, taz_v1_TaskState_TASK_STATE_COMPLETED);
        EXPECT_TRUE(result.empty());
    }

    // The stream already ended itself on the terminal frame: running the
    // loop again produces nothing further, deterministically (the timer
    // handle is already closed).
    RunLoop();
    EXPECT_EQ(Frames().size(), before + 1U);
    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());

    FreeTask(Dispatch().tasks, task_id);
}

TEST_F(FileHandlerTest, TaskStatusSubscribeClampsIntervalAndSpacesUpdates)
{
    const uint32_t task_id = taz_task_alloc(
        &Dispatch().tasks, 0U, taz_v1_ErrorPriority_ERROR_PRIORITY_NONE, true);
    ASSERT_NE(task_id, 0U);
    taz_task_set_state(&Dispatch().tasks, task_id,
                       taz_v1_TaskState_TASK_STATE_RUNNING);

    // interval_ms 0 clamps to the 100 ms floor - proved indirectly, the
    // same way process_monitor.c's own clamp test does, by observing that
    // consecutive updates stay at least ~90 ms apart.
    DispatchRequestNoRun(
        taz_v1_Opcode_OPCODE_TASK_STATUS,
        TaskStatusRequestBytes(task_id, /*subscribe=*/true, 0U), 650U);

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

    DispatchRequestNoRun(taz_v1_Opcode_OPCODE_CANCEL, CancelRequestBytes(650U),
                         651U);
    RunLoop();
    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());

    FreeTask(Dispatch().tasks, task_id);
}

TEST_F(FileHandlerTest, TaskStatusCancelEndsSubscriptionThenSecondCancelFails)
{
    const uint32_t task_id = taz_task_alloc(
        &Dispatch().tasks, 0U, taz_v1_ErrorPriority_ERROR_PRIORITY_NONE, true);
    ASSERT_NE(task_id, 0U);
    taz_task_set_state(&Dispatch().tasks, task_id,
                       taz_v1_TaskState_TASK_STATE_RUNNING);

    DispatchRequestNoRun(
        taz_v1_Opcode_OPCODE_TASK_STATUS,
        TaskStatusRequestBytes(task_id, /*subscribe=*/true, 100U), 660U);
    RunLoopUntil(Loop(), [&]() { return !Frames().empty(); }, 1000);
    ASSERT_EQ(Frames().size(), 1U);

    DispatchRequestNoRun(taz_v1_Opcode_OPCODE_CANCEL, CancelRequestBytes(660U),
                         661U);
    // api.md: the cancelled stream's final frame (CONTINUATION cleared),
    // then the CANCEL's own reply.
    ASSERT_EQ(Frames().size(), 3U);
    {
        EXPECT_EQ(unpack_header(Frames()[1]).stream_id, 660U);
        EXPECT_FALSE(FrameHasContinuation(Frames()[1]));
        taz_v1_TaskStatusResponse last = taz_v1_TaskStatusResponse_init_zero;
        ASSERT_TRUE(DecodeTaskStatusPayload(Frames()[1], &last));
        EXPECT_EQ(last.state, taz_v1_TaskState_TASK_STATE_RUNNING);

        taz_v1_CancelResponse resp = taz_v1_CancelResponse_init_zero;
        ASSERT_TRUE(DecodeCancelResponse(Frames()[2], &resp));
        EXPECT_TRUE(resp.cancelled);
    }

    // Same target again: already MON_CLOSING-equivalent, refused
    // synchronously - mirrors
    // MonitorCancelSameTargetTwiceInARowReturnsTrueThenFalse.
    DispatchRequestNoRun(taz_v1_Opcode_OPCODE_CANCEL, CancelRequestBytes(660U),
                         662U);
    ASSERT_EQ(Frames().size(), 4U);
    {
        taz_v1_CancelResponse resp = taz_v1_CancelResponse_init_zero;
        ASSERT_TRUE(DecodeCancelResponse(Frames()[3], &resp));
        EXPECT_FALSE(resp.cancelled);
    }

    RunLoop();
    EXPECT_EQ(Frames().size(), 4U);
    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());

    FreeTask(Dispatch().tasks, task_id);
}

// A client that sends CANCEL and disconnects can have both arrive in one
// read, so the stream ends and the connection closes in the same loop
// iteration: abort reaches the stream with its timer handle already
// closing. Mirrors MonitorAbortAfterCancelInSameIterationIsNoop.
TEST_F(FileHandlerTest, TaskStatusAbortAfterCancelInSameIterationIsNoop)
{
    const uint32_t task_id = taz_task_alloc(
        &Dispatch().tasks, 0U, taz_v1_ErrorPriority_ERROR_PRIORITY_NONE, true);
    ASSERT_NE(task_id, 0U);
    taz_task_set_state(&Dispatch().tasks, task_id,
                       taz_v1_TaskState_TASK_STATE_RUNNING);

    DispatchRequestNoRun(
        taz_v1_Opcode_OPCODE_TASK_STATUS,
        TaskStatusRequestBytes(task_id, /*subscribe=*/true, 100U), 670U);
    RunLoopUntil(Loop(), [&]() { return !Frames().empty(); }, 1000);
    ASSERT_EQ(Frames().size(), 1U);

    DispatchRequestNoRun(taz_v1_Opcode_OPCODE_CANCEL, CancelRequestBytes(670U),
                         671U);
    ASSERT_EQ(Frames().size(), 3U); // the final frame, then the reply

    CloseConnectionAndCancelAll();
    RunLoop();
    EXPECT_EQ(Frames().size(), 3U);
    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());

    FreeTask(Dispatch().tasks, task_id);
}

// Connection close without a preceding CANCEL - the plain abort path.
TEST_F(FileHandlerTest, TaskStatusConnectionCloseWhileSubscribedEndsCleanly)
{
    const uint32_t task_id = taz_task_alloc(
        &Dispatch().tasks, 0U, taz_v1_ErrorPriority_ERROR_PRIORITY_NONE, true);
    ASSERT_NE(task_id, 0U);
    taz_task_set_state(&Dispatch().tasks, task_id,
                       taz_v1_TaskState_TASK_STATE_RUNNING);

    DispatchRequestNoRun(
        taz_v1_Opcode_OPCODE_TASK_STATUS,
        TaskStatusRequestBytes(task_id, /*subscribe=*/true, 100U), 680U);
    RunLoopUntil(Loop(), [&]() { return !Frames().empty(); }, 1000);
    ASSERT_EQ(Frames().size(), 1U);

    CloseConnectionAndCancelAll();
    RunLoop();
    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());

    FreeTask(Dispatch().tasks, task_id);
}

// A finished monitor=false task waits for its subscriber's final frame
// before it is freed, so a subscription always ends with the terminal state.
// The final frame goes out when the task finishes, not at the next tick: the
// 60 s interval here would otherwise outlast the 5 s wait.
TEST_F(FileHandlerTest, TaskStatusSubscriptionOnMonitorFalseTaskGetsFinalFrame)
{
    DispatchRequestNoRun(taz_v1_Opcode_OPCODE_DETACH,
                         DetachExecRequestBytes(TAZ_TEST_EXITER_PATH, {"0"},
                                                /*monitor=*/false),
                         690U);
    ASSERT_EQ(Frames().size(), 1U);
    taz_v1_DetachResponse detach_resp = taz_v1_DetachResponse_init_zero;
    ASSERT_TRUE(DecodeDetachResponse(Frames()[0], &detach_resp));

    DispatchRequestNoRun(
        taz_v1_Opcode_OPCODE_TASK_STATUS,
        TaskStatusRequestBytes(detach_resp.task_id, /*subscribe=*/true, 60000U),
        691U);
    RunLoopUntil(
        Loop(),
        [&]()
        {
            return Frames().size() > 1U &&
                   !FrameHasContinuation(Frames().back());
        },
        5000);

    ASSERT_GT(Frames().size(), 1U);
    EXPECT_FALSE(FrameHasContinuation(Frames().back()));
    taz_v1_TaskStatusResponse last = taz_v1_TaskStatusResponse_init_zero;
    ASSERT_TRUE(DecodeTaskStatusPayload(Frames().back(), &last));
    EXPECT_EQ(last.state, taz_v1_TaskState_TASK_STATE_COMPLETED);

    RunLoop();
    EXPECT_EQ(taz_task_find(&Dispatch().tasks, detach_resp.task_id), nullptr);
    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());
}

// A subscribed task is never freed or evicted; if that invariant ever broke,
// the next tick still ends the stream with a final frame instead of reading
// freed memory or going silent.
TEST_F(FileHandlerTest, TaskStatusSubscriptionEndsWithFinalFrameIfTaskVanishes)
{
    const uint32_t task_id = taz_task_alloc(
        &Dispatch().tasks, 0U, taz_v1_ErrorPriority_ERROR_PRIORITY_NONE, true);
    ASSERT_NE(task_id, 0U);
    taz_task_set_state(&Dispatch().tasks, task_id,
                       taz_v1_TaskState_TASK_STATE_RUNNING);

    DispatchRequestNoRun(
        taz_v1_Opcode_OPCODE_TASK_STATUS,
        TaskStatusRequestBytes(task_id, /*subscribe=*/true, 100U), 692U);
    RunLoopUntil(Loop(), [&]() { return !Frames().empty(); }, 1000);
    ASSERT_EQ(Frames().size(), 1U);

    FreeTask(Dispatch().tasks, task_id);

    RunLoop();
    ASSERT_EQ(Frames().size(), 2U);
    EXPECT_FALSE(FrameHasContinuation(Frames()[1]));
    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());
}

} // namespace
