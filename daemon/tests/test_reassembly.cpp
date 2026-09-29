// Tests for the two-phase frame reassembly state machine.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

#include "taz/frame.h"
#include "taz/reassembly.h"
#include "taz/v1/common.pb.h"

namespace
{

struct FrameEvent
{
    taz_frame_header_t header{};
    std::vector<uint8_t> payload;
    taz_frame_verdict_t verdict{TAZ_FRAME_OK};
};

struct Ctx
{
    std::vector<FrameEvent> events;
};

void on_frame(const taz_frame_header_t *header, const uint8_t *payload,
              taz_frame_verdict_t verdict, void *ctx_ptr)
{
    auto *ctx = static_cast<Ctx *>(ctx_ptr);
    FrameEvent ev;
    ev.header = *header;
    ev.verdict = verdict;
    if (payload != nullptr && header->length > 0U)
    {
        ev.payload.assign(payload, payload + header->length);
    }
    ctx->events.push_back(ev);
}

// Pack a complete frame (header + optional payload) into buf. Returns total
// byte count written.
size_t BuildFrame(uint8_t type, uint16_t opcode, uint32_t stream_id,
                  const uint8_t *payload, uint32_t payload_len, uint8_t *buf)
{
    taz_frame_header_t h{};
    h.type = type;
    h.flags = 0U;
    h.opcode = opcode;
    h.length = payload_len;
    h.stream_id = stream_id;
    taz_frame_pack_header(&h, buf);
    if (payload != nullptr && payload_len > 0U)
    {
        std::memcpy(buf + TAZ_FRAME_HEADER_SIZE, payload, payload_len);
    }
    return static_cast<size_t>(TAZ_FRAME_HEADER_SIZE) + payload_len;
}

// ---------------------------------------------------------------------------
// Partial-header accumulation
// ---------------------------------------------------------------------------

TEST(Reassembly, PartialHeaderAccumulatesOneByte)
{
    // A PING (length=0) fed one byte at a time: callback fires on exactly the
    // 12th byte, not before.
    taz_reassembly_state_t state{};
    taz_reassembly_init(&state);
    Ctx ctx;

    uint8_t buf[TAZ_FRAME_HEADER_SIZE];
    BuildFrame(taz_v1_FrameType_FRAME_TYPE_PING, 0U, 7U, nullptr, 0U, buf);

    for (int i = 0; i < TAZ_FRAME_HEADER_SIZE - 1; ++i)
    {
        taz_reassembly_feed(&state, &buf[i], 1U, on_frame, &ctx);
        EXPECT_TRUE(ctx.events.empty()) << "premature callback at byte " << i;
    }
    taz_reassembly_feed(&state, &buf[TAZ_FRAME_HEADER_SIZE - 1], 1U, on_frame,
                        &ctx);
    ASSERT_EQ(ctx.events.size(), 1U);
    EXPECT_EQ(ctx.events[0].verdict, TAZ_FRAME_OK);
    EXPECT_EQ(ctx.events[0].header.type,
              static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_PING));
    EXPECT_EQ(ctx.events[0].header.stream_id, 7U);
}

TEST(Reassembly, PartialHeaderSplitInHalves)
{
    // Header split 6 + 6: callback fires only after the second half.
    taz_reassembly_state_t state{};
    taz_reassembly_init(&state);
    Ctx ctx;

    uint8_t buf[TAZ_FRAME_HEADER_SIZE];
    BuildFrame(taz_v1_FrameType_FRAME_TYPE_PONG, 0U, 5U, nullptr, 0U, buf);

    taz_reassembly_feed(&state, buf, 6U, on_frame, &ctx);
    EXPECT_TRUE(ctx.events.empty());
    taz_reassembly_feed(&state, buf + 6, 6U, on_frame, &ctx);
    ASSERT_EQ(ctx.events.size(), 1U);
    EXPECT_EQ(ctx.events[0].verdict, TAZ_FRAME_OK);
    EXPECT_EQ(ctx.events[0].header.type,
              static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_PONG));
}

// ---------------------------------------------------------------------------
// Partial-payload accumulation
// ---------------------------------------------------------------------------

