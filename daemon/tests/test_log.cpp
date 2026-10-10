// Unit tests for taz/log.h: the in-memory log ring, its config-gated
// recording, and taz_log_collect's query semantics.

#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <pb_decode.h>
#include <pb_encode.h>
#include <uv.h>

#include "file_test_support.h"
#include "taz/config.h"
#include "taz/log.h"
#include "taz/v1/advanced.pb.h"
#include "taz/v1/common.pb.h"
#include "taz/v1/daemon_control.pb.h"

namespace
{

void SetConfigLogLevel(const char *level)
{
    taz_v1_ConfigurationUpdateRequest req =
        taz_v1_ConfigurationUpdateRequest_init_zero;
    req.config_count = 1;
    std::strncpy(req.config[0].key, "log.level", sizeof(req.config[0].key) - 1);
    std::strncpy(req.config[0].value, level, sizeof(req.config[0].value) - 1);
    taz_v1_ConfigurationUpdateResponse resp;
    taz_config_update(&req, &resp);
}

class LogTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        taz_config_reset();
        taz_log_init();
        taz_log_reset_for_tests();
        SetConfigLogLevel("DEBUG");
    }

    void TearDown() override
    {
        taz_log_set_capacity_for_tests(TAZ_LOG_RING_CAPACITY);
        taz_log_reset_for_tests();
        taz_config_reset();
    }
};

} // namespace

// ---------------------------------------------------------------------------
// taz_log / taz_log_collect: recording and level gating
// ---------------------------------------------------------------------------

TEST_F(LogTest, RecordsAtEveryLevelWhenConfigIsDebug)
{
    taz_log(TAZ_LOG_DEBUG, "d");
    taz_log(TAZ_LOG_INFO, "i");
    taz_log(TAZ_LOG_WARN, "w");
    taz_log(TAZ_LOG_ERROR, "e");

    taz_v1_LogEntry out[8];
    const size_t n = taz_log_collect(0, 0, TAZ_LOG_DEBUG, out, 8);
    ASSERT_EQ(n, 4U);
    EXPECT_STREQ(out[0].level, "DEBUG");
    EXPECT_STREQ(out[0].message, "d");
    EXPECT_STREQ(out[1].level, "INFO");
    EXPECT_STREQ(out[2].level, "WARN");
    EXPECT_STREQ(out[3].level, "ERROR");
}

TEST_F(LogTest, SubThresholdEntriesAreNotRecordedWhenConfigLevelIsWarn)
{
    SetConfigLogLevel("WARN");

    taz_log(TAZ_LOG_DEBUG, "d");
    taz_log(TAZ_LOG_INFO, "i");
    taz_log(TAZ_LOG_WARN, "w");
    taz_log(TAZ_LOG_ERROR, "e");

    taz_v1_LogEntry out[8];
    const size_t n = taz_log_collect(0, 0, TAZ_LOG_DEBUG, out, 8);
    ASSERT_EQ(n, 2U);
    EXPECT_STREQ(out[0].level, "WARN");
    EXPECT_STREQ(out[1].level, "ERROR");
}

TEST_F(LogTest, LogBeforeInitIsSafeNoOp)
{
    taz_log_shutdown();

    // Must not crash, even though nothing is recorded (there is no ring to
    // record into).
    taz_log(TAZ_LOG_ERROR, "before init");

    taz_log_init();
    taz_log_reset_for_tests();
    taz_v1_LogEntry out[1];
    EXPECT_EQ(taz_log_collect(0, 0, TAZ_LOG_DEBUG, out, 1), 0U);
}

// ---------------------------------------------------------------------------
// taz_log_collect: min_level filter
// ---------------------------------------------------------------------------

TEST_F(LogTest, CollectFiltersByMinLevel)
{
    taz_log(TAZ_LOG_DEBUG, "d");
    taz_log(TAZ_LOG_INFO, "i");
    taz_log(TAZ_LOG_WARN, "w");
    taz_log(TAZ_LOG_ERROR, "e");

    taz_v1_LogEntry out[8];
    const size_t n = taz_log_collect(0, 0, TAZ_LOG_ERROR, out, 8);
    ASSERT_EQ(n, 1U);
    EXPECT_STREQ(out[0].level, "ERROR");
}

// ---------------------------------------------------------------------------
// taz_log_collect: since is strictly-after
// ---------------------------------------------------------------------------

