// Unit tests for exec.c/h: the output capture buffer and the taz_exec_start
// spawn engine.

#include <cstring>
#include <string>

#include <gtest/gtest.h>
#include <uv.h>

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

// --- taz_exec_start -------------------------------------------------------
//
// The cross-platform child under test is the test binary itself, re-invoked
// with a gtest filter that matches nothing: it runs to completion almost
// instantly, exits 0, and still prints a handful of lines to stdout.

struct ExecOutcome
{
    bool called = false;
    int64_t exit_status = -1;
    int term_signal = 0;
    std::string out;
    std::string err;
    bool timed_out = false;
    bool truncated = false;
    bool cancelled = false;
};

void record_outcome(const taz_exec_result_t *result, void *arg)
{
    auto *outcome = static_cast<ExecOutcome *>(arg);

    outcome->called = true;
    outcome->exit_status = result->exit_status;
    outcome->term_signal = result->term_signal;
    outcome->out.assign(reinterpret_cast<const char *>(result->out),
                        result->out_len);
    outcome->err.assign(reinterpret_cast<const char *>(result->err),
                        result->err_len);
    outcome->timed_out = result->timed_out;
    outcome->truncated = result->truncated;
    outcome->cancelled = result->cancelled;
}

std::string self_exe_path()
{
    char buf[4096];
    size_t size = sizeof(buf);
    int rc = uv_exepath(buf, &size);
    if (rc != 0)
    {
        return std::string();
    }
    return std::string(buf, size);
}

class ExecSpawnTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        ASSERT_EQ(uv_loop_init(&loop_), 0);
        exe_ = self_exe_path();
        ASSERT_FALSE(exe_.empty());
    }

    void TearDown() override
    {
        EXPECT_EQ(uv_run(&loop_, UV_RUN_DEFAULT), 0);
        EXPECT_EQ(uv_loop_close(&loop_), 0);
    }

    uv_loop_t loop_{};
    std::string exe_;
};

TEST_F(ExecSpawnTest, SuccessfulSpawnCallsOnDoneOnceWithOutput)
{
    const char *args[] = {"--gtest_filter=NoSuchSuite.*"};
    taz_exec_spec_t spec{};
    spec.file = exe_.c_str();
    spec.args = args;
    spec.args_count = 1;
    spec.max_output_bytes = 1U << 20;

    ExecOutcome outcome;
    taz_exec_t *x = nullptr;
    ASSERT_EQ(taz_exec_start(&loop_, &spec, record_outcome, &outcome, &x), 0);

    ASSERT_EQ(uv_run(&loop_, UV_RUN_DEFAULT), 0);

    EXPECT_TRUE(outcome.called);
    EXPECT_EQ(outcome.exit_status, 0);
    EXPECT_FALSE(outcome.out.empty());
    EXPECT_FALSE(outcome.timed_out);
    EXPECT_FALSE(outcome.truncated);
}

TEST_F(ExecSpawnTest, OutputCapTruncatesButChildIsStillReaped)
{
    const char *args[] = {"--gtest_filter=NoSuchSuite.*"};
    taz_exec_spec_t spec{};
    spec.file = exe_.c_str();
    spec.args = args;
    spec.args_count = 1;
    spec.max_output_bytes = 5;

    ExecOutcome outcome;
    taz_exec_t *x = nullptr;
    ASSERT_EQ(taz_exec_start(&loop_, &spec, record_outcome, &outcome, &x), 0);

    ASSERT_EQ(uv_run(&loop_, UV_RUN_DEFAULT), 0);

    EXPECT_TRUE(outcome.called);
    EXPECT_TRUE(outcome.truncated);
    EXPECT_LE(outcome.out.size() + outcome.err.size(), 5U);
}

TEST_F(ExecSpawnTest, NonexistentFileReturnsErrorWithoutCallback)
{
    std::string missing = exe_ + "-taz-test-does-not-exist";
    taz_exec_spec_t spec{};
    spec.file = missing.c_str();
    spec.max_output_bytes = 1024;

    ExecOutcome outcome;
    taz_exec_t *x = nullptr;
    EXPECT_NE(taz_exec_start(&loop_, &spec, record_outcome, &outcome, &x), 0);
    EXPECT_FALSE(outcome.called);
}

TEST_F(ExecSpawnTest, NonexistentCwdReturnsError)
{
    taz_exec_spec_t spec{};
    spec.file = exe_.c_str();
    spec.cwd = "/taz-test-cwd-does-not-exist";
    spec.max_output_bytes = 1024;

    ExecOutcome outcome;
    taz_exec_t *x = nullptr;
    EXPECT_NE(taz_exec_start(&loop_, &spec, record_outcome, &outcome, &x), 0);
    EXPECT_FALSE(outcome.called);
}

} // namespace