TEST(Reassembly, PartialPayloadAccumulates)
{
    // REQUEST frame with a 10-byte payload; feed header whole, then payload in
    // two unequal chunks.
    taz_reassembly_state_t state{};
    taz_reassembly_init(&state);
    Ctx ctx;

    const uint8_t payload[] = {0x01, 0x02, 0x03, 0x04, 0x05,
                               0x06, 0x07, 0x08, 0x09, 0x0AU};
    uint8_t buf[TAZ_FRAME_HEADER_SIZE + sizeof(payload)];
    BuildFrame(taz_v1_FrameType_FRAME_TYPE_REQUEST, 0x0001U, 42U, payload,
               static_cast<uint32_t>(sizeof(payload)), buf);

    taz_reassembly_feed(&state, buf, TAZ_FRAME_HEADER_SIZE, on_frame, &ctx);
    EXPECT_TRUE(ctx.events.empty());
    EXPECT_EQ(state.phase, TAZ_REASSEMBLY_PHASE_PAYLOAD);

    // 3 bytes, then remaining 7
    taz_reassembly_feed(&state, buf + TAZ_FRAME_HEADER_SIZE, 3U, on_frame,
                        &ctx);
    EXPECT_TRUE(ctx.events.empty());
    taz_reassembly_feed(&state, buf + TAZ_FRAME_HEADER_SIZE + 3U, 7U, on_frame,
                        &ctx);

    ASSERT_EQ(ctx.events.size(), 1U);
    EXPECT_EQ(ctx.events[0].verdict, TAZ_FRAME_OK);
    EXPECT_EQ(ctx.events[0].header.stream_id, 42U);
    const std::vector<uint8_t> expected(payload, payload + sizeof(payload));
    EXPECT_EQ(ctx.events[0].payload, expected);
}

// ---------------------------------------------------------------------------
// Coalesced frames (multiple frames in one read)
// ---------------------------------------------------------------------------

TEST(Reassembly, TwoZeroLengthFramesCoalesced)
{
    // Two PING frames concatenated in a single read; both must be delivered in
    // order.
    taz_reassembly_state_t state{};
    taz_reassembly_init(&state);
    Ctx ctx;

    uint8_t buf[2U * TAZ_FRAME_HEADER_SIZE];
    BuildFrame(taz_v1_FrameType_FRAME_TYPE_PING, 0U, 1U, nullptr, 0U, buf);
    BuildFrame(taz_v1_FrameType_FRAME_TYPE_PING, 0U, 2U, nullptr, 0U,
               buf + TAZ_FRAME_HEADER_SIZE);

    taz_reassembly_feed(&state, buf, sizeof(buf), on_frame, &ctx);
    ASSERT_EQ(ctx.events.size(), 2U);
    EXPECT_EQ(ctx.events[0].verdict, TAZ_FRAME_OK);
    EXPECT_EQ(ctx.events[0].header.stream_id, 1U);
    EXPECT_EQ(ctx.events[1].verdict, TAZ_FRAME_OK);
    EXPECT_EQ(ctx.events[1].header.stream_id, 2U);
}

TEST(Reassembly, TwoFramesWithPayloadsCoalesced)
{
    // REQUEST followed by RESPONSE, both with payloads, coalesced.
    taz_reassembly_state_t state{};
    taz_reassembly_init(&state);
    Ctx ctx;

    const uint8_t pay1[] = {0xAAU, 0xBBU};
    const uint8_t pay2[] = {0xCCU, 0xDDU, 0xEEU};

    uint8_t buf[(static_cast<size_t>(2U) * TAZ_FRAME_HEADER_SIZE) +
                sizeof(pay1) + sizeof(pay2)];
    const size_t n1 =
        BuildFrame(taz_v1_FrameType_FRAME_TYPE_REQUEST, 0x0001U, 10U, pay1,
                   static_cast<uint32_t>(sizeof(pay1)), buf);
    BuildFrame(taz_v1_FrameType_FRAME_TYPE_RESPONSE, 0x0001U, 20U, pay2,
               static_cast<uint32_t>(sizeof(pay2)), buf + n1);

    taz_reassembly_feed(&state, buf, sizeof(buf), on_frame, &ctx);
    ASSERT_EQ(ctx.events.size(), 2U);
    EXPECT_EQ(ctx.events[0].verdict, TAZ_FRAME_OK);
    EXPECT_EQ(ctx.events[0].header.stream_id, 10U);
    EXPECT_EQ(ctx.events[0].payload,
              std::vector<uint8_t>(pay1, pay1 + sizeof(pay1)));
    EXPECT_EQ(ctx.events[1].verdict, TAZ_FRAME_OK);
    EXPECT_EQ(ctx.events[1].header.stream_id, 20U);
    EXPECT_EQ(ctx.events[1].payload,
              std::vector<uint8_t>(pay2, pay2 + sizeof(pay2)));
}

