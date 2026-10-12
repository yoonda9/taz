// Tests for taz/work.h: the thread-pool work module used by the file-ops
// handlers. A fake dispatch (counting conn_ref/conn_unref, a closing flag
// behind conn_closing, a single pre-populated active stream) stands in for
// connection.c.

#include <vector>

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

TEST(Work, ShutdownRequestedStaysStickyUntilReset)
{
    ASSERT_EQ(taz_work_shutdown_requested(), 0);

    taz_work_begin_shutdown();
    EXPECT_EQ(taz_work_shutdown_requested(), 1);

    // Idempotent: nothing outstanding, calling it again must not crash and
    // must not change the flag.
    taz_work_begin_shutdown();
    EXPECT_EQ(taz_work_shutdown_requested(), 1);

    taz_work_reset_for_tests();
    EXPECT_EQ(taz_work_shutdown_requested(), 0);
}

TEST(Work, SubmitAfterShutdownBeganIsStillAcceptedWithClosingTrue)
{
    uv_loop_t loop;
    ASSERT_EQ(uv_loop_init(&loop), 0);

    FakeConn conn;
    taz_dispatch_t d;
    InitFakeDispatch(&d, &loop, &conn);
    d.active_streams[0] = 3U;
    d.active_count = 1U;

    // A cleanup step must still be able to run after shutdown has begun:
    // taz_work_submit itself never refuses.
    taz_work_begin_shutdown();

    WorkCtx w;
    w.d = &d;
    w.conn = &conn;
    ASSERT_EQ(taz_work_submit(&d, 3U, CaptureWork, CaptureDone, &w), 0);

    ASSERT_EQ(uv_run(&loop, UV_RUN_DEFAULT), 0);
    ASSERT_EQ(uv_loop_close(&loop), 0);

    EXPECT_EQ(w.done_calls, 1);
    EXPECT_EQ(w.done_closing, 1);

    taz_work_reset_for_tests();
}

TEST(Work, ResetForTestsClearsFlagAndOutstandingCount)
{
    uv_loop_t loop;
    ASSERT_EQ(uv_loop_init(&loop), 0);

    FakeConn conn;
    taz_dispatch_t d;
    InitFakeDispatch(&d, &loop, &conn);
    d.active_streams[0] = 4U;
    d.active_count = 1U;

    WorkCtx w;
    w.d = &d;
    w.conn = &conn;

    EXPECT_EQ(taz_work_outstanding(), 0U);
    ASSERT_EQ(taz_work_submit(&d, 4U, CaptureWork, CaptureDone, &w), 0);
    EXPECT_EQ(taz_work_outstanding(), 1U);

    taz_work_begin_shutdown();
    EXPECT_EQ(taz_work_shutdown_requested(), 1);

    ASSERT_EQ(uv_run(&loop, UV_RUN_DEFAULT), 0);
    ASSERT_EQ(uv_loop_close(&loop), 0);
    EXPECT_EQ(taz_work_outstanding(), 0U);

    taz_work_reset_for_tests();
    EXPECT_EQ(taz_work_shutdown_requested(), 0);
    EXPECT_EQ(taz_work_outstanding(), 0U);
}

namespace
{

// A work item that proves whether it ever actually ran, for the
// cancel-while-queued row below: the regular CaptureWork/BlockingWork
// helpers above only prove a done callback fired, not that the work
// callback was skipped.
struct CancelTargetCtx
{
    const taz_dispatch_t *d = nullptr;
    const FakeConn *conn = nullptr;
    bool ran = false;
    int done_calls = 0;
    int done_closing = -1;
};

void CancelTargetWork(void *user)
{
    static_cast<CancelTargetCtx *>(user)->ran = true;
}

void CancelTargetDone(void *user, int closing)
{
    auto *w = static_cast<CancelTargetCtx *>(user);
    w->done_calls++;
    w->done_closing = closing;
}

} // namespace

