// Unit tests for handlers/task_cancel.c: TASK_CANCEL (0x0045). signal == 0
// kills the whole tree (taz_exec_cancel) and a nonzero signal goes to the
// process group (taz_exec_signal); either accepted cancel later reports
// CANCELLED from the task's own on_done (handlers/detach.c, guarded by
// detached_task_t::cancel_requested). An invalid signal is INVALID_REQUEST,
// an already-finished task is accepted=false touching no handle, and an
// unknown task_id is NOT_FOUND.

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#ifndef _WIN32
#include <csignal>
#endif

#include <gtest/gtest.h>
#include <pb_decode.h>
#include <pb_encode.h>
#include <uv.h>

#include "file_test_support.h"
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

std::vector<uint8_t> TaskCancelRequestBytes(uint32_t task_id, int32_t signal)
{
    taz_v1_TaskCancelRequest req = taz_v1_TaskCancelRequest_init_zero;
    req.task_id = task_id;
    req.signal = signal;

    std::vector<uint8_t> buf(taz_v1_TaskCancelRequest_size);
    pb_ostream_t ostream = pb_ostream_from_buffer(buf.data(), buf.size());
    EXPECT_TRUE(pb_encode(&ostream, taz_v1_TaskCancelRequest_fields, &req));
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

bool DecodeTaskCancelResponse(const std::vector<uint8_t> &frame,
                              taz_v1_TaskCancelResponse *out)
{
    const taz_frame_header_t h = unpack_header(frame);
    if (h.type != static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_RESPONSE))
    {
        return false;
    }
    const std::vector<uint8_t> payload = frame_payload(frame);
    pb_istream_t stream =
        pb_istream_from_buffer(payload.data(), payload.size());
    return pb_decode(&stream, taz_v1_TaskCancelResponse_fields, out);
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

// Mirrors test_detach.cpp's FreeFinishedTask.
void FreeFinishedTask(taz_task_table_t &table, uint32_t task_id)
{
    taz_task_free(&table, task_id);
}

} // namespace

// ---------------------------------------------------------------------------
// signal == 0: whole-tree cancel, accepted=true, CANCELLED after exit
// ---------------------------------------------------------------------------

TEST_F(FileHandlerTest, TaskCancelSignalZeroKillsTreeAndBecomesCancelled)
{
    const auto detach_payload = DetachExecRequestBytes(TAZ_TEST_SLEEPER_PATH);
    DispatchRequestNoRun(taz_v1_Opcode_OPCODE_DETACH, detach_payload, 80U);
    ASSERT_EQ(Frames().size(), 1U);
    taz_v1_DetachResponse detach_resp = taz_v1_DetachResponse_init_zero;
    ASSERT_TRUE(DecodeDetachResponse(Frames()[0], &detach_resp));

    const auto cancel_payload = TaskCancelRequestBytes(detach_resp.task_id, 0);
    DispatchRequestNoRun(taz_v1_Opcode_OPCODE_TASK_CANCEL, cancel_payload, 81U);
    ASSERT_EQ(Frames().size(), 2U);
    taz_v1_TaskCancelResponse cancel_resp = taz_v1_TaskCancelResponse_init_zero;
    ASSERT_TRUE(DecodeTaskCancelResponse(Frames()[1], &cancel_resp));
    EXPECT_TRUE(cancel_resp.accepted);

    RunLoop();

    const detached_task_t *task =
        taz_task_find(&Dispatch().tasks, detach_resp.task_id);
    ASSERT_NE(task, nullptr);
    EXPECT_EQ(task->state, taz_v1_TaskState_TASK_STATE_CANCELLED);

    FreeFinishedTask(Dispatch().tasks, detach_resp.task_id);
}

// ---------------------------------------------------------------------------
// Nonzero valid signal: process-group signal, accepted, CANCELLED after exit
// ---------------------------------------------------------------------------

#ifndef _WIN32
TEST_F(FileHandlerTest, TaskCancelNonzeroSignalIsAcceptedAndBecomesCancelled)
{
    const auto detach_payload = DetachExecRequestBytes(TAZ_TEST_SLEEPER_PATH);
    DispatchRequestNoRun(taz_v1_Opcode_OPCODE_DETACH, detach_payload, 82U);
    ASSERT_EQ(Frames().size(), 1U);
    taz_v1_DetachResponse detach_resp = taz_v1_DetachResponse_init_zero;
    ASSERT_TRUE(DecodeDetachResponse(Frames()[0], &detach_resp));

    const auto cancel_payload =
        TaskCancelRequestBytes(detach_resp.task_id, SIGTERM);
    DispatchRequestNoRun(taz_v1_Opcode_OPCODE_TASK_CANCEL, cancel_payload, 83U);
    ASSERT_EQ(Frames().size(), 2U);
    taz_v1_TaskCancelResponse cancel_resp = taz_v1_TaskCancelResponse_init_zero;
    ASSERT_TRUE(DecodeTaskCancelResponse(Frames()[1], &cancel_resp));
    EXPECT_TRUE(cancel_resp.accepted);

    RunLoop();

    const detached_task_t *task =
        taz_task_find(&Dispatch().tasks, detach_resp.task_id);
    ASSERT_NE(task, nullptr);
    EXPECT_EQ(task->state, taz_v1_TaskState_TASK_STATE_CANCELLED);

    FreeFinishedTask(Dispatch().tasks, detach_resp.task_id);
}

