// Tests for taz/work.h: the thread-pool work module used by the file-ops
// handlers. A fake dispatch (counting conn_ref/conn_unref, a closing flag
// behind conn_closing, a single pre-populated active stream) stands in for
// connection.c.

#include <gtest/gtest.h>
#include <uv.h>

#include "taz/dispatch.h"
#include "taz/work.h"

namespace
{

struct FakeConn
{
    int ref_count = 0;
    int unref_count = 0;
    int closing = 0;
};

void CountRef(void *ctx)
{
    static_cast<FakeConn *>(ctx)->ref_count++;
}

void CountUnref(void *ctx)
{
    static_cast<FakeConn *>(ctx)->unref_count++;
}

int IsClosing(void *ctx)
{
    return static_cast<FakeConn *>(ctx)->closing;
}

void InitFakeDispatch(taz_dispatch_t *d, uv_loop_t *loop, FakeConn *conn)
{
    taz_dispatch_init(d);
    d->loop = loop;
    d->conn_ref = CountRef;
    d->conn_unref = CountUnref;
    d->conn_ctx = conn;
    d->conn_closing = IsClosing;
}

struct WorkCtx
{
    uv_thread_t work_thread{};
    uv_thread_t done_thread{};
    int done_calls = 0;
    int done_closing = -1;
    // Set by the test before submitting, so the done callback can record
    // state at call time and prove the module's ordering guarantee: done
    // runs before stream_done and unref.
    const taz_dispatch_t *d = nullptr;
    const FakeConn *conn = nullptr;
    size_t active_count_at_done = 0U;
    int unref_count_at_done = -1;
};

void CaptureWork(void *user)
{
    auto *w = static_cast<WorkCtx *>(user);
    w->work_thread = uv_thread_self();
}

void CaptureDone(void *user, int closing)
{
    auto *w = static_cast<WorkCtx *>(user);
    w->done_thread = uv_thread_self();
    w->done_calls++;
    w->done_closing = closing;
    w->active_count_at_done = w->d->active_count;
    w->unref_count_at_done = w->conn->unref_count;
}

} // namespace

TEST(Work, RunsOffLoopThreadAndCompletesOnLoopThread)
{
    uv_loop_t loop;
    ASSERT_EQ(uv_loop_init(&loop), 0);

    FakeConn conn;
    taz_dispatch_t d;
    InitFakeDispatch(&d, &loop, &conn);
    d.active_streams[0] = 42U;
    d.active_count = 1U;

    WorkCtx w;
    w.d = &d;
    w.conn = &conn;
    const uv_thread_t test_thread = uv_thread_self();

    ASSERT_EQ(taz_work_submit(&d, 42U, CaptureWork, CaptureDone, &w), 0);
    EXPECT_EQ(conn.ref_count, 1);

    ASSERT_EQ(uv_run(&loop, UV_RUN_DEFAULT), 0);
    ASSERT_EQ(uv_loop_close(&loop), 0);

    // The work callback ran on a pool thread, never the thread that drove
    // the loop; the done callback ran on this (the loop) thread.
    EXPECT_FALSE(uv_thread_equal(&w.work_thread, &test_thread));
    EXPECT_TRUE(uv_thread_equal(&w.done_thread, &test_thread));
    EXPECT_EQ(w.done_calls, 1);
    EXPECT_EQ(w.done_closing, 0);
    // done must run before stream_done and unref: at done time the stream
    // is still active and the connection has not yet been unreffed.
    EXPECT_EQ(w.active_count_at_done, 1U);
    EXPECT_EQ(w.unref_count_at_done, 0);
    EXPECT_EQ(d.active_count, 0U);
    EXPECT_EQ(conn.ref_count, 1);
    EXPECT_EQ(conn.unref_count, 1);
}

