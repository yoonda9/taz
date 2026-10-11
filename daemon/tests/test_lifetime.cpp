// Unit tests for detached-task lifetime: a detached task's process tree
// must never be left running past the connection, or the daemon, that
// owned it.
//
//   - task.c's taz_task_table_abort is the shared primitive: kill every
//     still-running task's tree, touch nothing else.
//   - dispatch.c's taz_dispatch_cancel_all (already called by
//     connection.c on connection close) now calls it too, so a closing
//     connection's detached tasks die along with everything else it was
//     running.
//   - connection.c's taz_conn_kill_all_task_trees walks its list of live
//     connections on a loop and does the same, for the SIGTERM path (wired
//     through work.c's shutdown hook in src/main.c; that wiring is tested
//     separately in test_work.cpp, since it needs no connection at all).

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
#include "taz/connection.h"
#include "taz/server.h"
#include "taz/task.h"
#include "taz/task_registry.h"
#include "taz/v1/advanced.pb.h"
#include "taz/v1/command.pb.h"
#include "taz/v1/common.pb.h"

namespace
{

// ---------------------------------------------------------------------------
// A real spawned exec, driven directly (not through handlers/detach.c),
// for tests that need an actual process tree to kill without the full
// DETACH request/response plumbing. Mirrors detach.c's own
// "drop the exec pointer before anything else can reach it" rule: once
// on_done fires, it clears the task table's exec pointer itself, so a
// later taz_task_table_abort/taz_dispatch_cancel_all pass (e.g. from a
// connection actually closing afterwards) never touches a dangling
// pointer.
// ---------------------------------------------------------------------------

struct ExecDone
{
    bool done = false;
    bool cancelled = false;
    taz_task_table_t *table = nullptr;
    uint32_t task_id = 0U;
};

void OnExecDone(const taz_exec_result_t *result, void *arg)
{
    auto *ed = static_cast<ExecDone *>(arg);
    ed->done = true;
    ed->cancelled = result->cancelled;
    if (ed->table != nullptr)
    {
        taz_task_set_exec(ed->table, ed->task_id, nullptr);
    }
}

constexpr size_t kMaxOutputBytes = static_cast<size_t>(1024U) * 1024U;

int StartSleeper(uv_loop_t *loop, ExecDone *ed, taz_exec_t **out)
{
    taz_exec_spec_t spec;
    (void)std::memset(&spec, 0, sizeof(spec));
    spec.file = TAZ_TEST_SLEEPER_PATH;
    spec.max_output_bytes = kMaxOutputBytes;
    return taz_exec_start(loop, &spec, OnExecDone, ed, out);
}

// ---------------------------------------------------------------------------
// Request builders / response decoders (DETACH exec + subscribed
// TASK_STATUS), mirroring test_task_cancel.cpp's/test_task_status.cpp's
// own local copies - each test TU keeps its own per this codebase's
// established precedent.
// ---------------------------------------------------------------------------

std::vector<uint8_t> DetachSleeperRequestBytes(bool monitor = true)
{
    auto req = std::make_unique<taz_v1_DetachRequest>();
    (void)std::memset(req.get(), 0, sizeof(*req));
    req->which_task = taz_v1_DetachRequest_exec_tag;
    (void)std::strncpy(req->task.exec.command, TAZ_TEST_SLEEPER_PATH,
                       sizeof(req->task.exec.command) - 1U);
    req->monitor = monitor;

    std::vector<uint8_t> buf(taz_v1_DetachRequest_size);
    pb_ostream_t ostream = pb_ostream_from_buffer(buf.data(), buf.size());
    EXPECT_TRUE(pb_encode(&ostream, taz_v1_DetachRequest_fields, req.get()));
    buf.resize(ostream.bytes_written);
    return buf;
}

std::vector<uint8_t> TaskStatusSubscribeRequestBytes(uint32_t task_id,
                                                     uint32_t interval_ms)
{
    taz_v1_TaskStatusRequest req = taz_v1_TaskStatusRequest_init_zero;
    req.task_id = task_id;
    req.subscribe = true;
    req.interval_ms = interval_ms;

    std::vector<uint8_t> buf(taz_v1_TaskStatusRequest_size);
    pb_ostream_t ostream = pb_ostream_from_buffer(buf.data(), buf.size());
    EXPECT_TRUE(pb_encode(&ostream, taz_v1_TaskStatusRequest_fields, &req));
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

// Same polling idiom as test_task_status.cpp's own RunLoopUntil.
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

} // namespace

// ---------------------------------------------------------------------------
// taz_task_table_abort: the shared primitive
// ---------------------------------------------------------------------------

TEST_F(FileHandlerTest, TaskTableAbortKillsRunningTasksButLeavesFinishedAlone)
{
    ExecDone running;
    taz_exec_t *exec = nullptr;
    ASSERT_EQ(StartSleeper(Loop(), &running, &exec), 0);

    const uint32_t running_id = taz_task_alloc(
        &Dispatch().tasks, 0U, taz_v1_ErrorPriority_ERROR_PRIORITY_NONE, true);
    ASSERT_NE(running_id, 0U);
    running.table = &Dispatch().tasks;
    running.task_id = running_id;
    taz_task_set_exec(&Dispatch().tasks, running_id, exec);
    taz_task_set_state(&Dispatch().tasks, running_id,
                       taz_v1_TaskState_TASK_STATE_RUNNING);

    const uint32_t finished_id = taz_task_alloc(
        &Dispatch().tasks, 0U, taz_v1_ErrorPriority_ERROR_PRIORITY_NONE, true);
    ASSERT_NE(finished_id, 0U);
    taz_task_set_state(&Dispatch().tasks, finished_id,
                       taz_v1_TaskState_TASK_STATE_COMPLETED);

    taz_task_table_abort(&Dispatch().tasks);
    RunLoop();

    EXPECT_TRUE(running.done);
    EXPECT_TRUE(running.cancelled);
    const detached_task_t *finished_slot =
        taz_task_find(&Dispatch().tasks, finished_id);
    ASSERT_NE(finished_slot, nullptr);
    EXPECT_EQ(finished_slot->state, taz_v1_TaskState_TASK_STATE_COMPLETED);

    taz_task_free(&Dispatch().tasks, running_id);
    taz_task_free(&Dispatch().tasks, finished_id);
}

TEST_F(FileHandlerTest, TaskTableAbortOnAnEmptyTableIsANoop)
{
    taz_task_table_abort(&Dispatch().tasks);
    RunLoop();
    SUCCEED();
}

// ---------------------------------------------------------------------------
// [LIFETIME] Connection close kills every still-running detached task,
// through the real DETACH handler and taz_dispatch_cancel_all (the path
// connection.c's conn_close already takes).
// ---------------------------------------------------------------------------

TEST_F(FileHandlerTest, CloseWithSeveralRunningDetachedTasksKillsEveryTree)
{
    constexpr uint32_t kTaskCount = 3U;
    uint32_t task_ids[kTaskCount];

    for (uint32_t i = 0U; i < kTaskCount; i++)
    {
        const auto payload = DetachSleeperRequestBytes();
        DispatchRequestNoRun(taz_v1_Opcode_OPCODE_DETACH, payload, 200U + i);
        ASSERT_EQ(Frames().size(), i + 1U);
        taz_v1_DetachResponse resp = taz_v1_DetachResponse_init_zero;
        ASSERT_TRUE(DecodeDetachResponse(Frames()[i], &resp));
        ASSERT_NE(resp.task_id, 0U);
        task_ids[i] = resp.task_id;

        const detached_task_t *task =
            taz_task_find(&Dispatch().tasks, resp.task_id);
        ASSERT_NE(task, nullptr);
        ASSERT_EQ(task->state, taz_v1_TaskState_TASK_STATE_RUNNING);
        ASSERT_NE(task->exec, nullptr);
    }

    CloseConnectionAndCancelAll();
    RunLoop();

    for (uint32_t i = 0U; i < kTaskCount; i++)
    {
        const detached_task_t *task =
            taz_task_find(&Dispatch().tasks, task_ids[i]);
        ASSERT_NE(task, nullptr);
        EXPECT_EQ(task->state, taz_v1_TaskState_TASK_STATE_CANCELLED);
        EXPECT_EQ(task->exec, nullptr);
        taz_task_free(&Dispatch().tasks, task_ids[i]);
    }
    EXPECT_EQ(RefCount(), UnrefCount());
}

// ---------------------------------------------------------------------------
// [LIFETIME] A subscriber stream closing as its (real, running) task's
// connection closes: the stream abort path and the new task-tree-kill
// path both fire on the same task without racing into a use-after-free.
// ---------------------------------------------------------------------------

TEST_F(FileHandlerTest, SubscriberStreamCloseRaceWithARealRunningTaskIsSafe)
{
    const auto detach_payload = DetachSleeperRequestBytes();
    DispatchRequestNoRun(taz_v1_Opcode_OPCODE_DETACH, detach_payload, 210U);
    ASSERT_EQ(Frames().size(), 1U);
    taz_v1_DetachResponse detach_resp = taz_v1_DetachResponse_init_zero;
    ASSERT_TRUE(DecodeDetachResponse(Frames()[0], &detach_resp));

    const detached_task_t *task =
        taz_task_find(&Dispatch().tasks, detach_resp.task_id);
    ASSERT_NE(task, nullptr);
    ASSERT_NE(task->exec, nullptr);

    DispatchRequestNoRun(
        taz_v1_Opcode_OPCODE_TASK_STATUS,
        TaskStatusSubscribeRequestBytes(detach_resp.task_id, 100U), 211U);
    RunLoopUntil(Loop(), [&]() { return Frames().size() >= 2U; }, 1000);

    CloseConnectionAndCancelAll();
    RunLoop();

    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());

