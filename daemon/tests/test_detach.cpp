// Unit tests for handlers/detach.c: DETACH (0x0043) spawning the exec
// oneof, its DetachResponse, the task table transitions its on_done drives
// (COMPLETED/FAILED/CANCELLED, result/error storage, monitor retention),
// the exit-code registry wiring (register at spawn, record_exit via the
// exec on-exit hook, kept after the task is freed or evicted), and the
// CRITICAL-priority PRIORITY ERROR emission.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <pb_decode.h>
#include <pb_encode.h>
#include <uv.h>

#ifndef _WIN32
#include <csignal>

#include <unistd.h>
#else
#include <windows.h>

#include "win32_token_test_support.h"
#endif

#include "file_test_support.h"
#include "run_as_test_support.h"
#include "taz/task.h"
#include "taz/task_registry.h"
#include "taz/v1/advanced.pb.h"
#include "taz/v1/command.pb.h"
#include "taz/v1/common.pb.h"
#include "taz/v1/file.pb.h"
#include "taz/work.h"

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
std::vector<uint8_t> DetachExecRequestBytes(
    const std::string &command, const std::vector<std::string> &args = {},
    bool monitor = true,
    taz_v1_ErrorPriority priority = taz_v1_ErrorPriority_ERROR_PRIORITY_NONE,
    uint32_t timeout_ms = 0U, const std::string &as_user = std::string())
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
    req->task.exec.timeout_ms = timeout_ms;
    (void)std::strncpy(req->task.exec.as_user, as_user.c_str(),
                       sizeof(req->task.exec.as_user) - 1U);
    req->monitor = monitor;
    req->error_priority = priority;

    std::vector<uint8_t> buf(taz_v1_DetachRequest_size);
    pb_ostream_t ostream = pb_ostream_from_buffer(buf.data(), buf.size());
    EXPECT_TRUE(pb_encode(&ostream, taz_v1_DetachRequest_fields, req.get()));
    buf.resize(ostream.bytes_written);
    return buf;
}

std::vector<uint8_t> DetachFilePutRequestBytes()
{
    auto req = std::make_unique<taz_v1_DetachRequest>();
    (void)std::memset(req.get(), 0, sizeof(*req));
    req->which_task = taz_v1_DetachRequest_file_put_tag;

    std::vector<uint8_t> buf(taz_v1_DetachRequest_size);
    pb_ostream_t ostream = pb_ostream_from_buffer(buf.data(), buf.size());
    EXPECT_TRUE(pb_encode(&ostream, taz_v1_DetachRequest_fields, req.get()));
    buf.resize(ostream.bytes_written);
    return buf;
}

std::vector<uint8_t> DetachFileGetRequestBytes()
{
    auto req = std::make_unique<taz_v1_DetachRequest>();
    (void)std::memset(req.get(), 0, sizeof(*req));
    req->which_task = taz_v1_DetachRequest_file_get_tag;

    std::vector<uint8_t> buf(taz_v1_DetachRequest_size);
    pb_ostream_t ostream = pb_ostream_from_buffer(buf.data(), buf.size());
    EXPECT_TRUE(pb_encode(&ostream, taz_v1_DetachRequest_fields, req.get()));
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

bool DecodeExecResponse(const detached_task_t *task,
                        taz_v1_CommandExecResponse *out)
{
    if (task == nullptr || task->result == nullptr)
    {
        return false;
    }
    pb_istream_t stream =
        pb_istream_from_buffer(task->result, task->result_len);
    return pb_decode(&stream, taz_v1_CommandExecResponse_fields, out);
}

#ifndef _WIN32
bool CollectBytes(pb_istream_t *stream, const pb_field_iter_t * /*f*/,
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

// The stdout a finished task's stored CommandExecResponse carries (only the
// POSIX-only rows below read it).
std::string TaskStdout(const detached_task_t *task)
{
    std::string out;
    taz_v1_CommandExecResponse resp = taz_v1_CommandExecResponse_init_zero;
    resp.stdout_data.funcs.decode = CollectBytes;
    resp.stdout_data.arg = &out;
    pb_istream_t stream =
        pb_istream_from_buffer(task->result, task->result_len);
    EXPECT_TRUE(pb_decode(&stream, taz_v1_CommandExecResponse_fields, &resp));
    return out;
}
#endif /* !_WIN32 */

// True if every slot in table is free - used to prove a rejected DETACH
// (NOT_SUPPORTED/spawn failure) created no task.
bool TableIsEmpty(const taz_task_table_t &table)
{
    return std::all_of(std::begin(table.slots), std::end(table.slots),
                       [](const detached_task_t &slot)
                       { return slot.task_id == 0U; });
}

// A monitor=true finished task stays in the table, holding a heap-allocated
// result, until it is evicted or its connection is freed. The fixture's
// TearDown frees the table; tests that need the slot gone sooner call this.
void FreeFinishedTask(taz_task_table_t &table, uint32_t task_id)
{
    taz_task_free(&table, task_id);
}

// ---------------------------------------------------------------------------
// Oneof rejection: file_put/file_get/empty -> NOT_SUPPORTED, no task
// ---------------------------------------------------------------------------

TEST_F(FileHandlerTest, DetachFilePutOneofIsNotSupported)
{
    DispatchRequest(taz_v1_Opcode_OPCODE_DETACH, DetachFilePutRequestBytes(),
                    60U);

    ASSERT_EQ(Frames().size(), 1U);
    taz_v1_ErrorInfo info = taz_v1_ErrorInfo_init_zero;
    ASSERT_TRUE(DecodeErrorInfo(Frames()[0], &info));
    EXPECT_EQ(info.code, taz_v1_ErrorCode_ERROR_CODE_NOT_SUPPORTED);
    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), 0);
    EXPECT_TRUE(TableIsEmpty(Dispatch().tasks));
}