TEST(Reassembly, ThreeFramesCoalesced)
{
    // Three PING frames in one buffer; all three must be delivered.
    taz_reassembly_state_t state{};
    taz_reassembly_init(&state);
    Ctx ctx;

    uint8_t buf[3U * TAZ_FRAME_HEADER_SIZE];
    BuildFrame(taz_v1_FrameType_FRAME_TYPE_PING, 0U, 1U, nullptr, 0U, buf);
    BuildFrame(taz_v1_FrameType_FRAME_TYPE_PING, 0U, 2U, nullptr, 0U,
               buf + TAZ_FRAME_HEADER_SIZE);
    BuildFrame(taz_v1_FrameType_FRAME_TYPE_PING, 0U, 3U, nullptr, 0U,
               buf + (static_cast<size_t>(2U) * TAZ_FRAME_HEADER_SIZE));

    taz_reassembly_feed(&state, buf, sizeof(buf), on_frame, &ctx);
    ASSERT_EQ(ctx.events.size(), 3U);
    for (size_t i = 0U; i < 3U; ++i)
    {
        EXPECT_EQ(ctx.events[i].verdict, TAZ_FRAME_OK) << "frame " << i;
        EXPECT_EQ(ctx.events[i].header.stream_id, static_cast<uint32_t>(i + 1U))
            << "frame " << i;
    }
}

// ---------------------------------------------------------------------------
// Unknown-type frames (§10.1): skip payload, report UNKNOWN_TYPE, continue
// ---------------------------------------------------------------------------

TEST(Reassembly, UnknownTypeZeroPayloadThenPing)
{
    // Unknown-type frame with zero-length payload immediately delivers
    // UNKNOWN_TYPE; the subsequent PING delivers OK.
    taz_reassembly_state_t state{};
    taz_reassembly_init(&state);
    Ctx ctx;

    uint8_t buf[2U * TAZ_FRAME_HEADER_SIZE];
    BuildFrame(taz_v1_FrameType_FRAME_TYPE_UNSPECIFIED, 0U, 0U, nullptr, 0U,
               buf);
    BuildFrame(taz_v1_FrameType_FRAME_TYPE_PING, 0U, 77U, nullptr, 0U,
               buf + TAZ_FRAME_HEADER_SIZE);

    taz_reassembly_feed(&state, buf, sizeof(buf), on_frame, &ctx);
    ASSERT_EQ(ctx.events.size(), 2U);
    EXPECT_EQ(ctx.events[0].verdict, TAZ_FRAME_UNKNOWN_TYPE);
    EXPECT_EQ(ctx.events[0].header.type,
              static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_UNSPECIFIED));
    EXPECT_EQ(ctx.events[1].verdict, TAZ_FRAME_OK);
    EXPECT_EQ(ctx.events[1].header.type,
              static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_PING));
    EXPECT_EQ(ctx.events[1].header.stream_id, 77U);
}

TEST(Reassembly, UnknownTypeWithPayloadThenPing)
{
    // Unknown-type frame (type 0x00) with a 4-byte payload: skip consumes
    // exactly 4 bytes, then the following PING is delivered OK.
    taz_reassembly_state_t state{};
    taz_reassembly_init(&state);
    Ctx ctx;

    const uint8_t unk_payload[] = {0x01U, 0x02U, 0x03U, 0x04U};
    uint8_t buf[(static_cast<size_t>(2U) * TAZ_FRAME_HEADER_SIZE) +
                sizeof(unk_payload)];
    const size_t n1 =
        BuildFrame(taz_v1_FrameType_FRAME_TYPE_UNSPECIFIED, 0U, 0U, unk_payload,
                   static_cast<uint32_t>(sizeof(unk_payload)), buf);
    BuildFrame(taz_v1_FrameType_FRAME_TYPE_PING, 0U, 99U, nullptr, 0U,
               buf + n1);

    taz_reassembly_feed(&state, buf, sizeof(buf), on_frame, &ctx);
    ASSERT_EQ(ctx.events.size(), 2U);
    EXPECT_EQ(ctx.events[0].verdict, TAZ_FRAME_UNKNOWN_TYPE);
    EXPECT_EQ(ctx.events[1].verdict, TAZ_FRAME_OK);
    EXPECT_EQ(ctx.events[1].header.type,
              static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_PING));
    EXPECT_EQ(ctx.events[1].header.stream_id, 99U);
}