// ---------------------------------------------------------------------------
// Invalid signal: INVALID_REQUEST, task left running untouched
// ---------------------------------------------------------------------------

TEST_F(FileHandlerTest, TaskCancelInvalidSignalIsInvalidRequest)
{
    const auto detach_payload = DetachExecRequestBytes(TAZ_TEST_SLEEPER_PATH);
    DispatchRequestNoRun(taz_v1_Opcode_OPCODE_DETACH, detach_payload, 84U);
    ASSERT_EQ(Frames().size(), 1U);
    taz_v1_DetachResponse detach_resp = taz_v1_DetachResponse_init_zero;
    ASSERT_TRUE(DecodeDetachResponse(Frames()[0], &detach_resp));

    // Not a signal number any POSIX platform knows - uv_kill/kill(2) reports
    // EINVAL, which the handler maps to INVALID_REQUEST.
    const auto cancel_payload =
        TaskCancelRequestBytes(detach_resp.task_id, 99999);
    DispatchRequestNoRun(taz_v1_Opcode_OPCODE_TASK_CANCEL, cancel_payload, 85U);
    ASSERT_EQ(Frames().size(), 2U);
    taz_v1_ErrorInfo info = taz_v1_ErrorInfo_init_zero;
    ASSERT_TRUE(DecodeErrorInfo(Frames()[1], &info));
    EXPECT_EQ(info.code, taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST);

    const detached_task_t *task =
        taz_task_find(&Dispatch().tasks, detach_resp.task_id);
    ASSERT_NE(task, nullptr);
    EXPECT_EQ(task->state, taz_v1_TaskState_TASK_STATE_RUNNING);
    EXPECT_FALSE(task->cancel_requested);

    // Clean up the still-running sleeper rather than leaving it orphaned
    // for the rest of this test binary's process lifetime.
    ASSERT_NE(task->exec, nullptr);
    taz_exec_cancel(task->exec);
    RunLoop();

    task = taz_task_find(&Dispatch().tasks, detach_resp.task_id);
    ASSERT_NE(task, nullptr);
    FreeFinishedTask(Dispatch().tasks, detach_resp.task_id);
}
#else
TEST_F(FileHandlerTest, TaskCancelInvalidSignalIsInvalidRequest)
{
    const auto detach_payload = DetachExecRequestBytes(TAZ_TEST_SLEEPER_PATH);
    DispatchRequestNoRun(taz_v1_Opcode_OPCODE_DETACH, detach_payload, 84U);
    ASSERT_EQ(Frames().size(), 1U);
    taz_v1_DetachResponse detach_resp = taz_v1_DetachResponse_init_zero;
    ASSERT_TRUE(DecodeDetachResponse(Frames()[0], &detach_resp));

    // No POSIX signal numbering on Windows, but taz_exec_signal still
    // rejects an obviously-invalid value with UV_EINVAL, which the handler
    // maps to INVALID_REQUEST.
    const auto cancel_payload =
        TaskCancelRequestBytes(detach_resp.task_id, 99999);
    DispatchRequestNoRun(taz_v1_Opcode_OPCODE_TASK_CANCEL, cancel_payload, 85U);
    ASSERT_EQ(Frames().size(), 2U);
    taz_v1_ErrorInfo info = taz_v1_ErrorInfo_init_zero;
    ASSERT_TRUE(DecodeErrorInfo(Frames()[1], &info));
    EXPECT_EQ(info.code, taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST);

    const detached_task_t *task =
        taz_task_find(&Dispatch().tasks, detach_resp.task_id);
    ASSERT_NE(task, nullptr);
    EXPECT_EQ(task->state, taz_v1_TaskState_TASK_STATE_RUNNING);
    EXPECT_FALSE(task->cancel_requested);

    // Clean up the still-running sleeper rather than leaving it orphaned
    // for the rest of this test binary's process lifetime.
    ASSERT_NE(task->exec, nullptr);
    taz_exec_cancel(task->exec);
    RunLoop();

    task = taz_task_find(&Dispatch().tasks, detach_resp.task_id);
    ASSERT_NE(task, nullptr);
    FreeFinishedTask(Dispatch().tasks, detach_resp.task_id);
}
#endif /* !_WIN32 */

// ---------------------------------------------------------------------------
// Already-finished task: accepted=false, no handle touched
// ---------------------------------------------------------------------------