TEST_F(FileHandlerTest, DetachFileGetOneofIsNotSupported)
{
    DispatchRequest(taz_v1_Opcode_OPCODE_DETACH, DetachFileGetRequestBytes(),
                    61U);

    ASSERT_EQ(Frames().size(), 1U);
    taz_v1_ErrorInfo info = taz_v1_ErrorInfo_init_zero;
    ASSERT_TRUE(DecodeErrorInfo(Frames()[0], &info));
    EXPECT_EQ(info.code, taz_v1_ErrorCode_ERROR_CODE_NOT_SUPPORTED);
    EXPECT_TRUE(TableIsEmpty(Dispatch().tasks));
}

TEST_F(FileHandlerTest, DetachEmptyOneofIsNotSupported)
{
    DispatchRequest(taz_v1_Opcode_OPCODE_DETACH, {}, 62U);

    ASSERT_EQ(Frames().size(), 1U);
    taz_v1_ErrorInfo info = taz_v1_ErrorInfo_init_zero;
    ASSERT_TRUE(DecodeErrorInfo(Frames()[0], &info));
    EXPECT_EQ(info.code, taz_v1_ErrorCode_ERROR_CODE_NOT_SUPPORTED);
    EXPECT_TRUE(TableIsEmpty(Dispatch().tasks));
}

TEST_F(FileHandlerTest, DetachUndecodablePayloadIsInvalidRequest)
{
    const std::vector<uint8_t> garbage{0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU};
    DispatchRequest(taz_v1_Opcode_OPCODE_DETACH, garbage, 63U);

    ASSERT_EQ(Frames().size(), 1U);
    taz_v1_ErrorInfo info = taz_v1_ErrorInfo_init_zero;
    ASSERT_TRUE(DecodeErrorInfo(Frames()[0], &info));
    EXPECT_EQ(info.code, taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST);
    EXPECT_TRUE(TableIsEmpty(Dispatch().tasks));
}

// ---------------------------------------------------------------------------
// Spawn failure: ERROR on the DETACH request, no task
// ---------------------------------------------------------------------------

TEST_F(FileHandlerTest, DetachSpawnFailureSendsErrorAndCreatesNoTask)
{
    const auto payload = DetachExecRequestBytes("/no/such/taz-test-binary-xyz");
    DispatchRequest(taz_v1_Opcode_OPCODE_DETACH, payload, 64U);

    ASSERT_EQ(Frames().size(), 1U);
    taz_v1_ErrorInfo info = taz_v1_ErrorInfo_init_zero;
    ASSERT_TRUE(DecodeErrorInfo(Frames()[0], &info));
    EXPECT_EQ(info.code, taz_v1_ErrorCode_ERROR_CODE_NOT_FOUND);
    EXPECT_EQ(RefCount(), 0);
    EXPECT_TRUE(TableIsEmpty(Dispatch().tasks));
}

