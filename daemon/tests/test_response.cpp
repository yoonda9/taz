// Unit tests for taz_response_send (response splitter).

#include <cstdint>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>
#include <pb_decode.h>
#include <pb_encode.h>

// MSVC's <stdio.h> defines stdout and stderr as macros, which rewrite the
// CommandExecResponse fields of the same names. Nothing here uses the streams.
#undef stdout
#undef stderr

#include "taz/frame.h"
#include "taz/response.h"
#include "taz/v1/command.pb.h"
#include "taz/v1/common.pb.h"
#include "taz/v1/daemon_control.pb.h"
#include "taz/v1/file.pb.h"

namespace
{

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------

struct WriteCtx
{
    std::vector<std::vector<uint8_t>> frames;
};

void capture_write(const uint8_t *data, size_t len, void *ctx)
{
    auto *wctx = static_cast<WriteCtx *>(ctx);
    wctx->frames.emplace_back(data, data + len);
}

// Unpack the frame header from a captured frame.
taz_frame_header_t unpack_header(const std::vector<uint8_t> &frame)
{
    taz_frame_header_t h{};
    if (frame.size() >= static_cast<size_t>(TAZ_FRAME_HEADER_SIZE))
    {
        taz_frame_unpack_header(frame.data(), &h);
    }
    return h;
}

// Decode a CommandExecResponse from a captured frame's payload.
bool decode_exec_response(const std::vector<uint8_t> &frame,
                          taz_v1_CommandExecResponse *out)
{
    if (frame.size() <= static_cast<size_t>(TAZ_FRAME_HEADER_SIZE))
    {
        return false;
    }
    pb_istream_t stream = pb_istream_from_buffer(
        frame.data() + TAZ_FRAME_HEADER_SIZE,
        frame.size() - static_cast<size_t>(TAZ_FRAME_HEADER_SIZE));
    return pb_decode(&stream, taz_v1_CommandExecResponse_fields, out);
}

// Callback that collects bytes into a vector<uint8_t>.
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

// Callback that encodes a large block of bytes.
struct BytesEncodeCtx
{
    const uint8_t *data;
    size_t size;
};

bool encode_bytes_cb(pb_ostream_t *stream, const pb_field_iter_t *field,
                     void *const *arg)
{
    const auto *bctx = static_cast<const BytesEncodeCtx *>(*arg);
    if (!pb_encode_tag_for_field(stream, field))
    {
        return false;
    }
    return pb_encode_string(stream, bctx->data, bctx->size);
}

// ---------------------------------------------------------------------------
// Callback-based DirListResponse: no static-array cap on entry count
// ---------------------------------------------------------------------------

// Wrapper message with a callback field for `entries` (tag 1, wire type 2).
struct DirListResponseCb
{
    pb_callback_t entries;
};

// clang-format off
#define DirListResponseCb_FIELDLIST(X, a) \
    X(a, CALLBACK, REPEATED, MESSAGE, entries, 1)
// clang-format on

#define DirListResponseCb_DEFAULT         NULL
#define DirListResponseCb_CALLBACK        pb_default_field_callback
#define DirListResponseCb_entries_MSGTYPE taz_v1_DirEntry

PB_BIND(DirListResponseCb, DirListResponseCb, AUTO)

// Encode callback: writes `count` DirEntry sub-messages.
struct RepeatEntriesCtx
{
    int count;
};

bool encode_repeated_entries(pb_ostream_t *stream, const pb_field_iter_t *field,
                             void *const *arg)
{
    const auto *ctx = static_cast<const RepeatEntriesCtx *>(*arg);
    for (int i = 0; i < ctx->count; i++)
    {
        taz_v1_DirEntry entry = taz_v1_DirEntry_init_zero;
        (void)memset(entry.name, 'a' + (i % 26), sizeof(entry.name) - 1U);
        entry.kind = taz_v1_Kind_KIND_FILE;
        entry.size =
            static_cast<uint64_t>(static_cast<unsigned int>(i)) * 1000U;
        if (!pb_encode_tag_for_field(stream, field))
        {
            return false;
        }
        if (!pb_encode_submessage(stream, taz_v1_DirEntry_fields, &entry))
        {
            return false;
        }
    }
    return true;
}

// Decode callback: appends each DirEntry to a vector.
bool collect_decoded_entries(pb_istream_t *stream,
                             const pb_field_iter_t * /*field*/, void **arg)
{
    auto *entries = static_cast<std::vector<taz_v1_DirEntry> *>(*arg);
    taz_v1_DirEntry entry = taz_v1_DirEntry_init_zero;
    if (!pb_decode(stream, taz_v1_DirEntry_fields, &entry))
    {
        return false;
    }
    entries->push_back(entry);
    return true;
}

// ---------------------------------------------------------------------------
// Test: single-frame fast path
// ---------------------------------------------------------------------------

TEST(ResponseSplitter, SmallMessageSingleFrame)
{
    taz_v1_VersionResponse msg = taz_v1_VersionResponse_init_zero;
    (void)strncpy(msg.version, "1.2.3", sizeof(msg.version) - 1U);
    (void)strncpy(msg.build, "abc123", sizeof(msg.build) - 1U);
    (void)strncpy(msg.platform, "linux/amd64", sizeof(msg.platform) - 1U);

    WriteCtx wctx;
    taz_response_send(capture_write, &wctx, 42U,
                      static_cast<uint16_t>(taz_v1_Opcode_OPCODE_VERSION),
                      taz_v1_VersionResponse_fields, &msg);

    ASSERT_EQ(wctx.frames.size(), 1U);

    const taz_frame_header_t h = unpack_header(wctx.frames[0]);
    EXPECT_EQ(h.type,
              static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_RESPONSE));
    EXPECT_EQ(h.flags, static_cast<uint8_t>(taz_v1_FrameFlag_FRAME_FLAG_NONE));
    EXPECT_EQ(h.stream_id, 42U);
    EXPECT_EQ(h.opcode, static_cast<uint16_t>(taz_v1_Opcode_OPCODE_VERSION));
}