TEST(Reassembly, UnknownTypeAboveMaxWithPayloadThenPing)
{
    // Type _taz_v1_FrameType_MAX+1 (above all assigned values) with a 6-byte
    // payload; after the skip, PING is delivered OK.
    taz_reassembly_state_t state{};
    taz_reassembly_init(&state);
    Ctx ctx;

    const auto unk_type = static_cast<uint8_t>(_taz_v1_FrameType_MAX + 1);
    const uint8_t unk_payload[] = {0xDEU, 0xADU, 0xBEU, 0xEFU, 0xCAU, 0xFEU};
    uint8_t buf[(static_cast<size_t>(2U) * TAZ_FRAME_HEADER_SIZE) +
                sizeof(unk_payload)];
    const size_t n1 =
        BuildFrame(unk_type, 0U, 0U, unk_payload,
                   static_cast<uint32_t>(sizeof(unk_payload)), buf);
    BuildFrame(taz_v1_FrameType_FRAME_TYPE_PING, 0U, 55U, nullptr, 0U,
               buf + n1);

    taz_reassembly_feed(&state, buf, sizeof(buf), on_frame, &ctx);
    ASSERT_EQ(ctx.events.size(), 2U);
    EXPECT_EQ(ctx.events[0].verdict, TAZ_FRAME_UNKNOWN_TYPE);
    EXPECT_EQ(ctx.events[0].header.type, unk_type);
    EXPECT_EQ(ctx.events[1].verdict, TAZ_FRAME_OK);
    EXPECT_EQ(ctx.events[1].header.stream_id, 55U);
}

TEST(Reassembly, UnknownTypeSkipFedInChunks)
{
    // Unknown-type frame with a 6-byte payload fed in 3 pairs; UNKNOWN_TYPE
    // fires only once, after all 6 payload bytes have been consumed.
    taz_reassembly_state_t state{};
    taz_reassembly_init(&state);
    Ctx ctx;

    const uint8_t unk_payload[] = {0xAAU, 0xBBU, 0xCCU, 0xDDU, 0xEEU, 0xFFU};
    uint8_t buf[TAZ_FRAME_HEADER_SIZE + sizeof(unk_payload)];
    BuildFrame(taz_v1_FrameType_FRAME_TYPE_UNSPECIFIED, 0U, 0U, unk_payload,
               static_cast<uint32_t>(sizeof(unk_payload)), buf);

    taz_reassembly_feed(&state, buf, TAZ_FRAME_HEADER_SIZE, on_frame, &ctx);
    EXPECT_TRUE(ctx.events.empty());
    EXPECT_EQ(state.phase, TAZ_REASSEMBLY_PHASE_SKIP);

    taz_reassembly_feed(&state, buf + TAZ_FRAME_HEADER_SIZE, 2U, on_frame,
                        &ctx);
    EXPECT_TRUE(ctx.events.empty());
    taz_reassembly_feed(&state, buf + TAZ_FRAME_HEADER_SIZE + 2U, 2U, on_frame,
                        &ctx);
    EXPECT_TRUE(ctx.events.empty());
    taz_reassembly_feed(&state, buf + TAZ_FRAME_HEADER_SIZE + 4U, 2U, on_frame,
                        &ctx);

    ASSERT_EQ(ctx.events.size(), 1U);
    EXPECT_EQ(ctx.events[0].verdict, TAZ_FRAME_UNKNOWN_TYPE);
    EXPECT_EQ(state.phase, TAZ_REASSEMBLY_PHASE_HEADER);
}

// ---------------------------------------------------------------------------
// Oversized frames (§10.1a): report OVERSIZED and enter the DONE state
// ---------------------------------------------------------------------------

TEST(Reassembly, OversizedPingEntersDone)
{
    // PING with length=1 is oversized; OVERSIZED fires, state enters DONE, and
    // all subsequent calls are no-ops.
    taz_reassembly_state_t state{};
    taz_reassembly_init(&state);
    Ctx ctx;

    uint8_t buf[TAZ_FRAME_HEADER_SIZE];
    BuildFrame(taz_v1_FrameType_FRAME_TYPE_PING, 0U, 0U, nullptr, 1U, buf);

    taz_reassembly_feed(&state, buf, sizeof(buf), on_frame, &ctx);
    ASSERT_EQ(ctx.events.size(), 1U);
    EXPECT_EQ(ctx.events[0].verdict, TAZ_FRAME_OVERSIZED);
    EXPECT_EQ(state.phase, TAZ_REASSEMBLY_PHASE_DONE);

    // Further input is silently discarded.
    ctx.events.clear();
    uint8_t extra[TAZ_FRAME_HEADER_SIZE]{};
    taz_reassembly_feed(&state, extra, sizeof(extra), on_frame, &ctx);
    EXPECT_TRUE(ctx.events.empty());
}