// as_user resolution happens after taz_task_alloc, so a failure there must
// free the slot it already took - mirrors test_command.cpp's
// AsUserNotPrivilegedReturnsNotSupported for the identical pure function,
// but through detach.c's own dispatch path and task-table cleanup.
TEST_F(FileHandlerTest,
       DetachAsUserNotPrivilegedFreesTaskAndReturnsNotSupported)
{
    const ScopedPrivilege unprivileged(0);
    const auto payload = DetachExecRequestBytes(
        TAZ_TEST_SLEEPER_PATH, {}, /*monitor=*/true,
        taz_v1_ErrorPriority_ERROR_PRIORITY_NONE, /*timeout_ms=*/0U,
        /*as_user=*/"alice");
    DispatchRequest(taz_v1_Opcode_OPCODE_DETACH, payload, 74U);

    ASSERT_EQ(Frames().size(), 1U);
    taz_v1_ErrorInfo info = taz_v1_ErrorInfo_init_zero;
    ASSERT_TRUE(DecodeErrorInfo(Frames()[0], &info));
    EXPECT_EQ(info.code, taz_v1_ErrorCode_ERROR_CODE_NOT_SUPPORTED);
    EXPECT_EQ(RefCount(), 0);
    EXPECT_TRUE(TableIsEmpty(Dispatch().tasks));
}

// ---------------------------------------------------------------------------
// Successful spawn: immediate DetachResponse, RUNNING, conn ref taken
// ---------------------------------------------------------------------------

TEST_F(FileHandlerTest, DetachExecSpawnsAndRespondsImmediately)
{
    const std::string exe = SelfExePath();
    ASSERT_FALSE(exe.empty());
    const auto payload =
        DetachExecRequestBytes(exe, {"--gtest_filter=NoSuchSuite.*"});

    taz_v1_DetachResponse resp = taz_v1_DetachResponse_init_zero;
    DispatchRequest(taz_v1_Opcode_OPCODE_DETACH, payload, 65U,
                    [&]()
                    {
                        // Observed while the process is still (ostensibly)
                        // running, before the loop has a chance to reap it.
                        ASSERT_EQ(Frames().size(), 1U);
                        ASSERT_TRUE(DecodeDetachResponse(Frames()[0], &resp));
                        EXPECT_NE(resp.task_id, 0U);
                        EXPECT_NE(resp.pid, 0U);
                        EXPECT_EQ(ActiveStreamCount(), 0U);
                        EXPECT_EQ(RefCount(), 1);

                        const detached_task_t *task =
                            taz_task_find(&Dispatch().tasks, resp.task_id);
                        ASSERT_NE(task, nullptr);
                        EXPECT_EQ(task->state,
                                  taz_v1_TaskState_TASK_STATE_RUNNING);
                        EXPECT_EQ(task->pid, resp.pid);
                        EXPECT_EQ(task->stream_id, 65U);
                    });

    // Task finished during the run above (monitor defaults to true).
    const detached_task_t *task =
        taz_task_find(&Dispatch().tasks, resp.task_id);
    ASSERT_NE(task, nullptr);
    EXPECT_EQ(task->state, taz_v1_TaskState_TASK_STATE_COMPLETED);
    EXPECT_EQ(UnrefCount(), 1);

    FreeFinishedTask(Dispatch().tasks, resp.task_id);
}

// ---------------------------------------------------------------------------
// on_done outcomes: COMPLETED/FAILED/CANCELLED, result/error, retention
// ---------------------------------------------------------------------------

TEST_F(FileHandlerTest, DetachCompletedStoresDecodableResult)
{
    const std::string exe = SelfExePath();
    ASSERT_FALSE(exe.empty());
    const auto payload = DetachExecRequestBytes(
        exe, {"--gtest_filter=NoSuchSuite.*"}, /*monitor=*/true);

    DispatchRequestNoRun(taz_v1_Opcode_OPCODE_DETACH, payload, 66U);
    ASSERT_EQ(Frames().size(), 1U);
    taz_v1_DetachResponse resp = taz_v1_DetachResponse_init_zero;
    ASSERT_TRUE(DecodeDetachResponse(Frames()[0], &resp));
    RunLoop();

    const detached_task_t *task =
        taz_task_find(&Dispatch().tasks, resp.task_id);
    ASSERT_NE(task, nullptr);
    EXPECT_EQ(task->state, taz_v1_TaskState_TASK_STATE_COMPLETED);

    taz_v1_CommandExecResponse exec_resp = taz_v1_CommandExecResponse_init_zero;
    ASSERT_TRUE(DecodeExecResponse(task, &exec_resp));
    EXPECT_EQ(exec_resp.exit_code, 0);

    FreeFinishedTask(Dispatch().tasks, resp.task_id);
}

