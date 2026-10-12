// Unit tests for the shutdown walk (taz/shutdown.h): taz_server_shutdown
// closes the listener, gracefully closes every connection (which kills
// every in-flight exec and detached task along the way, via the same
// taz_dispatch_cancel_all path connection close already took), and
// taz_shutdown_finish proves uv_run returned with nothing left open.
// No uv_stop anywhere: every row below lets uv_run return on its own.
//
// The drain deadline (a peer that never reads, so uv_shutdown cannot
// complete) needs a peer that accepts bytes but never reads them, which
// this process's own loop cannot arrange against itself; the integration
// suite covers it against a real daemon with an unread FILE_GET.

#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <vector>

#include <gtest/gtest.h>
#include <uv.h>

#include "taz/connection.h"
#include "taz/exec.h"
#include "taz/frame.h"
#include "taz/platform.h"
#include "taz/server.h"
#include "taz/shutdown.h"
#include "taz/task.h"
#include "taz/v1/advanced.pb.h"
#include "taz/v1/common.pb.h"
#include "taz/work.h"

#ifndef _WIN32
#include <csignal>
#endif

namespace
{

// Same polling idiom as test_lifetime.cpp's/test_task_status.cpp's own
// RunLoopUntil.
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
// A real spawned exec, driven directly (not through a handler), standing
// in for both handlers/command.c's stream-registered exec and
// handlers/detach.c's detached-task exec: both pin the connection with
// taz_dispatch_conn_ref right after a successful taz_exec_start and
// release it from their own on_done. stream_id == 0 means "no stream to
// release" (the detached-task case); table != nullptr means "also clear
// this task's exec pointer first" (mirrors detach.c's on_exec_done, which
// drops it before anything else can reach it).
// ---------------------------------------------------------------------------

struct ExecDoneCtx
{
    taz_dispatch_t *d = nullptr;
    uint32_t stream_id = 0U;
    taz_task_table_t *table = nullptr;
    uint32_t task_id = 0U;
    bool done = false;
    bool cancelled = false;
};

void OnTestExecDone(const taz_exec_result_t *result, void *arg)
{
    auto *ctx = static_cast<ExecDoneCtx *>(arg);
    ctx->done = true;
    ctx->cancelled = result->cancelled;
    if (ctx->table != nullptr)
    {
        taz_task_set_exec(ctx->table, ctx->task_id, nullptr);
    }
    if (ctx->stream_id != 0U)
    {
        taz_dispatch_stream_done(ctx->d, ctx->stream_id);
    }
    taz_dispatch_conn_unref(ctx->d);
}

constexpr size_t kMaxOutputBytes = static_cast<size_t>(1024U) * 1024U;

int StartSleeper(uv_loop_t *loop, ExecDoneCtx *ctx, taz_exec_t **out)
{
    taz_exec_spec_t spec;
    (void)std::memset(&spec, 0, sizeof(spec));
    spec.file = TAZ_TEST_SLEEPER_PATH;
    spec.max_output_bytes = kMaxOutputBytes;
    return taz_exec_start(loop, &spec, OnTestExecDone, ctx, out);
}

// ---------------------------------------------------------------------------
// Real-server/real-client plumbing, mirroring test_lifetime.cpp's
// ConnectionLifetime tests: the listener carries a taz_server_t * in
// .data behind the same uv_tcp_t type as an accepted taz_conn_t, so the
// walk must skip it, and the test's own client-side socket must be
// skipped too.
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// An idle daemon (no connection ever accepted).
// ---------------------------------------------------------------------------

TEST(Shutdown, IdleServerDrainsCleanly)
{
    taz_server_t server{};
    testing::internal::CaptureStdout();
    ASSERT_EQ(
        taz_server_start(&server, "127.0.0.1", 0, taz_conn_on_new_connection),
        0);
    (void)testing::internal::GetCapturedStdout();

    taz_server_shutdown(&server);

    ASSERT_EQ(uv_run(&server.loop, UV_RUN_DEFAULT), 0);
    EXPECT_EQ(taz_shutdown_finish(&server), 0);

    taz_work_reset_for_tests();
    taz_shutdown_reset_for_tests();
}

// ---------------------------------------------------------------------------
// A second taz_server_shutdown call is a no-op - no double close, no
// crash.
// ---------------------------------------------------------------------------

TEST(Shutdown, SecondWalkIsANoOp)
{
    taz_server_t server{};
    testing::internal::CaptureStdout();
    ASSERT_EQ(
        taz_server_start(&server, "127.0.0.1", 0, taz_conn_on_new_connection),
        0);
    (void)testing::internal::GetCapturedStdout();

    taz_server_shutdown(&server);
    taz_server_shutdown(&server);

    ASSERT_EQ(uv_run(&server.loop, UV_RUN_DEFAULT), 0);
    EXPECT_EQ(taz_shutdown_finish(&server), 0);

    taz_work_reset_for_tests();
    taz_shutdown_reset_for_tests();
}

// ---------------------------------------------------------------------------
// A connection with an in-flight (stream-registered) exec and a
// running detached task. Both trees are killed, the connection is freed,
// and uv_run returns on its own.
// ---------------------------------------------------------------------------

TEST(Shutdown, KillsInFlightStreamExecAndDetachedTask)
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