    task = taz_task_find(&Dispatch().tasks, detach_resp.task_id);
    ASSERT_NE(task, nullptr);
    EXPECT_EQ(task->state, taz_v1_TaskState_TASK_STATE_CANCELLED);
    EXPECT_EQ(task->exec, nullptr);

    taz_task_free(&Dispatch().tasks, detach_resp.task_id);
}

// ---------------------------------------------------------------------------
// [LIFETIME] taz_conn_kill_all_task_trees: the SIGTERM path. A real
// taz_conn_t (created via the production accept path, not a fake
// dispatch) with a real running task; the listening socket itself must
// never be mistaken for one (it carries a taz_server_t *, not a
// taz_conn_t *, behind the exact same uv_tcp_t type), including before the
// first connection arrives.
// ---------------------------------------------------------------------------

namespace
{

struct FindConnCtx
{
    const uv_handle_t *skip_listener;
    const uv_handle_t *skip_client;
    taz_conn_t *found;
};

void FindConnWalkCb(uv_handle_t *handle, void *arg)
{
    auto *ctx = static_cast<FindConnCtx *>(arg);
    if (handle->type != UV_TCP || handle == ctx->skip_listener ||
        handle == ctx->skip_client)
    {
        return;
    }
    ctx->found = static_cast<taz_conn_t *>(handle->data);
}

void OnTestConnect(uv_connect_t *req, int status)
{
    *static_cast<bool *>(req->data) = (status == 0);
}

} // namespace