// ---------------------------------------------------------------------------
// Test: 200 KiB bytes field splits into four frames
// ---------------------------------------------------------------------------

TEST(ResponseSplitter, LargeBytesFieldYieldsFourFrames)
{
    static const size_t kDataSize = static_cast<size_t>(200U) * 1024U;
    std::vector<uint8_t> stdout_data(kDataSize, static_cast<uint8_t>('A'));

    BytesEncodeCtx bctx{stdout_data.data(), stdout_data.size()};

    taz_v1_CommandExecResponse msg = taz_v1_CommandExecResponse_init_zero;
    msg.exit_code = 42;
    msg.truncated = true;
    msg.stdout.funcs.encode = encode_bytes_cb;
    msg.stdout.arg = &bctx;

    WriteCtx wctx;
    taz_response_send(capture_write, &wctx, 1U,
                      static_cast<uint16_t>(taz_v1_Opcode_OPCODE_COMMAND_EXEC),
                      taz_v1_CommandExecResponse_fields, &msg);

    ASSERT_EQ(wctx.frames.size(), 4U);

    // First three frames: CONTINUATION set, RESPONSE type, correct stream/op.
    for (size_t i = 0U; i < 3U; i++)
    {
        const taz_frame_header_t h = unpack_header(wctx.frames[i]);
        EXPECT_EQ(h.type,
                  static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_RESPONSE))
            << "frame " << i;
        EXPECT_NE(h.flags & static_cast<uint8_t>(
                                taz_v1_FrameFlag_FRAME_FLAG_CONTINUATION),
                  0U)
            << "frame " << i << " must have CONTINUATION";
        EXPECT_EQ(h.stream_id, 1U) << "frame " << i;
        EXPECT_EQ(h.opcode,
                  static_cast<uint16_t>(taz_v1_Opcode_OPCODE_COMMAND_EXEC))
            << "frame " << i;

        // Scalars (exit_code, truncated) should be zero/false in non-final
        // frames.
        taz_v1_CommandExecResponse partial =
            taz_v1_CommandExecResponse_init_zero;
        ASSERT_TRUE(decode_exec_response(wctx.frames[i], &partial))
            << "frame " << i << " must be a valid protobuf message";
        EXPECT_EQ(partial.exit_code, 0) << "scalar exit_code in frame " << i;
        EXPECT_FALSE(partial.truncated) << "scalar truncated in frame " << i;
    }

    // Last frame: no CONTINUATION.
    {
        const taz_frame_header_t h = unpack_header(wctx.frames[3]);
        EXPECT_EQ(h.type,
                  static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_RESPONSE));
        EXPECT_EQ(h.flags & static_cast<uint8_t>(
                                taz_v1_FrameFlag_FRAME_FLAG_CONTINUATION),
                  0U);

        // Scalars should be present in the last frame.
        taz_v1_CommandExecResponse last = taz_v1_CommandExecResponse_init_zero;
        ASSERT_TRUE(decode_exec_response(wctx.frames[3], &last));
        EXPECT_EQ(last.exit_code, 42);
        EXPECT_TRUE(last.truncated);
    }

    // Verify full stdout bytes can be reconstructed by concatenating across
    // all four frames.
    std::vector<uint8_t> merged_stdout;
    for (size_t i = 0U; i < 4U; i++)
    {
        taz_v1_CommandExecResponse partial =
            taz_v1_CommandExecResponse_init_zero;
        partial.stdout.funcs.decode = collect_bytes_cb;
        partial.stdout.arg = &merged_stdout;
        ASSERT_TRUE(decode_exec_response(wctx.frames[i], &partial));
    }
    ASSERT_EQ(merged_stdout.size(), kDataSize);
    for (size_t i = 0U; i < kDataSize; i++)
    {
        ASSERT_EQ(merged_stdout[i], static_cast<uint8_t>('A'))
            << "byte mismatch at offset " << i;
    }
}