TEST_F(LogTest, CollectSinceIsStrictlyAfter)
{
    taz_log(TAZ_LOG_INFO, "only");

    taz_v1_LogEntry out[8];
    ASSERT_EQ(taz_log_collect(0, 0, TAZ_LOG_DEBUG, out, 8), 1U);
    const uint64_t ts = out[0].timestamp;

    // since == the entry's own timestamp excludes it (strictly after).
    EXPECT_EQ(taz_log_collect(0, ts, TAZ_LOG_DEBUG, out, 8), 0U);
    // since one second earlier includes it.
    EXPECT_EQ(taz_log_collect(0, ts > 0U ? ts - 1U : 0U, TAZ_LOG_DEBUG, out, 8),
              1U);
}

// ---------------------------------------------------------------------------
// taz_log_collect: lines
// ---------------------------------------------------------------------------

TEST_F(LogTest, LinesZeroMeansAll)
{
    for (int i = 0; i < 5; i++)
    {
        taz_log(TAZ_LOG_INFO, "m%d", i);
    }

    taz_v1_LogEntry out[8];
    EXPECT_EQ(taz_log_collect(0, 0, TAZ_LOG_DEBUG, out, 8), 5U);
}

TEST_F(LogTest, LinesReturnsNewestNOldestFirst)
{
    for (int i = 0; i < 5; i++)
    {
        taz_log(TAZ_LOG_INFO, "m%d", i);
    }

    taz_v1_LogEntry out[8];
    const size_t n = taz_log_collect(2, 0, TAZ_LOG_DEBUG, out, 8);
    ASSERT_EQ(n, 2U);
    EXPECT_STREQ(out[0].message, "m3");
    EXPECT_STREQ(out[1].message, "m4");
}

TEST_F(LogTest, LinesLargerThanAvailableReturnsEverything)
{
    taz_log(TAZ_LOG_INFO, "m0");
    taz_log(TAZ_LOG_INFO, "m1");

    taz_v1_LogEntry out[8];
    EXPECT_EQ(taz_log_collect(100, 0, TAZ_LOG_DEBUG, out, 8), 2U);
}

TEST_F(LogTest, LinesAppliesAfterLevelAndSinceFiltering)
{
    taz_log(TAZ_LOG_DEBUG, "d0");
    taz_log(TAZ_LOG_INFO, "i0");
    taz_log(TAZ_LOG_INFO, "i1");
    taz_log(TAZ_LOG_INFO, "i2");

    // Of the 3 INFO-or-above entries, the newest 2.
    taz_v1_LogEntry out[8];
    const size_t n = taz_log_collect(2, 0, TAZ_LOG_INFO, out, 8);
    ASSERT_EQ(n, 2U);
    EXPECT_STREQ(out[0].message, "i1");
    EXPECT_STREQ(out[1].message, "i2");
}

// ---------------------------------------------------------------------------
// Ring wrap
// ---------------------------------------------------------------------------

TEST_F(LogTest, RingWrapKeepsOnlyNewestCapacityEntriesInOrder)
{
    taz_log_set_capacity_for_tests(4);

    for (int i = 0; i < 6; i++)
    {
        taz_log(TAZ_LOG_INFO, "m%d", i);
    }

    taz_v1_LogEntry out[8];
    const size_t n = taz_log_collect(0, 0, TAZ_LOG_DEBUG, out, 8);
    ASSERT_EQ(n, 4U);
    EXPECT_STREQ(out[0].message, "m2");
    EXPECT_STREQ(out[1].message, "m3");
    EXPECT_STREQ(out[2].message, "m4");
    EXPECT_STREQ(out[3].message, "m5");
}

TEST_F(LogTest, OutCapSmallerThanMatchesTruncatesWithoutCrashing)
{
    for (int i = 0; i < 5; i++)
    {
        taz_log(TAZ_LOG_INFO, "m%d", i);
    }

    taz_v1_LogEntry out[2];
    const size_t n = taz_log_collect(0, 0, TAZ_LOG_DEBUG, out, 2);
    ASSERT_EQ(n, 2U);
    EXPECT_STREQ(out[0].message, "m0");
    EXPECT_STREQ(out[1].message, "m1");
}

// ---------------------------------------------------------------------------
// UTF-8 sanitization / truncation
// ---------------------------------------------------------------------------

TEST_F(LogTest, OverLongMessageIsCutAtCodepointBoundary)
{
    // "\xC3\xA9" is U+00E9 (e-acute), a 2-byte UTF-8 sequence. 300 copies is
    // 600 bytes, well past LogEntry.message's 512-byte field.
    std::string input;
    for (int i = 0; i < 300; i++)
    {
        input += "\xC3\xA9";
    }
    taz_log(TAZ_LOG_INFO, "%s", input.c_str());

    taz_v1_LogEntry out[1];
    ASSERT_EQ(taz_log_collect(0, 0, TAZ_LOG_DEBUG, out, 1), 1U);

    // 511 usable bytes (512 - NUL) fit 255 whole 2-byte units (510 bytes);
    // the 256th unit is dropped whole rather than split.
    std::string expected;
    for (int i = 0; i < 255; i++)
    {
        expected += "\xC3\xA9";
    }
    ASSERT_EQ(expected.size(), 510U);
    EXPECT_EQ(std::string(out[0].message), expected);
}