namespace
{

struct BlockingWorkCtx
{
    uv_sem_t started{};
    uv_sem_t release{};
    int done_calls = 0;
    int done_closing = -1;
    // Set by the test before submitting, so the done callback can record
    // state at call time and prove the module's ordering guarantee: done
    // runs before stream_done and unref.
    const taz_dispatch_t *d = nullptr;
    const FakeConn *conn = nullptr;
    size_t active_count_at_done = 0U;
    int unref_count_at_done = -1;
};

void BlockingWork(void *user)
{
    auto *w = static_cast<BlockingWorkCtx *>(user);
    uv_sem_post(&w->started);
    uv_sem_wait(&w->release);
}

void BlockingDone(void *user, int closing)
{
    auto *w = static_cast<BlockingWorkCtx *>(user);
    w->done_calls++;
    w->done_closing = closing;
    w->active_count_at_done = w->d->active_count;
    w->unref_count_at_done = w->conn->unref_count;
}

} // namespace

TEST(Work, CloseWhileInFlightStillDeliversDoneWithClosingTrue)
{
    uv_loop_t loop;
    ASSERT_EQ(uv_loop_init(&loop), 0);

    FakeConn conn;
    taz_dispatch_t d;
    InitFakeDispatch(&d, &loop, &conn);
    d.active_streams[0] = 7U;
    d.active_count = 1U;

    BlockingWorkCtx w;
    w.d = &d;
    w.conn = &conn;
    ASSERT_EQ(uv_sem_init(&w.started, 0U), 0);
    ASSERT_EQ(uv_sem_init(&w.release, 0U), 0);

    ASSERT_EQ(taz_work_submit(&d, 7U, BlockingWork, BlockingDone, &w), 0);

    // Kick the loop once (queues the pool's completion-notification
    // handle); the pool thread itself runs independently and signals
    // `started` once it has actually begun executing the work callback.
    ASSERT_NE(uv_run(&loop, UV_RUN_NOWAIT), 0);
    uv_sem_wait(&w.started);

    // Simulate the connection closing while the work is still in flight,
    // then let the blocked work callback finish.
    conn.closing = 1;
    uv_sem_post(&w.release);

    ASSERT_EQ(uv_run(&loop, UV_RUN_DEFAULT), 0);
    ASSERT_EQ(uv_loop_close(&loop), 0);

    EXPECT_EQ(w.done_calls, 1);
    EXPECT_EQ(w.done_closing, 1);
    // done must run before stream_done and unref: at done time the stream
    // is still active and the connection has not yet been unreffed.
    EXPECT_EQ(w.active_count_at_done, 1U);
    EXPECT_EQ(w.unref_count_at_done, 0);
    EXPECT_EQ(d.active_count, 0U);
    EXPECT_EQ(conn.ref_count, 1);
    EXPECT_EQ(conn.unref_count, 1);

    uv_sem_destroy(&w.started);
    uv_sem_destroy(&w.release);
}

TEST(Work, TwoSubmissionsOnDifferentStreamsCompleteIndependently)
{
    uv_loop_t loop;
    ASSERT_EQ(uv_loop_init(&loop), 0);

    FakeConn conn;
    taz_dispatch_t d;
    InitFakeDispatch(&d, &loop, &conn);
    d.active_streams[0] = 1U;
    d.active_streams[1] = 2U;
    d.active_count = 2U;

    WorkCtx w1;
    WorkCtx w2;
    w1.d = &d;
    w1.conn = &conn;
    w2.d = &d;
    w2.conn = &conn;
    ASSERT_EQ(taz_work_submit(&d, 1U, CaptureWork, CaptureDone, &w1), 0);
    ASSERT_EQ(taz_work_submit(&d, 2U, CaptureWork, CaptureDone, &w2), 0);
    EXPECT_EQ(conn.ref_count, 2);

    ASSERT_EQ(uv_run(&loop, UV_RUN_DEFAULT), 0);
    ASSERT_EQ(uv_loop_close(&loop), 0);

    EXPECT_EQ(w1.done_calls, 1);
    EXPECT_EQ(w1.done_closing, 0);
    EXPECT_EQ(w2.done_calls, 1);
    EXPECT_EQ(w2.done_closing, 0);
    EXPECT_EQ(d.active_count, 0U);
    EXPECT_EQ(conn.ref_count, 2);
    EXPECT_EQ(conn.unref_count, 2);
}
