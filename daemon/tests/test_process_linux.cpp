// Unit tests for the Linux platform API in taz/process.h: pure, crafted-text
// parsers that do no /proc I/O themselves. Linux-only: the Windows platform
// TU does not declare or implement these, and this whole file compiles out
// on that platform.
#ifndef _WIN32

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "taz/process.h"

namespace
{

// A minimal, valid 22-token tail for a /proc/<pid>/stat line, starting right
// after the comm's closing ')': field 3 (state) through field 24 (rss).
// utime (field 14) = 7, stime (field 15) = 3, starttime (field 22) = 55555,
// rss (field 24) = 999; every other field is an unused 0.
const char *const kValidStatTail =
    "S 0 0 0 0 0 0 0 0 0 0 7 3 0 0 0 0 0 0 55555 0 999";

// ---------------------------------------------------------------------------
// taz_proc_parse_stat
// ---------------------------------------------------------------------------

TEST(ParseStat, RealLookingLineParsesAllFields)
{
    // 52 whitespace-separated fields total, matching a real
    // /proc/<pid>/stat line's shape.
    const std::string line =
        "1234 (python3) S 1 1234 1234 0 -1 4194560 100 0 0 0 7 3 0 0 20 0 1 0 "
        "55555 12345678 999 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 "
        "0 0 0 0";
    taz_proc_stat_t out;

    ASSERT_EQ(taz_proc_parse_stat(line.data(), line.size(), &out), 1);
    EXPECT_STREQ(out.comm, "python3");
    EXPECT_EQ(out.state, 'S');
    EXPECT_EQ(out.utime, 7U);
    EXPECT_EQ(out.stime, 3U);
    EXPECT_EQ(out.starttime, 55555U);
    EXPECT_EQ(out.rss_pages, 999U);
}

TEST(ParseStat, CommWithSpacesAndParensIsParsedFromTheLastCloseParen)
{
    const std::string line = std::string("1 (a b) (c) ") + kValidStatTail;
    taz_proc_stat_t out;

    ASSERT_EQ(taz_proc_parse_stat(line.data(), line.size(), &out), 1);
    EXPECT_STREQ(out.comm, "a b) (c");
}

TEST(ParseStat, FifteenByteCommParsesCorrectly)
{
    const std::string comm = "abcdefghijklmno"; // exactly 15 bytes
    ASSERT_EQ(comm.size(), 15U);
    const std::string line = std::string("1 (") + comm + ") " + kValidStatTail;
    taz_proc_stat_t out;

    ASSERT_EQ(taz_proc_parse_stat(line.data(), line.size(), &out), 1);
    EXPECT_STREQ(out.comm, comm.c_str());
}

TEST(ParseStat, MissingOpenParenFails)
{
    const std::string line = std::string("1 python3) ") + kValidStatTail;
    taz_proc_stat_t out;

    EXPECT_EQ(taz_proc_parse_stat(line.data(), line.size(), &out), 0);
}

TEST(ParseStat, MissingCloseParenFails)
{
    const std::string line = std::string("1 (python3 ") + kValidStatTail;
    taz_proc_stat_t out;

    EXPECT_EQ(taz_proc_parse_stat(line.data(), line.size(), &out), 0);
}

TEST(ParseStat, FewerThanTwentyFourFieldsFails)
{
    // Drops the final field (rss), leaving only 21 tokens after ')'.
    const std::string short_tail =
        "S 0 0 0 0 0 0 0 0 0 0 7 3 0 0 0 0 0 0 55555 0";
    const std::string line = std::string("1 (python3) ") + short_tail;
    taz_proc_stat_t out;

    EXPECT_EQ(taz_proc_parse_stat(line.data(), line.size(), &out), 0);
}

TEST(ParseStat, NonNumericFieldFails)
{
    const std::string bad_tail =
        "S 0 0 0 0 0 0 0 0 0 0 abc 3 0 0 0 0 0 0 55555 0 999";
    const std::string line = std::string("1 (python3) ") + bad_tail;
    taz_proc_stat_t out;

    EXPECT_EQ(taz_proc_parse_stat(line.data(), line.size(), &out), 0);
}

TEST(ParseStat, LenShorterThanTextIsHonoredNotTheNulOrFullString)
{
    // The real line ends right after "999"; a second, bogus ')' lives past
    // that in memory. Passing len up to the real line's end must parse
    // correctly and must never let the trailing ')' affect the "last )"
    // search.
    const std::string real_line = std::string("1 (python3) ") + kValidStatTail;
    const std::string buffer = real_line + ")trailing-garbage(unused";
    taz_proc_stat_t out;

    ASSERT_EQ(taz_proc_parse_stat(buffer.data(), real_line.size(), &out), 1);
    EXPECT_STREQ(out.comm, "python3");
    EXPECT_EQ(out.utime, 7U);
    EXPECT_EQ(out.stime, 3U);
    EXPECT_EQ(out.starttime, 55555U);
    EXPECT_EQ(out.rss_pages, 999U);
}

// ---------------------------------------------------------------------------
// taz_proc_parse_status_uid
// ---------------------------------------------------------------------------

TEST(ParseStatusUid, TabSeparatedUidLineParses)
{
    const std::string text =
        "Name:\tbash\nUid:\t1000\t1000\t1000\t1000\nGid:\t1000\n";
    unsigned long uid = 0UL;

    ASSERT_EQ(taz_proc_parse_status_uid(text.data(), text.size(), &uid), 1);
    EXPECT_EQ(uid, 1000UL);
}

TEST(ParseStatusUid, SpaceSeparatedUidLineParses)
{
    const std::string text = "Uid: 1000 1000 1000 1000\n";
    unsigned long uid = 0UL;

    ASSERT_EQ(taz_proc_parse_status_uid(text.data(), text.size(), &uid), 1);
    EXPECT_EQ(uid, 1000UL);
}

// The Name: line holds the process-controlled comm, so a "Uid:" inside it
// must not be mistaken for the real Uid: line.
TEST(ParseStatusUid, UidInsideNameLineIsIgnored)
{
    const std::string text =
        "Name:\tx Uid: 0\nUmask:\t0022\nUid:\t1000\t1000\t1000\t1000\n";
    unsigned long uid = 0UL;

    ASSERT_EQ(taz_proc_parse_status_uid(text.data(), text.size(), &uid), 1);
    EXPECT_EQ(uid, 1000UL);
}

TEST(ParseStatusUid, UidOnlyInsideNameLineFails)
{
    const std::string text = "Name:\tUid:\nGid:\t1000\n";
    unsigned long uid = 0UL;

    EXPECT_EQ(taz_proc_parse_status_uid(text.data(), text.size(), &uid), 0);
}

TEST(ParseStatusUid, NoUidLineFails)
{
    const std::string text = "Name:\tbash\nGid:\t1000\n";
    unsigned long uid = 0UL;

    EXPECT_EQ(taz_proc_parse_status_uid(text.data(), text.size(), &uid), 0);
}

TEST(ParseStatusUid, UidLineWithNoDigitsFails)
{
    const std::string text = "Uid:\n";
    unsigned long uid = 0UL;

    EXPECT_EQ(taz_proc_parse_status_uid(text.data(), text.size(), &uid), 0);
}

// ---------------------------------------------------------------------------
// taz_proc_parse_btime
// ---------------------------------------------------------------------------

TEST(ParseBtime, BtimeLineParses)
{
    const std::string text = "cpu  1 2 3\nbtime 1700000000\nprocesses 5\n";
    uint64_t btime = 0U;

    ASSERT_EQ(taz_proc_parse_btime(text.data(), text.size(), &btime), 1);
    EXPECT_EQ(btime, 1700000000ULL);
}

TEST(ParseBtime, BtimeMidLineIsIgnored)
{
    const std::string text = "cpu  1 2 btime 5\nbtime 1700000000\n";
    uint64_t btime = 0U;

    ASSERT_EQ(taz_proc_parse_btime(text.data(), text.size(), &btime), 1);
    EXPECT_EQ(btime, 1700000000ULL);
}

TEST(ParseBtime, NoBtimeFails)
{
    const std::string text = "cpu  1 2 3\nprocesses 5\n";
    uint64_t btime = 0U;

    EXPECT_EQ(taz_proc_parse_btime(text.data(), text.size(), &btime), 0);
}

// ---------------------------------------------------------------------------
// taz_proc_state_word
// ---------------------------------------------------------------------------

TEST(StateWord, EveryKnownLetterMapsToItsWord)
{
    EXPECT_STREQ(taz_proc_state_word('R'), "running");
    EXPECT_STREQ(taz_proc_state_word('S'), "sleeping");
    EXPECT_STREQ(taz_proc_state_word('D'), "disk-sleep");
    EXPECT_STREQ(taz_proc_state_word('T'), "stopped");
    EXPECT_STREQ(taz_proc_state_word('t'), "tracing-stop");
    EXPECT_STREQ(taz_proc_state_word('Z'), "zombie");
    EXPECT_STREQ(taz_proc_state_word('X'), "dead");
    EXPECT_STREQ(taz_proc_state_word('x'), "dead");
    EXPECT_STREQ(taz_proc_state_word('K'), "wake-kill");
    EXPECT_STREQ(taz_proc_state_word('W'), "waking");
    EXPECT_STREQ(taz_proc_state_word('P'), "parked");
    EXPECT_STREQ(taz_proc_state_word('I'), "idle");
}

TEST(StateWord, UnknownLetterMapsToUnknown)
{
    EXPECT_STREQ(taz_proc_state_word('?'), "unknown");
}

// ---------------------------------------------------------------------------
// taz_proc_cpu_percent
// ---------------------------------------------------------------------------

TEST(CpuPercent, OneFifthOfACpuOverTwentySecondsIsFiftyPercent)
{
    EXPECT_FLOAT_EQ(taz_proc_cpu_percent(1000U, 100U, 20.0), 50.0F);
}

TEST(CpuPercent, CanExceedOneHundredPercent)
{
    EXPECT_FLOAT_EQ(taz_proc_cpu_percent(300U, 100U, 1.0), 300.0F);
}

TEST(CpuPercent, ZeroElapsedIsZero)
{
    EXPECT_FLOAT_EQ(taz_proc_cpu_percent(1000U, 100U, 0.0), 0.0F);
}

TEST(CpuPercent, NegativeElapsedIsZero)
{
    EXPECT_FLOAT_EQ(taz_proc_cpu_percent(1000U, 100U, -5.0), 0.0F);
}

// ---------------------------------------------------------------------------
// taz_proc_stat_is_exited
// ---------------------------------------------------------------------------

TEST(StatIsExited, SleepingWithMatchingStarttimeIsNotExited)
{
    taz_proc_stat_t st{};
    st.state = 'S';
    st.starttime = 55555U;

    EXPECT_EQ(taz_proc_stat_is_exited(&st, 55555U), 0);
}

TEST(StatIsExited, ZombieIsExited)
{
    taz_proc_stat_t st{};
    st.state = 'Z';

    EXPECT_EQ(taz_proc_stat_is_exited(&st, 0U), 1);
}

TEST(StatIsExited, DeadUppercaseXIsExited)
{
    taz_proc_stat_t st{};
    st.state = 'X';

    EXPECT_EQ(taz_proc_stat_is_exited(&st, 0U), 1);
}

TEST(StatIsExited, DeadLowercaseXIsExited)
{
    taz_proc_stat_t st{};
    st.state = 'x';

    EXPECT_EQ(taz_proc_stat_is_exited(&st, 0U), 1);
}

TEST(StatIsExited, RunningWithNoPriorStarttimeIsNotExited)
{
    taz_proc_stat_t st{};
    st.state = 'R';
    st.starttime = 123U;

    EXPECT_EQ(taz_proc_stat_is_exited(&st, 0U), 0);
}

TEST(StatIsExited, RunningWithChangedStarttimeIsExited)
{
    taz_proc_stat_t st{};
    st.state = 'R';
    st.starttime = 6U;

    EXPECT_EQ(taz_proc_stat_is_exited(&st, 5U), 1);
}

// ---------------------------------------------------------------------------
// taz_proc_ticks_to_ns
// ---------------------------------------------------------------------------

TEST(TicksToNs, HundredTicksAtHundredPerSecondIsOneSecond)
{
    EXPECT_EQ(taz_proc_ticks_to_ns(100U, 100U), 1000000000ULL);
}

TEST(TicksToNs, ZeroTicksIsZero)
{
    EXPECT_EQ(taz_proc_ticks_to_ns(0U, 100U), 0ULL);
}

TEST(TicksToNs, ZeroTicksPerSecondFallsBackToHundred)
{
    EXPECT_EQ(taz_proc_ticks_to_ns(1U, 0U), 10000000ULL);
}

// 1e11 ticks * 1e9 would wrap a uint64_t; the result (1e18 ns) does not.
TEST(TicksToNs, LargeTickCountDoesNotWrap)
{
    EXPECT_EQ(taz_proc_ticks_to_ns(100000000050ULL, 100U),
              1000000000500000000ULL);
}

// ---------------------------------------------------------------------------
// taz_proc_cmdline_to_string
// ---------------------------------------------------------------------------

namespace
{

// Converts buf (NUL-separated argv bytes) into an outsize-byte buffer and
// returns the result as a std::string.
std::string CmdlineToString(const std::vector<uint8_t> &buf, size_t outsize)
{
    std::vector<char> out(outsize == 0U ? 1U : outsize);
    taz_proc_cmdline_to_string(buf.data(), buf.size(), out.data(), outsize);
    return (outsize == 0U) ? std::string() : std::string(out.data());
}

} // namespace

TEST(CmdlineToString, NulSeparatedArgsJoinWithSingleSpaces)
{
    const std::vector<uint8_t> buf = {'p', 'y', 't', 'h', 'o', 'n', '3',
                                      0,   '-', 'c', 0,   'x', 0};

    EXPECT_EQ(CmdlineToString(buf, 64U), "python3 -c x");
}

TEST(CmdlineToString, EmptyBufferProducesEmptyString)
{
    const std::vector<uint8_t> buf;

    EXPECT_EQ(CmdlineToString(buf, 64U), "");
}

TEST(CmdlineToString, SingleTrailingNulProducesEmptyString)
{
    const std::vector<uint8_t> buf = {0};

    EXPECT_EQ(CmdlineToString(buf, 64U), "");
}

TEST(CmdlineToString, InteriorEmptyArgProducesTwoSpacesNotCollapsed)
{
    const std::vector<uint8_t> buf = {'a', 0, 0, 'b', 0};

    EXPECT_EQ(CmdlineToString(buf, 64U), "a  b");
}

TEST(CmdlineToString, InvalidByteIsReplaced)
{
    const std::vector<uint8_t> buf = {'a', 0xFFU, 'b', 0};

    EXPECT_EQ(CmdlineToString(buf, 64U), "a\xEF\xBF\xBD"
                                         "b");
}

TEST(CmdlineToString, LongAsciiInputTruncatesToOutsizeMinusOne)
{
    const std::vector<uint8_t> buf(5000U, static_cast<uint8_t>('a'));

    const std::string result = CmdlineToString(buf, 4096U);
    EXPECT_EQ(result.size(), 4095U);
    EXPECT_EQ(result, std::string(4095U, 'a'));
}

TEST(CmdlineToString, TruncationNeverSplitsAMultiByteCodepoint)
{
    // Repeating 4-byte groups ('a' + the 3-byte EUR SIGN U+20AC): 5000 bytes
    // total, no trailing NUL. Into a 4096-byte buffer (capacity 4095):
    // 1023 whole groups fit in 4092 bytes; the next byte ('a') fits in the
    // remaining 3; the EUR SIGN after it needs 3 bytes but only 2 remain,
    // so it is dropped whole rather than split.
    const std::string unit = "a\xE2\x82\xAC";
    std::string pattern;
    while (pattern.size() < 5000U)
    {
        pattern += unit;
    }
    pattern.resize(5000U);
    const std::vector<uint8_t> buf(pattern.begin(), pattern.end());

    std::string expected;
    for (int i = 0; i < 1023; i++)
    {
        expected += unit;
    }
    expected += "a";

    const std::string result = CmdlineToString(buf, 4096U);
    EXPECT_EQ(result.size(), 4093U);
    EXPECT_EQ(result, expected);
}

} // namespace

#endif // !_WIN32