TEST(Work, BeginShutdownCancelsQueuedWorkBeforeItEverRuns)
{
    // libuv's thread-pool work queue is strict FIFO: an item can only be
    // dequeued once every item ahead of it has been popped. Saturate the
    // pool with more permanently-blocked jobs than it could possibly have
    // worker threads for (this project configures at most 8), so every
    // worker is guaranteed busy on one of those and the item submitted
    // after them is still sitting on the queue - cancellable - no matter
    // how the pool threads happen to get scheduled.
    constexpr unsigned kSaturateCount = 16U;

    uv_loop_t loop;
    ASSERT_EQ(uv_loop_init(&loop), 0);

    FakeConn conn;
    taz_dispatch_t d;
    InitFakeDispatch(&d, &loop, &conn);
    d.active_streams[0] = 55U;
    d.active_count = 1U;

    std::vector<BlockingWorkCtx> saturators(kSaturateCount);
    for (auto &s : saturators)
    {
        s.d = &d;
        s.conn = &conn;
        ASSERT_EQ(uv_sem_init(&s.started, 0U), 0);
        ASSERT_EQ(uv_sem_init(&s.release, 0U), 0);
        // submit_step: never touches active_streams, so it cannot disturb
        // the target's stream slot below.
        ASSERT_EQ(taz_work_submit_step(&d, BlockingWork, BlockingDone, &s), 0);
    }

    CancelTargetCtx target;
    target.d = &d;
    target.conn = &conn;
    ASSERT_EQ(
        taz_work_submit(&d, 55U, CancelTargetWork, CancelTargetDone, &target),
        0);

    taz_work_begin_shutdown();
    EXPECT_EQ(taz_work_shutdown_requested(), 1);

    for (auto &s : saturators)
    {
        uv_sem_post(&s.release);
    }

    ASSERT_EQ(uv_run(&loop, UV_RUN_DEFAULT), 0);
    ASSERT_EQ(uv_loop_close(&loop), 0);

    EXPECT_FALSE(target.ran);
    EXPECT_EQ(target.done_calls, 1);
    EXPECT_EQ(target.done_closing, 1);
    EXPECT_EQ(d.active_count, 0U); // stream released despite the cancel.

    for (auto &s : saturators)
    {
        uv_sem_destroy(&s.started);
        uv_sem_destroy(&s.release);
    }

    taz_work_reset_for_tests();
}

