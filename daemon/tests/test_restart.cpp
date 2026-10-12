// Unit tests for handlers/restart.c: RESTART (0x0005) decode/clamp, the
// BUSY gate once shutdown has already begun, the delay timer arming only
// once the response has actually left the connection's write queue, an
// acknowledged restart surviving its requester's disconnect, the timer
// firing into taz_shutdown_request, CANCEL's refusal, several RESTARTs
// sharing the daemon's one timer, and taz_restart_close_pending.

#include <cstdint>
#include <functional>
#include <vector>

#include <gtest/gtest.h>
#include <pb_decode.h>
#include <pb_encode.h>
#include <uv.h>

#include "file_test_support.h"
#include "handlers/restart.h"
#include "taz/shutdown.h"
#include "taz/v1/advanced.pb.h"
#include "taz/v1/common.pb.h"
#include "taz/v1/daemon_control.pb.h"
#include "taz/work.h"

namespace
{

std::vector<uint8_t> RestartRequestBytes(uint32_t delay_ms)
{
    taz_v1_RestartRequest req = taz_v1_RestartRequest_init_zero;
    req.delay_ms = delay_ms;

    std::vector<uint8_t> buf(taz_v1_RestartRequest_size);
    pb_ostream_t ostream = pb_ostream_from_buffer(buf.data(), buf.size());
    EXPECT_TRUE(pb_encode(&ostream, taz_v1_RestartRequest_fields, &req));
    buf.resize(ostream.bytes_written);
    return buf;
}

bool DecodeRestartResponse(const std::vector<uint8_t> &frame,
                           taz_v1_RestartResponse *out)
{
    const taz_frame_header_t h = unpack_header(frame);
    if (h.type != static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_RESPONSE))
    {
        return false;
    }
    const std::vector<uint8_t> payload = frame_payload(frame);
    pb_istream_t stream =
        pb_istream_from_buffer(payload.data(), payload.size());
    return pb_decode(&stream, taz_v1_RestartResponse_fields, out);
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

// Same polling idiom as test_task_status.cpp's/test_shutdown.cpp's own
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

} // namespace

// ---------------------------------------------------------------------------
// Clamp (delay_ms=0, and the 99-vs-100 boundary pair) and reply.
// ---------------------------------------------------------------------------

TEST_F(FileHandlerTest, RestartZeroDelayClampsToFloor)
{
    DispatchRequestNoRun(taz_v1_Opcode_OPCODE_RESTART, RestartRequestBytes(0U),
                         700U);
    ASSERT_EQ(Frames().size(), 1U);

    taz_v1_RestartResponse resp = taz_v1_RestartResponse_init_zero;
    ASSERT_TRUE(DecodeRestartResponse(Frames()[0], &resp));
    EXPECT_TRUE(resp.acknowledged);
    EXPECT_EQ(resp.effective_delay_ms, 100U);

    // The write queue was already empty: the timer is armed and the
    // request no longer holds the stream or the connection.
    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());

    taz_restart_close_pending();
    RunLoop();
    EXPECT_EQ(taz_shutdown_mode(), TAZ_SHUTDOWN_NONE);
}

TEST_F(FileHandlerTest, RestartDelayBoundaryPairAndAboveClampToExact)
{
    DispatchRequestNoRun(taz_v1_Opcode_OPCODE_RESTART, RestartRequestBytes(99U),
                         701U);
    DispatchRequestNoRun(taz_v1_Opcode_OPCODE_RESTART,
                         RestartRequestBytes(100U), 702U);
    DispatchRequestNoRun(taz_v1_Opcode_OPCODE_RESTART,
                         RestartRequestBytes(500U), 703U);
    ASSERT_EQ(Frames().size(), 3U);

    taz_v1_RestartResponse below = taz_v1_RestartResponse_init_zero;
    taz_v1_RestartResponse at = taz_v1_RestartResponse_init_zero;
    taz_v1_RestartResponse above = taz_v1_RestartResponse_init_zero;
    ASSERT_TRUE(DecodeRestartResponse(Frames()[0], &below));
    ASSERT_TRUE(DecodeRestartResponse(Frames()[1], &at));
    ASSERT_TRUE(DecodeRestartResponse(Frames()[2], &above));
    EXPECT_EQ(below.effective_delay_ms, 100U);
    EXPECT_EQ(at.effective_delay_ms, 100U);
    EXPECT_EQ(above.effective_delay_ms, 500U);

    taz_restart_close_pending();
    RunLoop();
    EXPECT_EQ(taz_shutdown_mode(), TAZ_SHUTDOWN_NONE);
    EXPECT_EQ(RefCount(), UnrefCount());
}