TEST_F(FileHandlerTest, DetachNonzeroExitIsFailedWithInternalErrorMessage)
{
    const auto payload = DetachExecRequestBytes(TAZ_TEST_EXITER_PATH, {"7"},
                                                /*monitor=*/true);
    DispatchRequestNoRun(taz_v1_Opcode_OPCODE_DETACH, payload, 67U);
    ASSERT_EQ(Frames().size(), 1U);
    taz_v1_DetachResponse resp = taz_v1_DetachResponse_init_zero;
    ASSERT_TRUE(DecodeDetachResponse(Frames()[0], &resp));
    RunLoop();

    const detached_task_t *task =
        taz_task_find(&Dispatch().tasks, resp.task_id);
    ASSERT_NE(task, nullptr);
    EXPECT_EQ(task->state, taz_v1_TaskState_TASK_STATE_FAILED);
    EXPECT_EQ(task->error_code, taz_v1_ErrorCode_ERROR_CODE_INTERNAL);
    EXPECT_STREQ(task->error_message, "exit code 7");
    // The result is still attached on FAILED, not only COMPLETED.
    taz_v1_CommandExecResponse exec_resp = taz_v1_CommandExecResponse_init_zero;
    EXPECT_TRUE(DecodeExecResponse(task, &exec_resp));
    EXPECT_EQ(exec_resp.exit_code, 7);

    FreeFinishedTask(Dispatch().tasks, resp.task_id);
}

TEST_F(FileHandlerTest, DetachTimeoutIsFailedWithTimeoutError)
{
    const auto payload = DetachExecRequestBytes(
        TAZ_TEST_SLEEPER_PATH, {}, /*monitor=*/true,
        taz_v1_ErrorPriority_ERROR_PRIORITY_NONE, /*timeout_ms=*/200U);
    DispatchRequestNoRun(taz_v1_Opcode_OPCODE_DETACH, payload, 68U);
    ASSERT_EQ(Frames().size(), 1U);
    taz_v1_DetachResponse resp = taz_v1_DetachResponse_init_zero;
    ASSERT_TRUE(DecodeDetachResponse(Frames()[0], &resp));

    const auto start = std::chrono::steady_clock::now();
    RunLoop();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_LT(elapsed, std::chrono::seconds(10));

    const detached_task_t *task =
        taz_task_find(&Dispatch().tasks, resp.task_id);
    ASSERT_NE(task, nullptr);
    EXPECT_EQ(task->state, taz_v1_TaskState_TASK_STATE_FAILED);
    EXPECT_EQ(task->error_code, taz_v1_ErrorCode_ERROR_CODE_TIMEOUT);

    FreeFinishedTask(Dispatch().tasks, resp.task_id);
}

TEST_F(FileHandlerTest, DetachMonitorFalseIsFreedOnFinish)
{
    const std::string exe = SelfExePath();
    ASSERT_FALSE(exe.empty());
    const auto payload = DetachExecRequestBytes(
        exe, {"--gtest_filter=NoSuchSuite.*"}, /*monitor=*/false);
    DispatchRequestNoRun(taz_v1_Opcode_OPCODE_DETACH, payload, 69U);
    ASSERT_EQ(Frames().size(), 1U);
    taz_v1_DetachResponse resp = taz_v1_DetachResponse_init_zero;
    ASSERT_TRUE(DecodeDetachResponse(Frames()[0], &resp));
    RunLoop();

    EXPECT_EQ(taz_task_find(&Dispatch().tasks, resp.task_id), nullptr);
}

TEST_F(FileHandlerTest, DetachCancelledNeverEscalates)
{
    const auto payload =
        DetachExecRequestBytes(TAZ_TEST_SLEEPER_PATH, {}, /*monitor=*/true,
                               taz_v1_ErrorPriority_ERROR_PRIORITY_CRITICAL);
    DispatchRequestNoRun(taz_v1_Opcode_OPCODE_DETACH, payload, 70U);
    ASSERT_EQ(Frames().size(), 1U);
    taz_v1_DetachResponse resp = taz_v1_DetachResponse_init_zero;
    ASSERT_TRUE(DecodeDetachResponse(Frames()[0], &resp));

    const detached_task_t *task =
        taz_task_find(&Dispatch().tasks, resp.task_id);
    ASSERT_NE(task, nullptr);
    ASSERT_NE(task->exec, nullptr);
    taz_exec_cancel(task->exec);

    RunLoop();

    task = taz_task_find(&Dispatch().tasks, resp.task_id);
    ASSERT_NE(task, nullptr);
    EXPECT_EQ(task->state, taz_v1_TaskState_TASK_STATE_CANCELLED);
    // No PRIORITY ERROR: a CANCELLED task never escalates, even with
    // error_priority CRITICAL.
    EXPECT_EQ(Frames().size(), 1U);
}

// ---------------------------------------------------------------------------
// PRIORITY emission
// ---------------------------------------------------------------------------