// ---------------------------------------------------------------------------
// Test: repeated sub-message elements are never split mid-element
// ---------------------------------------------------------------------------

TEST(ResponseSplitter, RepeatedElementsKeptAtomic)
{
    // Fill a DirListResponse with 64 entries with long names (~256 B each).
    // Each encoded entry is ~270 B; 64 * 270 ≈ 17 KiB — fits in one frame.
    // The test verifies: all entries survive the send and decode correctly
    // (no partial/corrupt elements).
    taz_v1_DirListResponse resp = taz_v1_DirListResponse_init_zero;
    resp.entries_count = 64;
    for (pb_size_t i = 0; i < 64; i++)
    {
        (void)memset(resp.entries[i].name, 'a' + (i % 26),
                     sizeof(resp.entries[i].name) - 1U);
        resp.entries[i].name[sizeof(resp.entries[i].name) - 1U] = '\0';
        resp.entries[i].kind = taz_v1_Kind_KIND_FILE;
        resp.entries[i].size = static_cast<uint64_t>(i) * 1000U;
    }

    WriteCtx wctx;
    taz_response_send(capture_write, &wctx, 7U,
                      static_cast<uint16_t>(taz_v1_Opcode_OPCODE_DIR_LIST),
                      taz_v1_DirListResponse_fields, &resp);

    // The entire response fits in one frame: no CONTINUATION.
    ASSERT_EQ(wctx.frames.size(), 1U);
    const taz_frame_header_t h = unpack_header(wctx.frames[0]);
    EXPECT_EQ(h.flags & static_cast<uint8_t>(
                            taz_v1_FrameFlag_FRAME_FLAG_CONTINUATION),
              0U);

    // Decode the frame and verify all 64 entries are intact.
    pb_istream_t stream = pb_istream_from_buffer(
        wctx.frames[0].data() + TAZ_FRAME_HEADER_SIZE,
        wctx.frames[0].size() - static_cast<size_t>(TAZ_FRAME_HEADER_SIZE));
    taz_v1_DirListResponse decoded = taz_v1_DirListResponse_init_zero;
    ASSERT_TRUE(pb_decode(&stream, taz_v1_DirListResponse_fields, &decoded));
    ASSERT_EQ(decoded.entries_count, 64);
    for (pb_size_t i = 0; i < 64; i++)
    {
        char expected[sizeof(resp.entries[i].name)];
        (void)memset(expected, 'a' + (i % 26), sizeof(expected) - 1U);
        expected[sizeof(expected) - 1U] = '\0';
        EXPECT_STREQ(decoded.entries[i].name, expected) << "entry " << i;
        EXPECT_EQ(decoded.entries[i].kind, taz_v1_Kind_KIND_FILE)
            << "entry " << i;
        EXPECT_EQ(decoded.entries[i].size, static_cast<uint64_t>(i) * 1000U)
            << "entry " << i;
    }
}

