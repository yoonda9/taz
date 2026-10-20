// Unit tests for cmdline.c: taz_cmdline_build, the pure Windows
// command-line quoter shared by taz_platform_reexec and the
// CreateProcessAsUserW spawn path. One TEST per quoting rule.

#include <cstddef>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "taz/cmdline.h"

namespace
{

// Builds a char *const argv[] (NULL-terminated) from a list of strings,
// keeping the backing storage alive for the caller - mirrors
// test_restart_args.cpp's Argv helper.
class Argv
{
  public:
    explicit Argv(std::vector<std::string> tokens) : tokens_(std::move(tokens))
    {
        for (auto &t : tokens_)
        {
            ptrs_.push_back(t.data());
        }
        ptrs_.push_back(nullptr);
    }

    char *const *data()
    {
        return ptrs_.data();
    }

  private:
    std::vector<std::string> tokens_;
    std::vector<char *> ptrs_;
};

} // namespace

TEST(Cmdline, Q1PlainWordIsAppendedBare)
{
    Argv argv({"hello"});
    char out[64];

    ASSERT_EQ(taz_cmdline_build(argv.data(), out, sizeof(out)), 0);
    EXPECT_STREQ(out, "hello");
}

TEST(Cmdline, Q2SpaceInsideNeedsQuoting)
{
    Argv argv({"hello world"});
    char out[64];

    ASSERT_EQ(taz_cmdline_build(argv.data(), out, sizeof(out)), 0);
    EXPECT_STREQ(out, R"("hello world")");
}

TEST(Cmdline, Q3SingleQuoteCharIsEscaped)
{
    Argv argv({R"(hello"world)"});
    char out[64];

    ASSERT_EQ(taz_cmdline_build(argv.data(), out, sizeof(out)), 0);
    EXPECT_STREQ(out, R"("hello\"world")");
}

TEST(Cmdline, Q4TwoAdjacentQuoteCharsAreEachEscaped)
{
    Argv argv({R"(hello""world)"});
    char out[64];

    ASSERT_EQ(taz_cmdline_build(argv.data(), out, sizeof(out)), 0);
    EXPECT_STREQ(out, R"("hello\"\"world")");
}

TEST(Cmdline, Q5LoneBackslashNeedsNoEscaping)
{
    Argv argv({R"(hello\world)"});
    char out[64];

    ASSERT_EQ(taz_cmdline_build(argv.data(), out, sizeof(out)), 0);
    EXPECT_STREQ(out, R"(hello\world)");
}

TEST(Cmdline, Q6DoubleBackslashNeedsNoEscaping)
{
    Argv argv({R"(hello\\world)"});
    char out[64];

    ASSERT_EQ(taz_cmdline_build(argv.data(), out, sizeof(out)), 0);
    EXPECT_STREQ(out, R"(hello\\world)");
}

TEST(Cmdline, Q7BackslashBeforeQuoteIsTripled)
{
    Argv argv({R"(hello\"world)"});
    char out[64];

    ASSERT_EQ(taz_cmdline_build(argv.data(), out, sizeof(out)), 0);
    EXPECT_STREQ(out, R"("hello\\\"world")");
}

TEST(Cmdline, Q8TwoBackslashesBeforeQuoteBecomeFive)
{
    Argv argv({R"(hello\\"world)"});
    char out[64];

    ASSERT_EQ(taz_cmdline_build(argv.data(), out, sizeof(out)), 0);
    EXPECT_STREQ(out, R"("hello\\\\\"world")");
}

TEST(Cmdline, Q9TrailingBackslashIsDoubledBeforeTheClosingQuote)
{
    Argv argv({R"(hello world\)"});
    char out[64];

    ASSERT_EQ(taz_cmdline_build(argv.data(), out, sizeof(out)), 0);
    EXPECT_STREQ(out, R"("hello world\\")");
}

TEST(Cmdline, Q10EmptyElementBecomesAnEmptyQuotedPair)
{
    Argv argv({""});
    char out[64];

    ASSERT_EQ(taz_cmdline_build(argv.data(), out, sizeof(out)), 0);
    EXPECT_STREQ(out, R"("")");
}

TEST(Cmdline, Q11TabInsideIsQuotedButNotEscaped)
{
    Argv argv({"a\tb"});
    char out[64];

    ASSERT_EQ(taz_cmdline_build(argv.data(), out, sizeof(out)), 0);
    EXPECT_STREQ(out, "\"a\tb\"");
}

TEST(Cmdline, Q12BufferTooSmallFailsWithoutWritingPastOutSize)
{
    // "hello world" (11 bytes) plus the NUL does not fit in kOutSize -
    // elements are joined by one space just like a successful build.
    Argv argv({"hello", "world"});
    constexpr size_t kOutSize = 8;
    constexpr size_t kPad = 8;
    std::vector<char> buf(kOutSize + kPad, '\xAA');

    EXPECT_EQ(taz_cmdline_build(argv.data(), buf.data(), kOutSize), -1);
    for (size_t i = kOutSize; i < buf.size(); i++)
    {
        EXPECT_EQ(static_cast<unsigned char>(buf[i]), 0xAAU) << "byte " << i;
    }
}

// A backslash followed by two ordinary characters inside a quoted element
// (any path with a space, e.g. under "C:\Program Files") once left the
// backslash count wrapped to SIZE_MAX, so the next character wrote past the
// buffer. The padding must stay untouched and the result be exact.
TEST(Cmdline, PathWithSpaceKeepsItsBackslashesAndStaysInBounds)
{
    Argv argv({R"(C:\Program Files\taz\tazd.exe)", "--port", "7777"});
    constexpr size_t kOutSize = 64;
    constexpr size_t kPad = 64;
    std::vector<char> buf(kOutSize + kPad, '\xAA');

    ASSERT_EQ(taz_cmdline_build(argv.data(), buf.data(), kOutSize), 0);
    EXPECT_STREQ(buf.data(), R"("C:\Program Files\taz\tazd.exe" --port 7777)");
    for (size_t i = kOutSize; i < buf.size(); i++)
    {
        EXPECT_EQ(static_cast<unsigned char>(buf[i]), 0xAAU) << "byte " << i;
    }
}

TEST(Cmdline, QuotedPathInsideAnElementIsEscaped)
{
    Argv argv({"/c", R"(echo ran> "C:\Users\x\ran.txt")"});
    char out[128];

    ASSERT_EQ(taz_cmdline_build(argv.data(), out, sizeof(out)), 0);
    EXPECT_STREQ(out, R"(/c "echo ran> \"C:\Users\x\ran.txt\"")");
}