TEST_F(FileHandlerTest, DetachFailedCriticalEmitsErrorWithPriorityFlag)
{
    const auto payload =
        DetachExecRequestBytes(TAZ_TEST_EXITER_PATH, {"3"}, /*monitor=*/false,
                               taz_v1_ErrorPriority_ERROR_PRIORITY_CRITICAL);
    DispatchRequest(taz_v1_Opcode_OPCODE_DETACH, payload, 71U);

    ASSERT_EQ(Frames().size(), 2U);
    taz_v1_DetachResponse resp = taz_v1_DetachResponse_init_zero;
    ASSERT_TRUE(DecodeDetachResponse(Frames()[0], &resp));

    const taz_frame_header_t h = unpack_header(Frames()[1]);
    EXPECT_EQ(h.type, static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_ERROR));
    EXPECT_NE(h.flags &
                  static_cast<uint8_t>(taz_v1_FrameFlag_FRAME_FLAG_PRIORITY),
              0U);
    EXPECT_EQ(h.stream_id, 71U);
    EXPECT_EQ(h.opcode, static_cast<uint16_t>(taz_v1_Opcode_OPCODE_DETACH));

    taz_v1_ErrorInfo info = taz_v1_ErrorInfo_init_zero;
    ASSERT_TRUE(DecodeErrorInfo(Frames()[1], &info));
    EXPECT_EQ(info.code, taz_v1_ErrorCode_ERROR_CODE_INTERNAL);
    EXPECT_STREQ(info.message, "exit code 3");
}

TEST_F(FileHandlerTest, DetachPriorityNotEmittedWhileConnectionIsClosing)
{
    const auto payload =
        DetachExecRequestBytes(TAZ_TEST_EXITER_PATH, {"3"}, /*monitor=*/false,
                               taz_v1_ErrorPriority_ERROR_PRIORITY_CRITICAL);
    DispatchRequestNoRun(taz_v1_Opcode_OPCODE_DETACH, payload, 72U);
    ASSERT_EQ(Frames().size(), 1U);

    SetConnClosing(1);
    RunLoop();

    // Only the original DetachResponse - no PRIORITY ERROR once the
    // connection has started closing.
    EXPECT_EQ(Frames().size(), 1U);
}

// ---------------------------------------------------------------------------
// Exit-code registry wiring
// ---------------------------------------------------------------------------

TEST_F(FileHandlerTest, DetachRegistersAndRecordsExitInTheRegistry)
{
    taz_task_registry_reset_for_tests();

    const auto payload = DetachExecRequestBytes(TAZ_TEST_EXITER_PATH, {"5"},
                                                /*monitor=*/true);
    DispatchRequestNoRun(taz_v1_Opcode_OPCODE_DETACH, payload, 73U);
    ASSERT_EQ(Frames().size(), 1U);
    taz_v1_DetachResponse resp = taz_v1_DetachResponse_init_zero;
    ASSERT_TRUE(DecodeDetachResponse(Frames()[0], &resp));
    RunLoop();

    const detached_task_t *task =
        taz_task_find(&Dispatch().tasks, resp.task_id);
    ASSERT_NE(task, nullptr);
    EXPECT_EQ(task->state, taz_v1_TaskState_TASK_STATE_FAILED);

    int64_t exit_status = 0;
    EXPECT_EQ(
        taz_task_registry_lookup(task->pid, task->start_key, &exit_status), 1);
    EXPECT_EQ(exit_status, 5);

    FreeFinishedTask(Dispatch().tasks, resp.task_id);
}

TEST_F(FileHandlerTest,
       DetachEvictsOldestFinishedMonitorTrueTaskButKeepsItsRegistryEntry)
{
    // Fill the table directly via the task API (taz_task_alloc's own
    // eviction order is proven by test_task.cpp). The first slot filled
    // finishes first, so it is the one the next DETACH evicts.
    const uint32_t victim_pid = 999001U;
    const uint64_t victim_start_key = 123456789U;
    ASSERT_EQ(taz_task_registry_register(victim_pid, victim_start_key), 0);
    taz_task_registry_record_exit(victim_pid, victim_start_key, 4);

    uint32_t victim_task_id = 0U;
    for (uint32_t i = 0; i < TAZ_TASK_SLOTS; i++)
    {
        const uint32_t task_id =
            taz_task_alloc(&Dispatch().tasks, 1000U + i,
                           taz_v1_ErrorPriority_ERROR_PRIORITY_NONE, true);
        ASSERT_NE(task_id, 0U);
        if (i == 0U)
        {
            victim_task_id = task_id;
            taz_task_set_pid(&Dispatch().tasks, task_id, victim_pid,
                             victim_start_key);
        }
        taz_task_set_state(&Dispatch().tasks, task_id,
                           taz_v1_TaskState_TASK_STATE_COMPLETED);
    }

    const std::string exe = SelfExePath();
    ASSERT_FALSE(exe.empty());
    const auto payload = DetachExecRequestBytes(
        exe, {"--gtest_filter=NoSuchSuite.*"}, /*monitor=*/true);
    DispatchRequest(taz_v1_Opcode_OPCODE_DETACH, payload, 2000U);

    ASSERT_EQ(Frames().size(), 1U);
    taz_v1_DetachResponse resp = taz_v1_DetachResponse_init_zero;
    ASSERT_TRUE(DecodeDetachResponse(Frames()[0], &resp));
    EXPECT_NE(resp.task_id, 0U);
    EXPECT_EQ(taz_task_find(&Dispatch().tasks, victim_task_id), nullptr);

    // The registry is independent of the table: PROCESS_MONITOR can still
    // report the evicted task's exit code.
    int64_t exit_code = 0;
    EXPECT_EQ(
        taz_task_registry_lookup(victim_pid, victim_start_key, &exit_code), 1);
    EXPECT_EQ(exit_code, 4);
}