TEST_F(FileHandlerTest, TaskCancelOnFinishedTaskIsNotAcceptedAndNoop)
{
    const auto detach_payload = DetachExecRequestBytes(TAZ_TEST_EXITER_PATH);
    DispatchRequestNoRun(taz_v1_Opcode_OPCODE_DETACH, detach_payload, 86U);
    ASSERT_EQ(Frames().size(), 1U);
    taz_v1_DetachResponse detach_resp = taz_v1_DetachResponse_init_zero;
    ASSERT_TRUE(DecodeDetachResponse(Frames()[0], &detach_resp));
    RunLoop();

    const detached_task_t *task =
        taz_task_find(&Dispatch().tasks, detach_resp.task_id);
    ASSERT_NE(task, nullptr);
    ASSERT_EQ(task->state, taz_v1_TaskState_TASK_STATE_COMPLETED);
    EXPECT_EQ(task->exec, nullptr);

    const auto cancel_payload = TaskCancelRequestBytes(detach_resp.task_id, 0);
    DispatchRequest(taz_v1_Opcode_OPCODE_TASK_CANCEL, cancel_payload, 87U);
    ASSERT_EQ(Frames().size(), 2U);
    taz_v1_TaskCancelResponse cancel_resp = taz_v1_TaskCancelResponse_init_zero;
    ASSERT_TRUE(DecodeTaskCancelResponse(Frames()[1], &cancel_resp));
    EXPECT_FALSE(cancel_resp.accepted);

    task = taz_task_find(&Dispatch().tasks, detach_resp.task_id);
    ASSERT_NE(task, nullptr);
    EXPECT_EQ(task->state, taz_v1_TaskState_TASK_STATE_COMPLETED);

    FreeFinishedTask(Dispatch().tasks, detach_resp.task_id);
}

// ---------------------------------------------------------------------------
// Unknown task_id: NOT_FOUND
// ---------------------------------------------------------------------------

TEST_F(FileHandlerTest, TaskCancelUnknownTaskIdIsNotFound)
{
    const auto cancel_payload = TaskCancelRequestBytes(0xDEADBEEFU, 0);
    DispatchRequest(taz_v1_Opcode_OPCODE_TASK_CANCEL, cancel_payload, 88U);

    ASSERT_EQ(Frames().size(), 1U);
    taz_v1_ErrorInfo info = taz_v1_ErrorInfo_init_zero;
    ASSERT_TRUE(DecodeErrorInfo(Frames()[0], &info));
    EXPECT_EQ(info.code, taz_v1_ErrorCode_ERROR_CODE_NOT_FOUND);
}

TEST_F(FileHandlerTest, TaskCancelUndecodablePayloadIsInvalidRequest)
{
    const std::vector<uint8_t> garbage{0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU};
    DispatchRequest(taz_v1_Opcode_OPCODE_TASK_CANCEL, garbage, 89U);

    ASSERT_EQ(Frames().size(), 1U);
    taz_v1_ErrorInfo info = taz_v1_ErrorInfo_init_zero;
    ASSERT_TRUE(DecodeErrorInfo(Frames()[0], &info));
    EXPECT_EQ(info.code, taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST);
}

// ---------------------------------------------------------------------------
// [LIFETIME]: a TASK_CANCEL racing the process's own exit - no use-after-free
// ---------------------------------------------------------------------------

TEST_F(FileHandlerTest, TaskCancelRacingExitDoesNotUseAfterFree)
{
    // exiter exits essentially immediately; dispatching DETACH then
    // TASK_CANCEL back-to-back without running the loop in between queues
    // the cancel while the process may already be in the middle of exiting
    // once the loop finally runs - exactly the race this handler's
    // task->exec == NULL check and on_done's exec-drop-first ordering must
    // survive.
    const auto detach_payload = DetachExecRequestBytes(TAZ_TEST_EXITER_PATH);
    DispatchRequestNoRun(taz_v1_Opcode_OPCODE_DETACH, detach_payload, 90U);
    ASSERT_EQ(Frames().size(), 1U);
    taz_v1_DetachResponse detach_resp = taz_v1_DetachResponse_init_zero;
    ASSERT_TRUE(DecodeDetachResponse(Frames()[0], &detach_resp));

    const auto cancel_payload = TaskCancelRequestBytes(detach_resp.task_id, 0);
    DispatchRequestNoRun(taz_v1_Opcode_OPCODE_TASK_CANCEL, cancel_payload, 91U);
    ASSERT_EQ(Frames().size(), 2U);

    RunLoop();

    const detached_task_t *task =
        taz_task_find(&Dispatch().tasks, detach_resp.task_id);
    ASSERT_NE(task, nullptr);
    EXPECT_TRUE(task->state == taz_v1_TaskState_TASK_STATE_CANCELLED ||
                task->state == taz_v1_TaskState_TASK_STATE_COMPLETED);
    EXPECT_EQ(task->exec, nullptr);

    FreeFinishedTask(Dispatch().tasks, detach_resp.task_id);
}