// ---------------------------------------------------------------------------
// An undecodable payload.
// ---------------------------------------------------------------------------

TEST_F(FileHandlerTest, RestartUndecodablePayloadIsInvalidRequest)
{
    const std::vector<uint8_t> garbage{0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU};
    DispatchRequestNoRun(taz_v1_Opcode_OPCODE_RESTART, garbage, 710U);

    ASSERT_EQ(Frames().size(), 1U);
    taz_v1_ErrorInfo info = taz_v1_ErrorInfo_init_zero;
    ASSERT_TRUE(DecodeErrorInfo(Frames()[0], &info));
    EXPECT_EQ(info.code, taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST);
    EXPECT_EQ(ActiveStreamCount(), 0U);
}

// ---------------------------------------------------------------------------
// Shutdown already in progress.
// ---------------------------------------------------------------------------

TEST_F(FileHandlerTest, RestartBusyOnceShutdownHasBegun)
{
    RequestShutdown();

    DispatchRequestNoRun(taz_v1_Opcode_OPCODE_RESTART,
                         RestartRequestBytes(500U), 720U);
    ASSERT_EQ(Frames().size(), 1U);
    taz_v1_ErrorInfo info = taz_v1_ErrorInfo_init_zero;
    ASSERT_TRUE(DecodeErrorInfo(Frames()[0], &info));
    EXPECT_EQ(info.code, taz_v1_ErrorCode_ERROR_CODE_BUSY);
    EXPECT_EQ(ActiveStreamCount(), 0U);

    taz_work_reset_for_tests();
}

// ---------------------------------------------------------------------------
// The timer arms only once the write queue has actually drained.
// ---------------------------------------------------------------------------

TEST_F(FileHandlerTest, RestartTimerArmsOnlyOnceWriteQueueDrains)
{
    SetQueueSize(1U);
    DispatchRequestNoRun(taz_v1_Opcode_OPCODE_RESTART,
                         RestartRequestBytes(100U), 730U);
    ASSERT_EQ(Frames().size(), 1U);

    // The write queue is still nonzero: the timer was never armed, so the
    // loop has nothing pending and returns immediately regardless of how
    // long 100 ms (the requested delay) would otherwise take.
    RunLoop();
    EXPECT_EQ(taz_shutdown_mode(), TAZ_SHUTDOWN_NONE);
    EXPECT_EQ(ActiveStreamCount(), 1U);

    // on_writable fires while the queue is still nonzero: still a no-op.
    NotifyWritable();
    RunLoop();
    EXPECT_EQ(taz_shutdown_mode(), TAZ_SHUTDOWN_NONE);
    EXPECT_EQ(ActiveStreamCount(), 1U);

    // The queue drains; the next on_writable arms it exactly once and
    // hands the request over to the daemon (stream ended, reference gone).
    SetQueueSize(0U);
    NotifyWritable();
    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());
    RunLoopUntil(
        Loop(), [&]() { return taz_shutdown_mode() != TAZ_SHUTDOWN_NONE; },
        1000);
    EXPECT_EQ(taz_shutdown_mode(), TAZ_SHUTDOWN_RESTART);

    // The timer's own uv_close callback runs on the next loop turn.
    RunLoop();

    taz_work_reset_for_tests();
    taz_shutdown_reset_for_tests();
}