TEST_F(FileHandlerTest, DetachMonitorFalseTaskKeepsItsRegistryEntryOnceFreed)
{
    const auto payload = DetachExecRequestBytes(TAZ_TEST_EXITER_PATH, {"6"},
                                                /*monitor=*/false);
    DispatchRequestNoRun(taz_v1_Opcode_OPCODE_DETACH, payload, 74U);
    ASSERT_EQ(Frames().size(), 1U);
    taz_v1_DetachResponse resp = taz_v1_DetachResponse_init_zero;
    ASSERT_TRUE(DecodeDetachResponse(Frames()[0], &resp));
    const detached_task_t *task =
        taz_task_find(&Dispatch().tasks, resp.task_id);
    ASSERT_NE(task, nullptr);
    const uint32_t pid = task->pid;
    const uint64_t start_key = task->start_key;
    ASSERT_NE(start_key, 0U);

    RunLoop();

    EXPECT_EQ(taz_task_find(&Dispatch().tasks, resp.task_id), nullptr);
    int64_t exit_code = 0;
    EXPECT_EQ(taz_task_registry_lookup(pid, start_key, &exit_code), 1);
    EXPECT_EQ(exit_code, 6);
}

TEST_F(FileHandlerTest, DetachKilledProcessRecordsItsSignalNotExitZero)
{
    const auto payload = DetachExecRequestBytes(TAZ_TEST_SLEEPER_PATH, {},
                                                /*monitor=*/true);
    DispatchRequestNoRun(taz_v1_Opcode_OPCODE_DETACH, payload, 75U);
    ASSERT_EQ(Frames().size(), 1U);
    taz_v1_DetachResponse resp = taz_v1_DetachResponse_init_zero;
    ASSERT_TRUE(DecodeDetachResponse(Frames()[0], &resp));
    const detached_task_t *task =
        taz_task_find(&Dispatch().tasks, resp.task_id);
    ASSERT_NE(task, nullptr);
    ASSERT_NE(task->exec, nullptr);
    const uint32_t pid = task->pid;
    const uint64_t start_key = task->start_key;

    taz_exec_cancel(task->exec);
    RunLoop();

    int64_t exit_code = 0;
    ASSERT_EQ(taz_task_registry_lookup(pid, start_key, &exit_code), 1);
#ifdef _WIN32
    // TerminateJobObject's exit code; Windows has no signals.
    EXPECT_EQ(exit_code, 1);
#else
    EXPECT_EQ(exit_code, -SIGKILL);
#endif
}

TEST_F(FileHandlerTest, DetachDuringShutdownIsBusyAndCreatesNoTask)
{
    taz_work_begin_shutdown();
    const auto payload = DetachExecRequestBytes(TAZ_TEST_EXITER_PATH, {"0"});
    DispatchRequestNoRun(taz_v1_Opcode_OPCODE_DETACH, payload, 76U);
    taz_work_reset_for_tests();

    ASSERT_EQ(Frames().size(), 1U);
    taz_v1_ErrorInfo info = taz_v1_ErrorInfo_init_zero;
    ASSERT_TRUE(DecodeErrorInfo(Frames()[0], &info));
    EXPECT_EQ(info.code, taz_v1_ErrorCode_ERROR_CODE_BUSY);
    EXPECT_STREQ(info.message, "daemon is shutting down");
    EXPECT_TRUE(TableIsEmpty(Dispatch().tasks));
}