TEST_F(LogTest, InvalidUtf8BecomesReplacementCharacter)
{
    // Two stray UTF-8 continuation bytes: each is one invalid maximal
    // subpart, replaced by one U+FFFD (EF BF BD) apiece.
    const std::string input = "\x80\x80";
    taz_log(TAZ_LOG_INFO, "%s", input.c_str());

    taz_v1_LogEntry out[1];
    ASSERT_EQ(taz_log_collect(0, 0, TAZ_LOG_DEBUG, out, 1), 1U);

    const std::string expected = "\xEF\xBF\xBD\xEF\xBF\xBD";
    EXPECT_EQ(std::string(out[0].message), expected);
}

// ---------------------------------------------------------------------------
// Concurrency (TSan/Valgrind row)
// ---------------------------------------------------------------------------

namespace
{

struct ConcurrentLogCtx
{
    int thread_index = 0;
};

constexpr int kConcurrentThreads = 8;
constexpr int kConcurrentItersPerThread = 50;
constexpr size_t kConcurrentTotal =
    static_cast<size_t>(kConcurrentThreads) *
    static_cast<size_t>(kConcurrentItersPerThread);

void ConcurrentLogWork(void *arg)
{
    auto *ctx = static_cast<ConcurrentLogCtx *>(arg);
    for (int i = 0; i < kConcurrentItersPerThread; i++)
    {
        taz_log(TAZ_LOG_INFO, "thread %d iter %d", ctx->thread_index, i);
    }
}

} // namespace

TEST_F(LogTest, ConcurrentLoggingFromManyThreadsIsConsistent)
{
    std::vector<uv_thread_t> threads(kConcurrentThreads);
    std::vector<ConcurrentLogCtx> ctxs(kConcurrentThreads);
    for (size_t i = 0; i < threads.size(); i++)
    {
        ctxs[i].thread_index = static_cast<int>(i);
        ASSERT_EQ(uv_thread_create(&threads[i], ConcurrentLogWork, &ctxs[i]),
                  0);
    }
    for (size_t i = 0; i < threads.size(); i++)
    {
        ASSERT_EQ(uv_thread_join(&threads[i]), 0);
    }

    taz_v1_LogEntry out[kConcurrentTotal];
    const size_t n =
        taz_log_collect(0, 0, TAZ_LOG_DEBUG, out, kConcurrentTotal);
    EXPECT_EQ(n, kConcurrentTotal);
}

namespace
{

void ConcurrentConfigFlipWork(void *arg)
{
    (void)arg;
    static const char *const kLevels[] = {"DEBUG", "INFO", "WARN", "ERROR"};
    for (int i = 0; i < kConcurrentItersPerThread; i++)
    {
        SetConfigLogLevel(kLevels[i % 4]);
    }
}

} // namespace

// Pairs a thread calling taz_config_update("log.level", ...) - the loop
// thread's path for CONFIGURATION_UPDATE - with a thread calling taz_log
// - a pool thread's path - to prove the level gate and the config write no
// longer race (TSan: see the rejected review of this task).
TEST_F(LogTest, ConcurrentConfigUpdateDuringLoggingIsRaceFree)
{
    ConcurrentLogCtx log_ctx;
    log_ctx.thread_index = 0;

    uv_thread_t log_thread;
    uv_thread_t config_thread;
    ASSERT_EQ(uv_thread_create(&log_thread, ConcurrentLogWork, &log_ctx), 0);
    ASSERT_EQ(
        uv_thread_create(&config_thread, ConcurrentConfigFlipWork, nullptr), 0);

    ASSERT_EQ(uv_thread_join(&log_thread), 0);
    ASSERT_EQ(uv_thread_join(&config_thread), 0);
}

// ---------------------------------------------------------------------------
// taz_log_level_parse
// ---------------------------------------------------------------------------

TEST(LogLevelParse, EmptyStringIsDebug)
{
    taz_log_level_t level;
    ASSERT_EQ(taz_log_level_parse("", &level), 0);
    EXPECT_EQ(level, TAZ_LOG_DEBUG);
}