TEST(Reassembly, OversizedRequestEntersDone)
{
    // REQUEST with length = TAZ_FRAME_MAX_PAYLOAD + 1 is oversized.
    taz_reassembly_state_t state{};
    taz_reassembly_init(&state);
    Ctx ctx;

    uint8_t buf[TAZ_FRAME_HEADER_SIZE];
    BuildFrame(taz_v1_FrameType_FRAME_TYPE_REQUEST, 0x0001U, 0U, nullptr,
               static_cast<uint32_t>(TAZ_FRAME_MAX_PAYLOAD) + 1U, buf);

    taz_reassembly_feed(&state, buf, sizeof(buf), on_frame, &ctx);
    ASSERT_EQ(ctx.events.size(), 1U);
    EXPECT_EQ(ctx.events[0].verdict, TAZ_FRAME_OVERSIZED);
    EXPECT_EQ(state.phase, TAZ_REASSEMBLY_PHASE_DONE);
}

TEST(Reassembly, OversizedHeaderFedPiecemeal)
{
    // Oversized PING header fed one byte at a time: OVERSIZED fires exactly
    // once after the 12th byte.
    taz_reassembly_state_t state{};
    taz_reassembly_init(&state);
    Ctx ctx;

    uint8_t buf[TAZ_FRAME_HEADER_SIZE];
    BuildFrame(taz_v1_FrameType_FRAME_TYPE_PING, 0U, 0U, nullptr, 1U, buf);

    for (int i = 0; i < TAZ_FRAME_HEADER_SIZE - 1; ++i)
    {
        taz_reassembly_feed(&state, &buf[i], 1U, on_frame, &ctx);
        EXPECT_TRUE(ctx.events.empty()) << "premature callback at byte " << i;
        EXPECT_EQ(state.phase, TAZ_REASSEMBLY_PHASE_HEADER);
    }
    taz_reassembly_feed(&state, &buf[TAZ_FRAME_HEADER_SIZE - 1], 1U, on_frame,
                        &ctx);
    ASSERT_EQ(ctx.events.size(), 1U);
    EXPECT_EQ(ctx.events[0].verdict, TAZ_FRAME_OVERSIZED);
    EXPECT_EQ(state.phase, TAZ_REASSEMBLY_PHASE_DONE);
}

TEST(Reassembly, OversizedUnknownTypeEntersDone)
{
    // Unknown type with length > 64 KiB is also oversized (§10.1).
    taz_reassembly_state_t state{};
    taz_reassembly_init(&state);
    Ctx ctx;

    uint8_t buf[TAZ_FRAME_HEADER_SIZE];
    BuildFrame(taz_v1_FrameType_FRAME_TYPE_UNSPECIFIED, 0U, 0U, nullptr,
               static_cast<uint32_t>(TAZ_FRAME_MAX_PAYLOAD) + 1U, buf);

    taz_reassembly_feed(&state, buf, sizeof(buf), on_frame, &ctx);
    ASSERT_EQ(ctx.events.size(), 1U);
    EXPECT_EQ(ctx.events[0].verdict, TAZ_FRAME_OVERSIZED);
    EXPECT_EQ(state.phase, TAZ_REASSEMBLY_PHASE_DONE);
}

// ---------------------------------------------------------------------------
// State-machine reset (sanity check after normal completion)
// ---------------------------------------------------------------------------

TEST(Reassembly, ReturnsToHeaderPhaseAfterPayload)
{
    // After a complete REQUEST frame the machine returns to HEADER phase, ready
    // for the next frame.
    taz_reassembly_state_t state{};
    taz_reassembly_init(&state);
    Ctx ctx;

    const uint8_t payload[] = {0x42U};
    uint8_t buf[TAZ_FRAME_HEADER_SIZE + sizeof(payload)];
    BuildFrame(taz_v1_FrameType_FRAME_TYPE_REQUEST, 0x0001U, 1U, payload,
               static_cast<uint32_t>(sizeof(payload)), buf);

    taz_reassembly_feed(&state, buf, sizeof(buf), on_frame, &ctx);
    ASSERT_EQ(ctx.events.size(), 1U);
    EXPECT_EQ(state.phase, TAZ_REASSEMBLY_PHASE_HEADER);
    EXPECT_EQ(state.cursor, 0U);
}

} // namespace