TEST(Work, BeginShutdownNeverCancelsQueuedCleanupWork)
{
    // Same saturated pool as above, but the queued item is a cleanup step
    // (closing an fd, removing a temp file): it must still run.
    constexpr unsigned kSaturateCount = 16U;

    uv_loop_t loop;
    ASSERT_EQ(uv_loop_init(&loop), 0);

    FakeConn conn;
    taz_dispatch_t d;
    InitFakeDispatch(&d, &loop, &conn);

    std::vector<BlockingWorkCtx> saturators(kSaturateCount);
    for (auto &s : saturators)
    {
        s.d = &d;
        s.conn = &conn;
        ASSERT_EQ(uv_sem_init(&s.started, 0U), 0);
        ASSERT_EQ(uv_sem_init(&s.release, 0U), 0);
        ASSERT_EQ(taz_work_submit_step(&d, BlockingWork, BlockingDone, &s), 0);
    }

    CancelTargetCtx target;
    target.d = &d;
    target.conn = &conn;
    ASSERT_EQ(taz_work_submit_cleanup(&d, CancelTargetWork, CancelTargetDone,
                                      &target),
              0);

    taz_work_begin_shutdown();

    for (auto &s : saturators)
    {
        uv_sem_post(&s.release);
    }

    ASSERT_EQ(uv_run(&loop, UV_RUN_DEFAULT), 0);
    ASSERT_EQ(uv_loop_close(&loop), 0);

    EXPECT_TRUE(target.ran);
    EXPECT_EQ(target.done_calls, 1);
    EXPECT_EQ(target.done_closing, 1);
    EXPECT_EQ(conn.ref_count, conn.unref_count);

    for (auto &s : saturators)
    {
        uv_sem_destroy(&s.started);
        uv_sem_destroy(&s.release);
    }

    taz_work_reset_for_tests();
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

TEST(Work, SubmitStepDoesNotReleaseStream)
{
    taz_work_reset_for_tests();

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

    ASSERT_EQ(taz_work_submit_step(&d, CaptureWork, CaptureDone, &w), 0);
    EXPECT_EQ(conn.ref_count, 1);

    ASSERT_EQ(uv_run(&loop, UV_RUN_DEFAULT), 0);
    ASSERT_EQ(uv_loop_close(&loop), 0);

    EXPECT_EQ(w.done_calls, 1);
    EXPECT_EQ(w.done_closing, 0);
    // Stream should still be active after a step, not released.
    EXPECT_EQ(d.active_count, 1U);
    EXPECT_EQ(conn.ref_count, 1);
    EXPECT_EQ(conn.unref_count, 1);
}

TEST(Work, PlainSubmitStillReleasesStream)
{
    taz_work_reset_for_tests();

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

    ASSERT_EQ(taz_work_submit(&d, 42U, CaptureWork, CaptureDone, &w), 0);
    EXPECT_EQ(conn.ref_count, 1);

    ASSERT_EQ(uv_run(&loop, UV_RUN_DEFAULT), 0);
    ASSERT_EQ(uv_loop_close(&loop), 0);

    EXPECT_EQ(w.done_calls, 1);
    EXPECT_EQ(w.done_closing, 0);
    // Plain submit should release the stream.
    EXPECT_EQ(d.active_count, 0U);
    EXPECT_EQ(conn.ref_count, 1);
    EXPECT_EQ(conn.unref_count, 1);
}

namespace
{

struct ShutdownStepCtx
{
    uv_sem_t started{};
    uv_sem_t release{};
    int done_calls = 0;
    int done_closing = -1;
    const taz_dispatch_t *d = nullptr;
    const FakeConn *conn = nullptr;
    size_t active_count_at_done = 0U;
    int unref_count_at_done = -1;
};

void BlockingStepWork(void *user)
{
    auto *w = static_cast<ShutdownStepCtx *>(user);
    uv_sem_post(&w->started);
    uv_sem_wait(&w->release);
}

void ShutdownStepDone(void *user, int closing)
{
    auto *w = static_cast<ShutdownStepCtx *>(user);
    w->done_calls++;
    w->done_closing = closing;
    w->active_count_at_done = w->d->active_count;
    w->unref_count_at_done = w->conn->unref_count;
}

} // namespace

TEST(Work, BeginShutdownLetsRunningStepFinishThenLoopExitsOnItsOwn)
{
    uv_loop_t loop;
    ASSERT_EQ(uv_loop_init(&loop), 0);

    FakeConn conn;
    taz_dispatch_t d;
    InitFakeDispatch(&d, &loop, &conn);
    d.active_streams[0] = 9U;
    d.active_count = 1U;

    ShutdownStepCtx w;
    w.d = &d;
    w.conn = &conn;
    ASSERT_EQ(uv_sem_init(&w.started, 0U), 0);
    ASSERT_EQ(uv_sem_init(&w.release, 0U), 0);

    ASSERT_EQ(taz_work_shutdown_requested(), 0);
    ASSERT_EQ(taz_work_submit_step(&d, BlockingStepWork, ShutdownStepDone, &w),
              0);
    ASSERT_NE(uv_run(&loop, UV_RUN_NOWAIT), 0);
    uv_sem_wait(&w.started); // now actually running on a pool thread.

    taz_work_begin_shutdown();
    EXPECT_EQ(taz_work_shutdown_requested(), 1);
    // uv_cancel on work already running returns UV_EBUSY: it keeps going.
    EXPECT_EQ(w.done_calls, 0);

    uv_sem_post(&w.release);
    // No uv_stop anywhere: the loop exits on its own once the only
    // outstanding request (this one) completes.
    ASSERT_EQ(uv_run(&loop, UV_RUN_DEFAULT), 0);
    ASSERT_EQ(uv_loop_close(&loop), 0);

    EXPECT_EQ(w.done_calls, 1);
    // The step's done should see closing=1 due to shutdown.
    EXPECT_EQ(w.done_closing, 1);
    // Stream should still be active: steps don't release.
    EXPECT_EQ(w.active_count_at_done, 1U);
    EXPECT_EQ(w.unref_count_at_done, 0);

    uv_sem_destroy(&w.started);
    uv_sem_destroy(&w.release);

    taz_work_reset_for_tests();
}