// ---------------------------------------------------------------------------
// Test: repeated sub-message elements kept atomic on slow path (>64 KiB)
// ---------------------------------------------------------------------------

TEST(ResponseSplitter, RepeatedElementsKeptAtomicSlowPath)
{
    // 300 DirEntry sub-messages with 255-char names: each encodes to ~267 B,
    // total ~80 KiB > 65 KiB, which forces the slow-path chunker.  Every
    // element fits within one frame, so the chunker must place each one
    // atomically — flushing the current frame first when needed — and must
    // never split an element across a frame boundary.
    static const int kCount = 300;

    RepeatEntriesCtx ectx{kCount};
    DirListResponseCb msg{};
    msg.entries.funcs.encode = encode_repeated_entries;
    msg.entries.arg = &ectx;

    WriteCtx wctx;
    taz_response_send(capture_write, &wctx, 8U,
                      static_cast<uint16_t>(taz_v1_Opcode_OPCODE_DIR_LIST),
                      &DirListResponseCb_msg, &msg);

    // Slow path must have been taken: more than one frame.
    ASSERT_GT(wctx.frames.size(), 1U);

    // All but the last frame must carry CONTINUATION.
    for (size_t i = 0U; i + 1U < wctx.frames.size(); i++)
    {
        const taz_frame_header_t h = unpack_header(wctx.frames[i]);
        EXPECT_NE(h.flags & static_cast<uint8_t>(
                                taz_v1_FrameFlag_FRAME_FLAG_CONTINUATION),
                  0U)
            << "frame " << i << " missing CONTINUATION";
    }
    {
        const taz_frame_header_t h =
            unpack_header(wctx.frames[wctx.frames.size() - 1U]);
        EXPECT_EQ(h.flags & static_cast<uint8_t>(
                                taz_v1_FrameFlag_FRAME_FLAG_CONTINUATION),
                  0U)
            << "last frame must not have CONTINUATION";
    }

    // Decode every frame and collect all DirEntry elements.
    std::vector<taz_v1_DirEntry> decoded;
    for (size_t fi = 0U; fi < wctx.frames.size(); fi++)
    {
        DirListResponseCb resp{};
        resp.entries.funcs.decode = collect_decoded_entries;
        resp.entries.arg = &decoded;
        pb_istream_t stream = pb_istream_from_buffer(
            wctx.frames[fi].data() + TAZ_FRAME_HEADER_SIZE,
            wctx.frames[fi].size() -
                static_cast<size_t>(TAZ_FRAME_HEADER_SIZE));
        ASSERT_TRUE(pb_decode(&stream, &DirListResponseCb_msg, &resp))
            << "frame " << fi << " does not decode as a valid DirListResponse";
    }

    // All 300 elements must be present and bit-exact.
    ASSERT_EQ(decoded.size(), static_cast<size_t>(kCount));
    for (size_t i = 0U; i < static_cast<size_t>(kCount); i++)
    {
        char expected[sizeof(taz_v1_DirEntry::name)];
        (void)memset(expected, 'a' + static_cast<int>(i % 26U),
                     sizeof(expected) - 1U);
        expected[sizeof(expected) - 1U] = '\0';
        EXPECT_STREQ(decoded[i].name, expected) << "entry " << i;
        EXPECT_EQ(decoded[i].kind, taz_v1_Kind_KIND_FILE) << "entry " << i;
        EXPECT_EQ(decoded[i].size, i * 1000U) << "entry " << i;
    }
}

// ---------------------------------------------------------------------------
// Test: two large bytes fields — both preserved across frames
// ---------------------------------------------------------------------------