TEST_F(FileHandlerTest, DetachSpawnFailureOnAFullTableEvictsNothing)
{
    std::vector<uint32_t> ids;
    for (uint32_t i = 0; i < TAZ_TASK_SLOTS; i++)
    {
        const uint32_t task_id =
            taz_task_alloc(&Dispatch().tasks, 4000U + i,
                           taz_v1_ErrorPriority_ERROR_PRIORITY_NONE, true);
        ASSERT_NE(task_id, 0U);
        taz_task_set_state(&Dispatch().tasks, task_id,
                           taz_v1_TaskState_TASK_STATE_COMPLETED);
        ids.push_back(task_id);
    }

    const auto payload = DetachExecRequestBytes("/nonexistent/taz-no-such-cmd");
    DispatchRequest(taz_v1_Opcode_OPCODE_DETACH, payload, 77U);

    ASSERT_EQ(Frames().size(), 1U);
    taz_v1_ErrorInfo info = taz_v1_ErrorInfo_init_zero;
    ASSERT_TRUE(DecodeErrorInfo(Frames()[0], &info));
    EXPECT_EQ(info.code, taz_v1_ErrorCode_ERROR_CODE_NOT_FOUND);
    for (const uint32_t id : ids)
    {
        EXPECT_NE(taz_task_find(&Dispatch().tasks, id), nullptr) << id;
    }
}

TEST_F(FileHandlerTest, DetachBusyWhenTheRegistryIsFullOfRunningProcesses)
{
    for (uint32_t i = 0; i < TAZ_TASK_REGISTRY_CAPACITY; i++)
    {
        ASSERT_EQ(taz_task_registry_register(5000000U + i, 1U), 0);
    }

    const auto payload = DetachExecRequestBytes(TAZ_TEST_EXITER_PATH, {"0"});
    DispatchRequest(taz_v1_Opcode_OPCODE_DETACH, payload, 78U);

    ASSERT_EQ(Frames().size(), 1U);
    taz_v1_ErrorInfo info = taz_v1_ErrorInfo_init_zero;
    ASSERT_TRUE(DecodeErrorInfo(Frames()[0], &info));
    EXPECT_EQ(info.code, taz_v1_ErrorCode_ERROR_CODE_BUSY);
    EXPECT_STREQ(info.message, "too many detached processes are running");
    EXPECT_TRUE(TableIsEmpty(Dispatch().tasks));
}

#ifndef _WIN32
TEST_F(FileHandlerTest, DetachIgnoresTheConnectionDefaultTimeout)
{
    Dispatch().conn_timeout_ms = 100U;
    const auto payload =
        DetachExecRequestBytes("/bin/sh", {"-c", "sleep 0.5; echo done"}, true);
    DispatchRequest(taz_v1_Opcode_OPCODE_DETACH, payload, 79U);

    ASSERT_EQ(Frames().size(), 1U);
    taz_v1_DetachResponse resp = taz_v1_DetachResponse_init_zero;
    ASSERT_TRUE(DecodeDetachResponse(Frames()[0], &resp));
    const detached_task_t *task =
        taz_task_find(&Dispatch().tasks, resp.task_id);
    ASSERT_NE(task, nullptr);
    EXPECT_EQ(task->state, taz_v1_TaskState_TASK_STATE_COMPLETED);
    taz_v1_CommandExecResponse result = taz_v1_CommandExecResponse_init_zero;
    ASSERT_TRUE(DecodeExecResponse(task, &result));
    EXPECT_FALSE(result.timed_out);
    EXPECT_EQ(TaskStdout(task), "done\n");
}

TEST_F(FileHandlerTest, DetachRunsAsTheConnectionRunAsIdentity)
{
    // The connection identity is applied without a privilege check (RUN_AS
    // itself checked it), and switching to this process's own uid/gid
    // needs no privilege, so this runs the same as a normal user or root.
    Dispatch().run_as_active = 1;
    Dispatch().run_as_uid = static_cast<uint32_t>(getuid());
    Dispatch().run_as_gid = static_cast<uint32_t>(getgid());
    (void)std::snprintf(Dispatch().run_as_user, sizeof(Dispatch().run_as_user),
                        "%s", "taz-run-as");
    (void)std::snprintf(Dispatch().run_as_home, sizeof(Dispatch().run_as_home),
                        "%s", "/taz-home");

    const auto payload = DetachExecRequestBytes(
        "/bin/sh", {"-c", "printf '%s %s %s' \"$USER\" \"$LOGNAME\" \"$HOME\""},
        true);
    DispatchRequest(taz_v1_Opcode_OPCODE_DETACH, payload, 80U);

    ASSERT_EQ(Frames().size(), 1U);
    taz_v1_DetachResponse resp = taz_v1_DetachResponse_init_zero;
    ASSERT_TRUE(DecodeDetachResponse(Frames()[0], &resp));
    const detached_task_t *task =
        taz_task_find(&Dispatch().tasks, resp.task_id);
    ASSERT_NE(task, nullptr);
    EXPECT_EQ(task->state, taz_v1_TaskState_TASK_STATE_COMPLETED);
    EXPECT_EQ(TaskStdout(task), "taz-run-as taz-run-as /taz-home");
}
#endif /* !_WIN32 */