// on_writable may end the RESTART stream from inside
// taz_dispatch_notify_writable's own iteration; a stream swapped into its
// slot must still be notified.
TEST_F(FileHandlerTest, RestartEndingItsStreamInOnWritableSkipsNoOtherStream)
{
    SetQueueSize(1U);
    DispatchRequestNoRun(taz_v1_Opcode_OPCODE_RESTART,
                         RestartRequestBytes(60000U), 731U);
    DispatchRequestNoRun(taz_v1_Opcode_OPCODE_RESTART,
                         RestartRequestBytes(60000U), 732U);
    ASSERT_EQ(ActiveStreamCount(), 2U);

    SetQueueSize(0U);
    NotifyWritable();
    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());

    taz_restart_close_pending();
    RunLoop();
    EXPECT_EQ(taz_shutdown_mode(), TAZ_SHUTDOWN_NONE);
}

// ---------------------------------------------------------------------------
// The requester disconnects before the timer fires: the restart still
// happens.
// ---------------------------------------------------------------------------

TEST_F(FileHandlerTest, RestartSurvivesItsConnectionClosingAfterTheAck)
{
    DispatchRequestNoRun(taz_v1_Opcode_OPCODE_RESTART,
                         RestartRequestBytes(100U), 740U);
    ASSERT_EQ(Frames().size(), 1U);
    EXPECT_EQ(ActiveStreamCount(), 0U);

    CloseConnectionAndCancelAll();

    RunLoopUntil(
        Loop(), [&]() { return taz_shutdown_mode() != TAZ_SHUTDOWN_NONE; },
        1000);
    EXPECT_EQ(taz_shutdown_mode(), TAZ_SHUTDOWN_RESTART);
    RunLoop();
    EXPECT_EQ(RefCount(), UnrefCount());

    taz_work_reset_for_tests();
    taz_shutdown_reset_for_tests();
}

TEST_F(FileHandlerTest, RestartSurvivesItsConnectionClosingBeforeTheAckDrains)
{
    // The response never finishes writing: abort arms the timer itself.
    SetQueueSize(1U);
    DispatchRequestNoRun(taz_v1_Opcode_OPCODE_RESTART,
                         RestartRequestBytes(100U), 741U);
    ASSERT_EQ(Frames().size(), 1U);
    EXPECT_EQ(ActiveStreamCount(), 1U);

    CloseConnectionAndCancelAll();
    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());

    RunLoopUntil(
        Loop(), [&]() { return taz_shutdown_mode() != TAZ_SHUTDOWN_NONE; },
        1000);
    EXPECT_EQ(taz_shutdown_mode(), TAZ_SHUTDOWN_RESTART);
    RunLoop();

    taz_work_reset_for_tests();
    taz_shutdown_reset_for_tests();
}

TEST_F(FileHandlerTest, RestartAbortAfterShutdownBeganClosesItsTimer)
{
    // The connection closes as part of a shutdown already under way (a
    // STOP won): the not-yet-armed restart is closed, never armed.
    SetQueueSize(1U);
    DispatchRequestNoRun(taz_v1_Opcode_OPCODE_RESTART,
                         RestartRequestBytes(60000U), 742U);
    ASSERT_EQ(ActiveStreamCount(), 1U);

    RequestShutdown();
    CloseConnectionAndCancelAll();
    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());

    // The walk closes the (never armed) daemon timer; returns at once.
    taz_restart_close_pending();
    RunLoop();
    EXPECT_EQ(taz_shutdown_mode(), TAZ_SHUTDOWN_NONE);

    taz_work_reset_for_tests();
}

// ---------------------------------------------------------------------------
// The timer fires: taz_shutdown_request(TAZ_SHUTDOWN_RESTART).
// ---------------------------------------------------------------------------

TEST_F(FileHandlerTest, RestartTimerFiresThenRequestsShutdown)
{
    DispatchRequestNoRun(taz_v1_Opcode_OPCODE_RESTART,
                         RestartRequestBytes(100U), 750U);
    ASSERT_EQ(Frames().size(), 1U);

    RunLoopUntil(
        Loop(), [&]() { return taz_shutdown_mode() != TAZ_SHUTDOWN_NONE; },
        1000);
    EXPECT_EQ(taz_shutdown_mode(), TAZ_SHUTDOWN_RESTART);

    // The timer fires inside UV_RUN_ONCE's own end-of-iteration recheck,
    // after that same iteration's closing-handle phase already ran, so its
    // uv_close callback only runs on the next loop turn.
    RunLoop();
    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());

    // Neither a later connection close nor the walk's
    // taz_restart_close_pending has anything left to close.
    CloseConnectionAndCancelAll();
    taz_restart_close_pending();
    RunLoop();
    EXPECT_EQ(RefCount(), UnrefCount());

    taz_work_reset_for_tests();
    taz_shutdown_reset_for_tests();
}