    // An in-flight stream exec, pinned like handlers/command.c's
    // COMMAND_EXEC.
    constexpr uint32_t kStreamId = 77U;
    conn->dispatch.active_streams[0] = kStreamId;
    conn->dispatch.active_count = 1U;
    ExecDoneCtx stream_ctx;
    stream_ctx.d = &conn->dispatch;
    stream_ctx.stream_id = kStreamId;
    taz_exec_t *stream_exec = nullptr;
    ASSERT_EQ(StartSleeper(&server.loop, &stream_ctx, &stream_exec), 0);
    taz_dispatch_set_stream_exec(&conn->dispatch, kStreamId, stream_exec);
    taz_dispatch_conn_ref(&conn->dispatch);

    // A running detached task, pinned like handlers/detach.c's own exec.
    const uint32_t task_id =
        taz_task_alloc(&conn->dispatch.tasks, 0U,
                       taz_v1_ErrorPriority_ERROR_PRIORITY_NONE, true);
    ASSERT_NE(task_id, 0U);
    ExecDoneCtx task_ctx;
    task_ctx.d = &conn->dispatch;
    task_ctx.table = &conn->dispatch.tasks;
    task_ctx.task_id = task_id;
    taz_exec_t *task_exec = nullptr;
    ASSERT_EQ(StartSleeper(&server.loop, &task_ctx, &task_exec), 0);
    taz_task_set_exec(&conn->dispatch.tasks, task_id, task_exec);
    taz_task_set_state(&conn->dispatch.tasks, task_id,
                       taz_v1_TaskState_TASK_STATE_RUNNING);
    taz_dispatch_conn_ref(&conn->dispatch);

    taz_server_shutdown(&server);

    // Safe now: taz_server_shutdown already synchronously marked conn
    // closing and dispatched both cancellations above, so the client
    // racing an EOF against it just hits conn_close's own closing guard.
    uv_close(reinterpret_cast<uv_handle_t *>(&client), nullptr);

    ASSERT_EQ(uv_run(&server.loop, UV_RUN_DEFAULT), 0);

    EXPECT_TRUE(stream_ctx.done);
    EXPECT_TRUE(stream_ctx.cancelled);
    EXPECT_TRUE(task_ctx.done);
    EXPECT_TRUE(task_ctx.cancelled);
    EXPECT_EQ(taz_conn_open_count(&server.loop), 0U);

    EXPECT_EQ(taz_shutdown_finish(&server), 0);

    taz_work_reset_for_tests();
    taz_shutdown_reset_for_tests();
}

// ---------------------------------------------------------------------------
// A frame read in the same iteration as the close began is dropped -
// on_frame_cb's closing guard, not taz_conn_handle_frame (already covered
// by test_connection.cpp without any taz_conn_t behind it).
// ---------------------------------------------------------------------------

namespace
{

struct ClientRecv
{
    std::vector<uint8_t> bytes;
    char scratch[4096];
};

void ClientAllocCb(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf)
{
    auto *recv = static_cast<ClientRecv *>(handle->data);
    const size_t n = suggested_size < sizeof(recv->scratch)
                         ? suggested_size
                         : sizeof(recv->scratch);
    *buf = uv_buf_init(recv->scratch, static_cast<unsigned int>(n));
}

void ClientReadCb(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf)
{
    auto *recv = static_cast<ClientRecv *>(stream->data);
    if (nread > 0)
    {
        recv->bytes.insert(recv->bytes.end(), buf->base, buf->base + nread);
    }
}

} // namespace