TEST(LogLevelParse, EveryExactToken)
{
    const struct
    {
        const char *token;
        taz_log_level_t level;
    } cases[] = {
        {"DEBUG", TAZ_LOG_DEBUG},
        {"INFO", TAZ_LOG_INFO},
        {"WARN", TAZ_LOG_WARN},
        {"ERROR", TAZ_LOG_ERROR},
    };
    for (const auto &c : cases)
    {
        taz_log_level_t level;
        ASSERT_EQ(taz_log_level_parse(c.token, &level), 0) << c.token;
        EXPECT_EQ(level, c.level) << c.token;
    }
}

TEST(LogLevelParse, UnknownTokenFails)
{
    taz_log_level_t level = TAZ_LOG_WARN;
    EXPECT_NE(taz_log_level_parse("TRACE", &level), 0);
    // *out is left unchanged on failure.
    EXPECT_EQ(level, TAZ_LOG_WARN);
}

TEST(LogLevelParse, IsCaseSensitive)
{
    taz_log_level_t level;
    EXPECT_NE(taz_log_level_parse("info", &level), 0);
    EXPECT_NE(taz_log_level_parse("Info", &level), 0);
    ASSERT_EQ(taz_log_level_parse("INFO", &level), 0);
    EXPECT_EQ(level, TAZ_LOG_INFO);
}

// ---------------------------------------------------------------------------
// handle_log (0x0042), via taz_dispatch_frame
// ---------------------------------------------------------------------------

namespace
{

std::vector<uint8_t> encode_log_request(uint32_t lines, uint64_t since,
                                        const std::string &level)
{
    taz_v1_LogRequest req = taz_v1_LogRequest_init_zero;
    req.lines = lines;
    req.since = since;
    if (!level.empty())
    {
        (void)strncpy(req.level, level.c_str(), sizeof(req.level) - 1U);
    }
    std::vector<uint8_t> buf(taz_v1_LogRequest_size);
    pb_ostream_t ostream = pb_ostream_from_buffer(buf.data(), buf.size());
    EXPECT_TRUE(pb_encode(&ostream, taz_v1_LogRequest_fields, &req));
    buf.resize(ostream.bytes_written);
    return buf;
}

bool FrameHasContinuation(const std::vector<uint8_t> &frame)
{
    return (unpack_header(frame).flags &
            static_cast<uint8_t>(taz_v1_FrameFlag_FRAME_FLAG_CONTINUATION)) !=
           0U;
}

// Decodes every captured frame as a LogResponse and returns the
// concatenated entries in frame order (what a real client's chunked-
// RESPONSE merge would reconstruct).
std::vector<taz_v1_LogEntry>
decode_all_log_entries(const std::vector<std::vector<uint8_t>> &frames)
{
    std::vector<taz_v1_LogEntry> entries;
    for (const auto &frame : frames)
    {
        const std::vector<uint8_t> body = frame_payload(frame);
        taz_v1_LogResponse resp = taz_v1_LogResponse_init_zero;
        pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
        EXPECT_TRUE(pb_decode(&istream, taz_v1_LogResponse_fields, &resp));
        for (pb_size_t i = 0; i < resp.entries_count; i++)
        {
            entries.push_back(resp.entries[i]);
        }
    }
    return entries;
}

// Mirrors LogTest's fixture (config/ring reset around every test) on top of
// FileHandlerTest's real loop + fake dispatch, for driving LOG requests
// through taz_dispatch_frame.
class LogHandlerTest : public FileHandlerTest
{
  protected:
    void SetUp() override
    {
        FileHandlerTest::SetUp();
        taz_config_reset();
        taz_log_init();
        taz_log_reset_for_tests();
        SetConfigLogLevel("DEBUG");
    }

    void TearDown() override
    {
        taz_log_set_capacity_for_tests(TAZ_LOG_RING_CAPACITY);
        taz_log_reset_for_tests();
        taz_config_reset();
        FileHandlerTest::TearDown();
    }
};

} // namespace

TEST_F(LogHandlerTest, EmptyLevelReturnsAllRecordedEntriesOldestFirst)
{
    taz_log(TAZ_LOG_DEBUG, "d0");
    taz_log(TAZ_LOG_INFO, "i0");
    taz_log(TAZ_LOG_ERROR, "e0");

    DispatchRequest(taz_v1_Opcode_OPCODE_LOG, encode_log_request(0, 0, ""), 1U);

    ASSERT_EQ(Frames().size(), 1U);
    EXPECT_FALSE(FrameHasContinuation(Frames()[0]));
    const std::vector<taz_v1_LogEntry> entries =
        decode_all_log_entries(Frames());
    ASSERT_EQ(entries.size(), 3U);
    EXPECT_STREQ(entries[0].message, "d0");
    EXPECT_STREQ(entries[1].message, "i0");
    EXPECT_STREQ(entries[2].message, "e0");

    EXPECT_EQ(RefCount(), 0);
    EXPECT_EQ(UnrefCount(), 0);
    EXPECT_EQ(ActiveStreamCount(), 0U);
}

