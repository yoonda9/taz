// Unit tests for the exec output capture buffer (exec.c/h).

#include <cstring>
#include <string>

#include <gtest/gtest.h>

#include "taz/exec.h"

namespace
{

class ExecCaptureTest : public ::testing::Test
{
  protected:
    void TearDown() override
    {
        taz_exec_capture_free(&cap_);
    }

    taz_exec_capture_t cap_{};
};

void append_str(taz_exec_capture_t *cap, taz_exec_stream_t which,
                const std::string &s)
{
    taz_exec_capture_append(cap, which, s.data(), s.size());
}

std::string out_str(const taz_exec_capture_t &cap)
{
    return std::string(reinterpret_cast<const char *>(cap.out), cap.out_len);
}

std::string err_str(const taz_exec_capture_t &cap)
{
    return std::string(reinterpret_cast<const char *>(cap.err), cap.err_len);
}

TEST_F(ExecCaptureTest, UnderCapKeepsAllBytes)
{
    taz_exec_capture_init(&cap_, 10);

    append_str(&cap_, TAZ_EXEC_STREAM_OUT, "abc");

    EXPECT_EQ(out_str(cap_), "abc");
    EXPECT_EQ(cap_.err_len, 0U);
    EXPECT_FALSE(cap_.truncated);
}

TEST_F(ExecCaptureTest, ExactlyAtCapIsNotTruncated)
{
    taz_exec_capture_init(&cap_, 10);

    append_str(&cap_, TAZ_EXEC_STREAM_OUT, "0123456789");

    EXPECT_EQ(cap_.out_len, 10U);
    EXPECT_FALSE(cap_.truncated);
}

TEST_F(ExecCaptureTest, OverCapTruncatesAndStopsGrowth)
{
    taz_exec_capture_init(&cap_, 10);

    append_str(&cap_, TAZ_EXEC_STREAM_OUT, "0123456789ABCDE");

    EXPECT_EQ(cap_.out_len, 10U);
    EXPECT_EQ(out_str(cap_), "0123456789");
    EXPECT_TRUE(cap_.truncated);
}

TEST_F(ExecCaptureTest, CapIsCombinedAcrossOutAndErr)
{
    taz_exec_capture_init(&cap_, 10);

    append_str(&cap_, TAZ_EXEC_STREAM_OUT, "123456");
    append_str(&cap_, TAZ_EXEC_STREAM_ERR, "abcdef");

    EXPECT_EQ(cap_.out_len, 6U);
    EXPECT_EQ(cap_.err_len, 4U);
    EXPECT_EQ(out_str(cap_), "123456");
    EXPECT_EQ(err_str(cap_), "abcd");
    EXPECT_TRUE(cap_.truncated);
}

TEST_F(ExecCaptureTest, AppendAfterTruncationLeavesLengthsUnchanged)
{
    taz_exec_capture_init(&cap_, 10);

    append_str(&cap_, TAZ_EXEC_STREAM_OUT, "0123456789ABCDE");
    ASSERT_TRUE(cap_.truncated);
    size_t out_len_before = cap_.out_len;
    size_t err_len_before = cap_.err_len;

    append_str(&cap_, TAZ_EXEC_STREAM_OUT, "more");
    append_str(&cap_, TAZ_EXEC_STREAM_ERR, "more");

    EXPECT_EQ(cap_.out_len, out_len_before);
    EXPECT_EQ(cap_.err_len, err_len_before);
}

TEST_F(ExecCaptureTest, ZeroLengthAppendIsNoop)
{
    taz_exec_capture_init(&cap_, 10);

    taz_exec_capture_append(&cap_, TAZ_EXEC_STREAM_OUT, "", 0);

    EXPECT_EQ(cap_.out_len, 0U);
    EXPECT_EQ(cap_.err_len, 0U);
    EXPECT_FALSE(cap_.truncated);
}

TEST_F(ExecCaptureTest, FreeOfUntouchedCaptureIsSafe)
{
    taz_exec_capture_init(&cap_, 10);

    taz_exec_capture_free(&cap_);
}

} // namespace