// ---------------------------------------------------------------------------
// CANCEL on the RESTART stream is refused.
// ---------------------------------------------------------------------------

TEST_F(FileHandlerTest, RestartCancelIsRefused)
{
    // Keep the stream open (response not yet written) so CANCEL reaches
    // the RESTART stream's own cancel op rather than finding no stream.
    SetQueueSize(1U);
    DispatchRequestNoRun(taz_v1_Opcode_OPCODE_RESTART,
                         RestartRequestBytes(60000U), 760U);
    ASSERT_EQ(Frames().size(), 1U);

    DispatchRequestNoRun(taz_v1_Opcode_OPCODE_CANCEL, CancelRequestBytes(760U),
                         761U);
    ASSERT_EQ(Frames().size(), 2U);
    taz_v1_CancelResponse resp = taz_v1_CancelResponse_init_zero;
    ASSERT_TRUE(DecodeCancelResponse(Frames()[1], &resp));
    EXPECT_FALSE(resp.cancelled);
    EXPECT_EQ(ActiveStreamCount(), 1U); // the RESTART stream is still open

    taz_restart_close_pending();
    RunLoop();
    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());
    EXPECT_EQ(taz_shutdown_mode(), TAZ_SHUTDOWN_NONE);
}

// ---------------------------------------------------------------------------
// Several RESTARTs acknowledged before any fires: one restart, at the
// earliest deadline - a shorter delay moves it earlier, a longer one never
// postpones it.
// ---------------------------------------------------------------------------

TEST_F(FileHandlerTest, RestartSeveralBeforeAnyFiresRestartsOnceAtTheEarliest)
{
    DispatchRequestNoRun(taz_v1_Opcode_OPCODE_RESTART,
                         RestartRequestBytes(60000U), 770U);
    DispatchRequestNoRun(taz_v1_Opcode_OPCODE_RESTART,
                         RestartRequestBytes(100U), 771U);
    DispatchRequestNoRun(taz_v1_Opcode_OPCODE_RESTART,
                         RestartRequestBytes(60000U), 772U);
    ASSERT_EQ(Frames().size(), 3U);

    for (const auto &frame : Frames())
    {
        taz_v1_RestartResponse resp = taz_v1_RestartResponse_init_zero;
        ASSERT_TRUE(DecodeRestartResponse(frame, &resp));
        EXPECT_TRUE(resp.acknowledged);
    }

    RunLoopUntil(
        Loop(), [&]() { return taz_shutdown_mode() != TAZ_SHUTDOWN_NONE; },
        1000);
    EXPECT_EQ(taz_shutdown_mode(), TAZ_SHUTDOWN_RESTART);

    // One timer for all three, already closed by its own firing: nothing
    // is left to wait 60 s for.
    RunLoop();
    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());

    taz_work_reset_for_tests();
    taz_shutdown_reset_for_tests();
}

// ---------------------------------------------------------------------------
// taz_restart_close_pending: the walk closes every unfired restart.
// ---------------------------------------------------------------------------

TEST_F(FileHandlerTest, RestartClosePendingClosesEveryUnfiredRequest)
{
    DispatchRequestNoRun(taz_v1_Opcode_OPCODE_RESTART,
                         RestartRequestBytes(60000U), 780U);
    SetQueueSize(1U);
    DispatchRequestNoRun(taz_v1_Opcode_OPCODE_RESTART,
                         RestartRequestBytes(60000U), 781U);
    ASSERT_EQ(ActiveStreamCount(), 1U); // the second one is still attached

    taz_restart_close_pending();
    taz_restart_close_pending(); // idempotent
    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());

    // Returns at once: neither 60 s timer is left open.
    RunLoop();
    EXPECT_EQ(taz_shutdown_mode(), TAZ_SHUTDOWN_NONE);
}