#ifdef _WIN32
// The per-spawn token duplicate, through detach.c's own wiring rather than
// test_command.cpp's direct calls into the shared taz_command_resolve_as_user:
// the connection identity's token reaches a real CreateProcessAsUserW spawn. No
// second account exists yet, so this duplicates the test process's own token -
// OwnPrimaryToken's reason covers the missing-privilege skip. Spawns
// System32's own cmd.exe, not this (possibly ASan-instrumented) test
// binary re-exec'd - the as-user path's own env block carries no ASan
// runtime DLL directory, which test_exec.cpp's ExecSpawnTest rows handle
// with a PATH override this row has no need for.
TEST_F(FileHandlerTest, DetachRunsAsTheConnectionRunAsIdentityOnWindows)
{
    std::string reason;
    ScopedHandle own_token = OwnPrimaryToken(&reason);
    if (!own_token.valid())
    {
        GTEST_SKIP() << reason;
    }

    char system_root[1024];
    const DWORD system_root_len = GetEnvironmentVariableA(
        "SystemRoot", system_root, static_cast<DWORD>(sizeof(system_root)));
    ASSERT_GT(system_root_len, 0U);
    const std::string cmd_exe =
        std::string(system_root, system_root_len) + "\\System32\\cmd.exe";

    Dispatch().run_as_active = 1;
    // Ownership moves to the connection; FileHandlerTest::TearDown's
    // taz_run_as_reset closes it, mirroring a real connection close.
    Dispatch().run_as_token = own_token.release();
    (void)std::snprintf(Dispatch().run_as_user, sizeof(Dispatch().run_as_user),
                        "%s", "whoever");

    const auto payload = DetachExecRequestBytes(cmd_exe, {"/c", "exit 0"});
    DispatchRequest(taz_v1_Opcode_OPCODE_DETACH, payload, 90U);

    ASSERT_EQ(Frames().size(), 1U);
    taz_v1_DetachResponse resp = taz_v1_DetachResponse_init_zero;
    ASSERT_TRUE(DecodeDetachResponse(Frames()[0], &resp));
    const detached_task_t *task =
        taz_task_find(&Dispatch().tasks, resp.task_id);
    ASSERT_NE(task, nullptr);
    EXPECT_EQ(task->state, taz_v1_TaskState_TASK_STATE_COMPLETED);
}
#endif /* _WIN32 */

// taz_task_alloc only evicts a *finished* monitor=true slot; a table full
// of still-running tasks has nothing eligible to evict, so it returns 0 and
// detach.c maps that to BUSY, creating no task.
TEST_F(FileHandlerTest,
       DetachBusyWhenTableFullOfStillRunningTasksCreatesNoNewTask)
{
    for (uint32_t i = 0; i < TAZ_TASK_SLOTS; i++)
    {
        const uint32_t task_id =
            taz_task_alloc(&Dispatch().tasks, 3000U + i,
                           taz_v1_ErrorPriority_ERROR_PRIORITY_NONE, true);
        ASSERT_NE(task_id, 0U);
        taz_task_set_state(&Dispatch().tasks, task_id,
                           taz_v1_TaskState_TASK_STATE_RUNNING);
    }

    const auto payload = DetachExecRequestBytes("/no/such/taz-test-binary-xyz");
    DispatchRequest(taz_v1_Opcode_OPCODE_DETACH, payload, 4000U);

    ASSERT_EQ(Frames().size(), 1U);
    taz_v1_ErrorInfo info = taz_v1_ErrorInfo_init_zero;
    ASSERT_TRUE(DecodeErrorInfo(Frames()[0], &info));
    EXPECT_EQ(info.code, taz_v1_ErrorCode_ERROR_CODE_BUSY);
    EXPECT_EQ(RefCount(), 0);

    // Every slot still holds one of the 64 still-running tasks filled
    // above - the rejected DETACH took none of them.
    EXPECT_EQ(std::count_if(std::begin(Dispatch().tasks.slots),
                            std::end(Dispatch().tasks.slots),
                            [](const detached_task_t &slot)
                            { return slot.task_id != 0U; }),
              static_cast<std::ptrdiff_t>(TAZ_TASK_SLOTS));
}

} // namespace