TEST(Shutdown, DropsFrameOnAConnectionAlreadyClosing)
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

    uv_tcp_t client = {};
    ClientRecv recv;
    client.data = &recv;
    ASSERT_EQ(uv_tcp_init(&server.loop, &client), 0);
    uv_connect_t connect_req = {};
    bool connected = false;
    connect_req.data = &connected;
    ASSERT_EQ(uv_tcp_connect(&connect_req, &client,
                             reinterpret_cast<const sockaddr *>(&addr),
                             OnTestConnect),
              0);
    // uv_read_start needs the connect to have actually completed first -
    // Windows's IOCP-backed uv_tcp_t returns UV_ENOTCONN otherwise, unlike
    // Linux, which tolerates arming the read before the handshake lands.
    RunLoopUntil(&server.loop, [&]() { return connected; }, 1000);
    ASSERT_EQ(uv_read_start(reinterpret_cast<uv_stream_t *>(&client),
                            ClientAllocCb, ClientReadCb),
              0);

    FindConnCtx fctx{reinterpret_cast<uv_handle_t *>(&server.handle),
                     reinterpret_cast<uv_handle_t *>(&client), nullptr};
    RunLoopUntil(
        &server.loop,
        [&]()
        {
            uv_walk(&server.loop, FindConnWalkCb, &fctx);
            return connected && fctx.found != nullptr && !recv.bytes.empty();
        },
        1000);
    taz_conn_t *conn = fctx.found;
    ASSERT_NE(conn, nullptr);

    // Let any trailing bytes of the initial CAPABILITY frame settle, then
    // snapshot: nothing else must ever arrive after this point.
    for (int i = 0; i < 5; i++)
    {
        uv_run(&server.loop, UV_RUN_NOWAIT);
    }
    const size_t before = recv.bytes.size();
    ASSERT_GT(before, 0U);

    // Simulate the shutdown walk having already begun on this connection,
    // without actually closing its handle, so on_read/on_frame_cb still
    // run for the PING below - the guard under test must be the one that
    // drops it, not the socket simply being gone.
    conn->closing = 1;

    taz_frame_header_t ping{};
    ping.type = static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_PING);
    ping.flags = 0U;
    ping.opcode = 0U;
    ping.length = 0U;
    ping.stream_id = 99U;
    auto frame_bytes =
        std::make_unique<uint8_t[]>(static_cast<size_t>(TAZ_FRAME_HEADER_SIZE));
    taz_frame_pack_header(&ping, frame_bytes.get());

    uv_write_t write_req{};
    const uv_buf_t wbuf =
        uv_buf_init(reinterpret_cast<char *>(frame_bytes.get()),
                    static_cast<unsigned int>(TAZ_FRAME_HEADER_SIZE));
    ASSERT_EQ(uv_write(&write_req, reinterpret_cast<uv_stream_t *>(&client),
                       &wbuf, 1U, nullptr),
              0);

    for (int i = 0; i < 10; i++)
    {
        uv_run(&server.loop, UV_RUN_NOWAIT);
    }

    EXPECT_EQ(recv.bytes.size(), before)
        << "a PING on a closing connection must not get a PONG";

    // Teardown: undo the manual override and let production code close
    // and free the connection for real, so nothing leaks under ASan.
    conn->closing = 0;
    taz_conn_shutdown_all(&server.loop);
    uv_close(reinterpret_cast<uv_handle_t *>(&client), nullptr);
    uv_close(reinterpret_cast<uv_handle_t *>(&server.handle), nullptr);
    ASSERT_EQ(uv_run(&server.loop, UV_RUN_DEFAULT), 0);
    ASSERT_EQ(uv_loop_close(&server.loop), 0);
}

// ---------------------------------------------------------------------------
// taz_shutdown_finish reports (and does not hide) a stray handle.
// ---------------------------------------------------------------------------

TEST(Shutdown, FinishReportsAStrayHandle)
{
    taz_server_t server{};
    testing::internal::CaptureStdout();
    ASSERT_EQ(
        taz_server_start(&server, "127.0.0.1", 0, taz_conn_on_new_connection),
        0);
    (void)testing::internal::GetCapturedStdout();

    // Close the listener (an active, still-listening handle would block
    // taz_shutdown_finish's own uv_run(DEFAULT) forever - this test wants
    // only the timer below left open) but deliberately leave this timer
    // never closed, never even started, so taz_shutdown_finish has
    // nothing else to wait on and uv_loop_close reports it as a stray
    // handle.
    taz_server_close_listener(&server);
    uv_timer_t leaked{};
    ASSERT_EQ(uv_timer_init(&server.loop, &leaked), 0);

    const int rc = taz_shutdown_finish(&server);
    EXPECT_NE(rc, 0);

    uv_close(reinterpret_cast<uv_handle_t *>(&leaked), nullptr);
    ASSERT_EQ(uv_run(&server.loop, UV_RUN_DEFAULT), 0);
    ASSERT_EQ(uv_loop_close(&server.loop), 0);
}