TEST(ResponseSplitter, TwoLargeBytesFields)
{
    // stdout = 100 KiB 'S', stderr = 50 KiB 'E'.  Total > 64 KiB → must split.
    static const size_t kOutSize = static_cast<size_t>(100U) * 1024U;
    static const size_t kErrSize = static_cast<size_t>(50U) * 1024U;

    std::vector<uint8_t> out_data(kOutSize, static_cast<uint8_t>('S'));
    std::vector<uint8_t> err_data(kErrSize, static_cast<uint8_t>('E'));

    BytesEncodeCtx out_ctx{out_data.data(), out_data.size()};
    BytesEncodeCtx err_ctx{err_data.data(), err_data.size()};

    taz_v1_CommandExecResponse msg = taz_v1_CommandExecResponse_init_zero;
    msg.exit_code = 0;
    msg.stdout.funcs.encode = encode_bytes_cb;
    msg.stdout.arg = &out_ctx;
    msg.stderr.funcs.encode = encode_bytes_cb;
    msg.stderr.arg = &err_ctx;

    WriteCtx wctx;
    taz_response_send(capture_write, &wctx, 2U,
                      static_cast<uint16_t>(taz_v1_Opcode_OPCODE_COMMAND_EXEC),
                      taz_v1_CommandExecResponse_fields, &msg);

    ASSERT_GT(wctx.frames.size(), 1U);

    // All but last have CONTINUATION.
    for (size_t i = 0U; i + 1U < wctx.frames.size(); i++)
    {
        const taz_frame_header_t h = unpack_header(wctx.frames[i]);
        EXPECT_NE(h.flags & static_cast<uint8_t>(
                                taz_v1_FrameFlag_FRAME_FLAG_CONTINUATION),
                  0U)
            << "frame " << i << " must have CONTINUATION";
    }
    // Last has no CONTINUATION.
    {
        const taz_frame_header_t h =
            unpack_header(wctx.frames[wctx.frames.size() - 1U]);
        EXPECT_EQ(h.flags & static_cast<uint8_t>(
                                taz_v1_FrameFlag_FRAME_FLAG_CONTINUATION),
                  0U);
    }

    // Reconstruct stdout and stderr by merging across all frames.
    std::vector<uint8_t> merged_out;
    std::vector<uint8_t> merged_err;
    for (const auto &frame : wctx.frames)
    {
        taz_v1_CommandExecResponse partial =
            taz_v1_CommandExecResponse_init_zero;
        partial.stdout.funcs.decode = collect_bytes_cb;
        partial.stdout.arg = &merged_out;
        partial.stderr.funcs.decode = collect_bytes_cb;
        partial.stderr.arg = &merged_err;
        ASSERT_TRUE(decode_exec_response(frame, &partial));
    }

    EXPECT_EQ(merged_out.size(), kOutSize);
    EXPECT_EQ(merged_err.size(), kErrSize);

    for (size_t i = 0U; i < kOutSize; i++)
    {
        ASSERT_EQ(merged_out[i], static_cast<uint8_t>('S'))
            << "stdout byte " << i;
    }
    for (size_t i = 0U; i < kErrSize; i++)
    {
        ASSERT_EQ(merged_err[i], static_cast<uint8_t>('E'))
            << "stderr byte " << i;
    }
}

// ---------------------------------------------------------------------------
// Test: each frame payload stays within the RESPONSE limit
// ---------------------------------------------------------------------------

TEST(ResponseSplitter, AllFramesWithinPayloadLimit)
{
    static const size_t kDataSize = static_cast<size_t>(200U) * 1024U;
    std::vector<uint8_t> data(kDataSize, static_cast<uint8_t>('X'));
    BytesEncodeCtx bctx{data.data(), data.size()};

    taz_v1_CommandExecResponse msg = taz_v1_CommandExecResponse_init_zero;
    msg.stdout.funcs.encode = encode_bytes_cb;
    msg.stdout.arg = &bctx;

    WriteCtx wctx;
    taz_response_send(capture_write, &wctx, 3U,
                      static_cast<uint16_t>(taz_v1_Opcode_OPCODE_COMMAND_EXEC),
                      taz_v1_CommandExecResponse_fields, &msg);

    for (size_t i = 0U; i < wctx.frames.size(); i++)
    {
        const taz_frame_header_t h = unpack_header(wctx.frames[i]);
        EXPECT_LE(h.length,
                  static_cast<uint32_t>(TAZ_FRAME_MAX_PAYLOAD_RESPONSE))
            << "frame " << i << " exceeds RESPONSE payload limit";
        EXPECT_EQ(wctx.frames[i].size(),
                  static_cast<size_t>(TAZ_FRAME_HEADER_SIZE) + h.length)
            << "frame " << i << " size mismatch";
    }
}

} // namespace