TEST_F(LogHandlerTest, ErrorLevelExcludesInfo)
{
    taz_log(TAZ_LOG_INFO, "i0");
    taz_log(TAZ_LOG_ERROR, "e0");

    DispatchRequest(taz_v1_Opcode_OPCODE_LOG, encode_log_request(0, 0, "ERROR"),
                    2U);

    const std::vector<taz_v1_LogEntry> entries =
        decode_all_log_entries(Frames());
    ASSERT_EQ(entries.size(), 1U);
    EXPECT_STREQ(entries[0].message, "e0");
}

TEST_F(LogHandlerTest, UnknownLevelIsInvalidRequestAndStreamIsReleased)
{
    DispatchRequest(taz_v1_Opcode_OPCODE_LOG, encode_log_request(0, 0, "TRACE"),
                    3U);

    ASSERT_EQ(Frames().size(), 1U);
    EXPECT_EQ(unpack_header(Frames()[0]).type,
              static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_ERROR));
    taz_v1_ErrorInfo err = taz_v1_ErrorInfo_init_zero;
    const std::vector<uint8_t> body = frame_payload(Frames()[0]);
    pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
    ASSERT_TRUE(pb_decode(&istream, taz_v1_ErrorInfo_fields, &err));
    EXPECT_EQ(err.code, taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST);

    EXPECT_EQ(ActiveStreamCount(), 0U);
}

TEST_F(LogHandlerTest, LinesReturnsNewestNOldestFirst)
{
    for (int i = 0; i < 5; i++)
    {
        taz_log(TAZ_LOG_INFO, "m%d", i);
    }

    DispatchRequest(taz_v1_Opcode_OPCODE_LOG, encode_log_request(2, 0, ""), 4U);

    const std::vector<taz_v1_LogEntry> entries =
        decode_all_log_entries(Frames());
    ASSERT_EQ(entries.size(), 2U);
    EXPECT_STREQ(entries[0].message, "m3");
    EXPECT_STREQ(entries[1].message, "m4");
}

TEST_F(LogHandlerTest, LinesZeroReturnsAll)
{
    for (int i = 0; i < 5; i++)
    {
        taz_log(TAZ_LOG_INFO, "m%d", i);
    }

    DispatchRequest(taz_v1_Opcode_OPCODE_LOG, encode_log_request(0, 0, ""), 5U);

    EXPECT_EQ(decode_all_log_entries(Frames()).size(), 5U);
}

TEST_F(LogHandlerTest, NoMatchingEntriesYieldsOneEmptyFrame)
{
    DispatchRequest(taz_v1_Opcode_OPCODE_LOG, encode_log_request(0, 0, ""), 6U);

    ASSERT_EQ(Frames().size(), 1U);
    EXPECT_FALSE(FrameHasContinuation(Frames()[0]));
    EXPECT_EQ(decode_all_log_entries(Frames()).size(), 0U);
}

TEST_F(LogHandlerTest, SixtyFourEntriesYieldExactlyOneFrame)
{
    for (int i = 0; i < 64; i++)
    {
        taz_log(TAZ_LOG_INFO, "m%04d", i);
    }

    DispatchRequest(taz_v1_Opcode_OPCODE_LOG, encode_log_request(0, 0, ""), 7U);

    ASSERT_EQ(Frames().size(), 1U);
    EXPECT_FALSE(FrameHasContinuation(Frames()[0]));
    EXPECT_EQ(decode_all_log_entries(Frames()).size(), 64U);
}

TEST_F(LogHandlerTest, SixtyFiveEntriesYieldTwoFrames)
{
    for (int i = 0; i < 65; i++)
    {
        taz_log(TAZ_LOG_INFO, "m%04d", i);
    }

    DispatchRequest(taz_v1_Opcode_OPCODE_LOG, encode_log_request(0, 0, ""), 8U);

    ASSERT_EQ(Frames().size(), 2U);
    EXPECT_TRUE(FrameHasContinuation(Frames()[0]));
    EXPECT_FALSE(FrameHasContinuation(Frames()[1]));
    const std::vector<taz_v1_LogEntry> entries =
        decode_all_log_entries(Frames());
    ASSERT_EQ(entries.size(), 65U);
    EXPECT_STREQ(entries[0].message, "m0000");
    EXPECT_STREQ(entries[64].message, "m0064");
}