// ---------------------------------------------------------------------------
// A STOP arriving while a RESTART already drains overrides it, so the
// daemon exits instead of re-executing; a RESTART never overrides a STOP.
// ---------------------------------------------------------------------------

TEST(Shutdown, StopOverridesARestartButNotTheReverse)
{
    // No server is bound, so no walk runs: only the mode is under test.
    taz_shutdown_request(TAZ_SHUTDOWN_RESTART);
    EXPECT_EQ(taz_shutdown_mode(), TAZ_SHUTDOWN_RESTART);
    taz_shutdown_request(TAZ_SHUTDOWN_STOP);
    EXPECT_EQ(taz_shutdown_mode(), TAZ_SHUTDOWN_STOP);
    taz_shutdown_request(TAZ_SHUTDOWN_RESTART);
    EXPECT_EQ(taz_shutdown_mode(), TAZ_SHUTDOWN_STOP);

    taz_shutdown_reset_for_tests();
}

// ---------------------------------------------------------------------------
// A real SIGTERM delivered through taz_platform_init_signals reaches
// taz_shutdown_request via on_signal exactly like a live tazd, and uv_run
// returns on its own with the loop clean. The walk only unrefs the signal
// handles, so a repeat SIGTERM before taz_shutdown_finish is still caught.
// ---------------------------------------------------------------------------

#ifndef _WIN32

TEST(Shutdown, RealSigtermThroughPlatformSignalsReturnsCleanly)
{
    taz_server_t server{};
    testing::internal::CaptureStdout();
    ASSERT_EQ(
        taz_server_start(&server, "127.0.0.1", 0, taz_conn_on_new_connection),
        0);
    (void)testing::internal::GetCapturedStdout();

    taz_shutdown_bind(&server);
    taz_platform_init_signals(&server.loop);

    // Queued before uv_run ever polls the self-pipe libuv's signal watcher
    // reads from, so on_signal (and the walk it starts) runs entirely
    // inside the uv_run call below, exactly like a live tazd's SIGTERM.
    ASSERT_EQ(raise(SIGTERM), 0);

    ASSERT_EQ(uv_run(&server.loop, UV_RUN_DEFAULT), 0);
    EXPECT_EQ(taz_shutdown_mode(), TAZ_SHUTDOWN_STOP);

    // The walk has run, yet both signal handles are still installed (only
    // unref'd): a second SIGTERM now reaches on_signal, a no-op, instead of
    // the default action, which would kill this test binary outright.
    ASSERT_EQ(raise(SIGTERM), 0);
    ASSERT_EQ(uv_run(&server.loop, UV_RUN_NOWAIT), 0);
    EXPECT_EQ(taz_shutdown_mode(), TAZ_SHUTDOWN_STOP);

    EXPECT_EQ(taz_shutdown_finish(&server), 0);

    taz_work_reset_for_tests();
    taz_shutdown_reset_for_tests();
}

#endif // !_WIN32

// ---------------------------------------------------------------------------
// Windows: no console-control event can be raised safely against a unit
// test process (CTRL_C_EVENT/CTRL_BREAK_EVENT target the whole console
// process group, and CTRL_CLOSE_EVENT terminates it) - the integration
// fixture's CTRL_BREAK against a real tazd covers that path. This proves
// taz_platform_init_signals/release_signals/close_signals link and run on
// a real loop, that a released handle no longer keeps uv_run alive, and
// that closing twice is harmless.
// ---------------------------------------------------------------------------

#ifdef _WIN32

TEST(Shutdown, PlatformSignalsInitAndCloseOnWindows)
{
    uv_loop_t loop{};
    ASSERT_EQ(uv_loop_init(&loop), 0);

    taz_platform_init_signals(&loop);
    taz_platform_release_signals();
    ASSERT_EQ(uv_run(&loop, UV_RUN_DEFAULT), 0);

    taz_platform_close_signals();
    taz_platform_close_signals();
    ASSERT_EQ(uv_run(&loop, UV_RUN_DEFAULT), 0);
    ASSERT_EQ(uv_loop_close(&loop), 0);
}

#endif // _WIN32