TEST(ConnectionLifetime, KillAllTaskTreesReachesConnectionsSkipsListener)
{
    taz_server_t server{};
    testing::internal::CaptureStdout();
    ASSERT_EQ(
        taz_server_start(&server, "127.0.0.1", 0, taz_conn_on_new_connection),
        0);
    (void)testing::internal::GetCapturedStdout();

    struct sockaddr_storage addr = {};
    int addrlen = static_cast<int>(sizeof(addr));
    ASSERT_EQ(uv_tcp_getsockname(&server.handle,
                                 reinterpret_cast<sockaddr *>(&addr), &addrlen),
              0);

    // The test's own client end: production code never creates it, and
    // FindConnWalkCb below must skip it.
    uv_tcp_t client = {};
    ASSERT_EQ(uv_tcp_init(&server.loop, &client), 0);
    uv_connect_t connect_req = {};
    bool connected = false;
    connect_req.data = &connected;
    ASSERT_EQ(uv_tcp_connect(&connect_req, &client,
                             reinterpret_cast<const sockaddr *>(&addr),
                             OnTestConnect),
              0);

    FindConnCtx fctx{reinterpret_cast<uv_handle_t *>(&server.handle),
                     reinterpret_cast<uv_handle_t *>(&client), nullptr};
    RunLoopUntil(
        &server.loop,
        [&]()
        {
            uv_walk(&server.loop, FindConnWalkCb, &fctx);
            return connected && fctx.found != nullptr;
        },
        1000);
    taz_conn_t *conn = fctx.found;
    ASSERT_NE(conn, nullptr);

    ExecDone ed;
    taz_exec_t *exec = nullptr;
    ASSERT_EQ(StartSleeper(&server.loop, &ed, &exec), 0);
    const uint32_t task_id =
        taz_task_alloc(&conn->dispatch.tasks, 0U,
                       taz_v1_ErrorPriority_ERROR_PRIORITY_NONE, true);
    ASSERT_NE(task_id, 0U);
    ed.table = &conn->dispatch.tasks;
    ed.task_id = task_id;
    taz_task_set_exec(&conn->dispatch.tasks, task_id, exec);
    taz_task_set_state(&conn->dispatch.tasks, task_id,
                       taz_v1_TaskState_TASK_STATE_RUNNING);

    taz_conn_kill_all_task_trees(&server.loop);
    RunLoopUntil(&server.loop, [&]() { return ed.done; }, 1000);

    EXPECT_TRUE(ed.cancelled);

    // Teardown: close every handle this test owns itself (the listening
    // socket included - once the accepted connection above closes, on its
    // own close path, nothing else would ever make a bare UV_RUN_ONCE
    // return: the still-open listener would just block waiting for a next
    // connection that never arrives) and drain to completion with a
    // single UV_RUN_DEFAULT.
    uv_close(reinterpret_cast<uv_handle_t *>(&client), nullptr);
    uv_close(reinterpret_cast<uv_handle_t *>(&server.handle), nullptr);
    ASSERT_EQ(uv_run(&server.loop, UV_RUN_DEFAULT), 0);
    ASSERT_EQ(uv_loop_close(&server.loop), 0);
}

// The listener carries a taz_server_t * in .data. A SIGTERM before any
// client has connected must not treat it as a connection.
TEST(ConnectionLifetime, KillAllTaskTreesBeforeAnyConnectionIsANoOp)
{
    taz_server_t server{};
    testing::internal::CaptureStdout();
    ASSERT_EQ(
        taz_server_start(&server, "127.0.0.1", 0, taz_conn_on_new_connection),
        0);
    (void)testing::internal::GetCapturedStdout();

    taz_conn_kill_all_task_trees(&server.loop);

    uv_close(reinterpret_cast<uv_handle_t *>(&server.handle), nullptr);
    ASSERT_EQ(uv_run(&server.loop, UV_RUN_DEFAULT), 0);
    ASSERT_EQ(uv_loop_close(&server.loop), 0);
}
