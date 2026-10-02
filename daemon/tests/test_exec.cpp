// Unit tests for exec.c/h: the output capture buffer and the taz_exec_start
// spawn engine.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <gtest/gtest.h>
#include <uv.h>

#include "taz/exec.h"

namespace
{

// Lets the env-merge tests below use this same binary as the spawned child:
// if TAZ_TEST_ECHO_ENV_VAR is set in the child's environment, print the
// value of the env var it names and exit immediately, before main()/gtest
// ever runs — static init order makes this safe regardless of what argv or
// gtest's own startup would otherwise do with a re-spawned test binary.
struct EchoEnvOnStartup
{
    EchoEnvOnStartup() noexcept
    {
        const char *var_name = std::getenv("TAZ_TEST_ECHO_ENV_VAR");
        if (var_name == nullptr)
        {
            return;
        }
        const char *value = std::getenv(var_name);
        (void)std::fputs(value != nullptr ? value : "", stdout);
        (void)std::fflush(stdout);
        std::exit(0);
    }
};

const EchoEnvOnStartup echo_env_on_startup{};

void set_parent_env(const char *name, const char *value)
{
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

void unset_parent_env(const char *name)
{
#ifdef _WIN32
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}

class ExecCaptureTest : public ::testing::Test
{
  protected:
    void TearDown() override
    {
        taz_exec_capture_free(&cap_);
    }

    taz_exec_capture_t *Cap()
    {
        return &cap_;
    }

  private:
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
    taz_exec_capture_init(Cap(), 10);

    append_str(Cap(), TAZ_EXEC_STREAM_OUT, "abc");

    EXPECT_EQ(out_str(*Cap()), "abc");
    EXPECT_EQ(Cap()->err_len, 0U);
    EXPECT_FALSE(Cap()->truncated);
}

TEST_F(ExecCaptureTest, ExactlyAtCapIsNotTruncated)
{
    taz_exec_capture_init(Cap(), 10);

    append_str(Cap(), TAZ_EXEC_STREAM_OUT, "0123456789");

    EXPECT_EQ(Cap()->out_len, 10U);
    EXPECT_FALSE(Cap()->truncated);
}

TEST_F(ExecCaptureTest, OverCapTruncatesAndStopsGrowth)
{
    taz_exec_capture_init(Cap(), 10);

    append_str(Cap(), TAZ_EXEC_STREAM_OUT, "0123456789ABCDE");

    EXPECT_EQ(Cap()->out_len, 10U);
    EXPECT_EQ(out_str(*Cap()), "0123456789");
    EXPECT_TRUE(Cap()->truncated);
}

TEST_F(ExecCaptureTest, CapIsCombinedAcrossOutAndErr)
{
    taz_exec_capture_init(Cap(), 10);

    append_str(Cap(), TAZ_EXEC_STREAM_OUT, "123456");
    append_str(Cap(), TAZ_EXEC_STREAM_ERR, "abcdef");

    EXPECT_EQ(Cap()->out_len, 6U);
    EXPECT_EQ(Cap()->err_len, 4U);
    EXPECT_EQ(out_str(*Cap()), "123456");
    EXPECT_EQ(err_str(*Cap()), "abcd");
    EXPECT_TRUE(Cap()->truncated);
}

TEST_F(ExecCaptureTest, AppendAfterTruncationLeavesLengthsUnchanged)
{
    taz_exec_capture_init(Cap(), 10);

    append_str(Cap(), TAZ_EXEC_STREAM_OUT, "0123456789ABCDE");
    ASSERT_TRUE(Cap()->truncated);
    const size_t out_len_before = Cap()->out_len;
    const size_t err_len_before = Cap()->err_len;

    append_str(Cap(), TAZ_EXEC_STREAM_OUT, "more");
    append_str(Cap(), TAZ_EXEC_STREAM_ERR, "more");

    EXPECT_EQ(Cap()->out_len, out_len_before);
    EXPECT_EQ(Cap()->err_len, err_len_before);
}

TEST_F(ExecCaptureTest, ZeroLengthAppendIsNoop)
{
    taz_exec_capture_init(Cap(), 10);

    taz_exec_capture_append(Cap(), TAZ_EXEC_STREAM_OUT, "", 0);

    EXPECT_EQ(Cap()->out_len, 0U);
    EXPECT_EQ(Cap()->err_len, 0U);
    EXPECT_FALSE(Cap()->truncated);
}

TEST_F(ExecCaptureTest, FreeOfUntouchedCaptureIsSafe)
{
    taz_exec_capture_init(Cap(), 10);

    taz_exec_capture_free(Cap());
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
    const int rc = uv_exepath(buf, &size);
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

    uv_loop_t *Loop()
    {
        return &loop_;
    }

    const std::string &Exe() const
    {
        return exe_;
    }

  private:
    uv_loop_t loop_{};
    std::string exe_;
};

TEST_F(ExecSpawnTest, SuccessfulSpawnCallsOnDoneOnceWithOutput)
{
    const char *const args[] = {"--gtest_filter=NoSuchSuite.*"};
    taz_exec_spec_t spec{};
    spec.file = Exe().c_str();
    spec.args = args;
    spec.args_count = 1;
    spec.max_output_bytes = 1U << 20;

    ExecOutcome outcome;
    taz_exec_t *x = nullptr;
    ASSERT_EQ(taz_exec_start(Loop(), &spec, record_outcome, &outcome, &x), 0);

    ASSERT_EQ(uv_run(Loop(), UV_RUN_DEFAULT), 0);

    EXPECT_TRUE(outcome.called);
    EXPECT_EQ(outcome.exit_status, 0);
    EXPECT_FALSE(outcome.out.empty());
    EXPECT_FALSE(outcome.timed_out);
    EXPECT_FALSE(outcome.truncated);
}

TEST_F(ExecSpawnTest, OutputCapTruncatesButChildIsStillReaped)
{
    const char *const args[] = {"--gtest_filter=NoSuchSuite.*"};
    taz_exec_spec_t spec{};
    spec.file = Exe().c_str();
    spec.args = args;
    spec.args_count = 1;
    spec.max_output_bytes = 5;

    ExecOutcome outcome;
    taz_exec_t *x = nullptr;
    ASSERT_EQ(taz_exec_start(Loop(), &spec, record_outcome, &outcome, &x), 0);

    ASSERT_EQ(uv_run(Loop(), UV_RUN_DEFAULT), 0);

    EXPECT_TRUE(outcome.called);
    EXPECT_TRUE(outcome.truncated);
    EXPECT_LE(outcome.out.size() + outcome.err.size(), 5U);
}

TEST_F(ExecSpawnTest, NonexistentFileReturnsErrorWithoutCallback)
{
    const std::string missing = Exe() + "-taz-test-does-not-exist";
    taz_exec_spec_t spec{};
    spec.file = missing.c_str();
    spec.max_output_bytes = 1024;

    ExecOutcome outcome;
    taz_exec_t *x = nullptr;
    EXPECT_NE(taz_exec_start(Loop(), &spec, record_outcome, &outcome, &x), 0);
    EXPECT_FALSE(outcome.called);
}

TEST_F(ExecSpawnTest, NonexistentCwdReturnsError)
{
    taz_exec_spec_t spec{};
    spec.file = Exe().c_str();
    spec.cwd = "/taz-test-cwd-does-not-exist";
    spec.max_output_bytes = 1024;

    ExecOutcome outcome;
    taz_exec_t *x = nullptr;
    EXPECT_NE(taz_exec_start(Loop(), &spec, record_outcome, &outcome, &x), 0);
    EXPECT_FALSE(outcome.called);
}

// --- env merge --------------------------------------------------------
//
// spec.env entries always include a TAZ_TEST_ECHO_ENV_VAR entry naming the
// variable the child should echo back on stdout (see EchoEnvOnStartup
// above), so these tests observe the merged child environment without
// depending on any external echo/printenv binary.

TEST_F(ExecSpawnTest, ExtraVarIsAddedToChildEnv)
{
    taz_v1_KeyValue env[] = {
        {"TAZ_TEST_ECHO_ENV_VAR", "TAZ_TEST_ADDED_VAR"},
        {"TAZ_TEST_ADDED_VAR", "added_value"},
    };
    taz_exec_spec_t spec{};
    spec.file = Exe().c_str();
    spec.env = env;
    spec.env_count = 2;
    spec.max_output_bytes = 1024;

    ExecOutcome outcome;
    taz_exec_t *x = nullptr;
    ASSERT_EQ(taz_exec_start(Loop(), &spec, record_outcome, &outcome, &x), 0);

    ASSERT_EQ(uv_run(Loop(), UV_RUN_DEFAULT), 0);

    EXPECT_TRUE(outcome.called);
    EXPECT_EQ(outcome.out, "added_value");
}

TEST_F(ExecSpawnTest, ExtraVarOverridesSameNameEntry)
{
    set_parent_env("TAZ_TEST_OVERRIDE_VAR", "original_value");

    taz_v1_KeyValue env[] = {
        {"TAZ_TEST_ECHO_ENV_VAR", "TAZ_TEST_OVERRIDE_VAR"},
        {"TAZ_TEST_OVERRIDE_VAR", "overridden_value"},
    };
    taz_exec_spec_t spec{};
    spec.file = Exe().c_str();
    spec.env = env;
    spec.env_count = 2;
    spec.max_output_bytes = 1024;

    ExecOutcome outcome;
    taz_exec_t *x = nullptr;
    ASSERT_EQ(taz_exec_start(Loop(), &spec, record_outcome, &outcome, &x), 0);

    ASSERT_EQ(uv_run(Loop(), UV_RUN_DEFAULT), 0);

    unset_parent_env("TAZ_TEST_OVERRIDE_VAR");

    EXPECT_TRUE(outcome.called);
    EXPECT_EQ(outcome.out, "overridden_value");
}

#ifndef _WIN32
// POSIX env names are case-sensitive (unlike Windows, where this same
// request would be an override — see .ralph/specs/step-5-command-exec/
// plan.md:91-92), so a differently-cased extra entry must be added
// alongside the original rather than replacing it.
TEST_F(ExecSpawnTest, DifferentlyCasedNameIsNotTreatedAsOverrideOnPosix)
{
    set_parent_env("TAZ_TEST_CASE_VAR", "original_value");

    taz_v1_KeyValue env[] = {
        {"TAZ_TEST_ECHO_ENV_VAR", "TAZ_TEST_CASE_VAR"},
        {"taz_test_case_var", "should_not_override"},
    };
    taz_exec_spec_t spec{};
    spec.file = Exe().c_str();
    spec.env = env;
    spec.env_count = 2;
    spec.max_output_bytes = 1024;

    ExecOutcome outcome;
    taz_exec_t *x = nullptr;
    ASSERT_EQ(taz_exec_start(Loop(), &spec, record_outcome, &outcome, &x), 0);

    ASSERT_EQ(uv_run(Loop(), UV_RUN_DEFAULT), 0);

    unset_parent_env("TAZ_TEST_CASE_VAR");

    EXPECT_TRUE(outcome.called);
    EXPECT_EQ(outcome.out, "original_value");
}
#endif

TEST_F(ExecSpawnTest, UnrelatedDaemonEnvVarsSurviveUnchanged)
{
    const char *path_value = std::getenv("PATH");
    ASSERT_NE(path_value, nullptr);

    taz_v1_KeyValue env[] = {
        {"TAZ_TEST_ECHO_ENV_VAR", "PATH"},
    };
    taz_exec_spec_t spec{};
    spec.file = Exe().c_str();
    spec.env = env;
    spec.env_count = 1;
    spec.max_output_bytes = 1U << 20;

    ExecOutcome outcome;
    taz_exec_t *x = nullptr;
    ASSERT_EQ(taz_exec_start(Loop(), &spec, record_outcome, &outcome, &x), 0);

    ASSERT_EQ(uv_run(Loop(), UV_RUN_DEFAULT), 0);

    EXPECT_TRUE(outcome.called);
    EXPECT_EQ(outcome.out, path_value);
}

// --- timeout / tree kill / cancel -----------------------------------------
//
// taz_test_sleeper (built from sleeper.c) sleeps for 60 s with no shell
// involved, so these tests have a child that would still be running long
// after the assertions below if the kill path were broken.

TEST_F(ExecSpawnTest, TimeoutKillsChildAndSetsTimedOut)
{
    taz_exec_spec_t spec{};
    spec.file = TAZ_TEST_SLEEPER_PATH;
    spec.max_output_bytes = 1024;
    spec.timeout_ms = 200;

    ExecOutcome outcome;
    taz_exec_t *x = nullptr;
    ASSERT_EQ(taz_exec_start(Loop(), &spec, record_outcome, &outcome, &x), 0);

    auto start = std::chrono::steady_clock::now();
    ASSERT_EQ(uv_run(Loop(), UV_RUN_DEFAULT), 0);
    auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_TRUE(outcome.called);
    EXPECT_TRUE(outcome.timed_out);
    EXPECT_FALSE(outcome.cancelled);
    EXPECT_LT(elapsed, std::chrono::seconds(10));
}

TEST_F(ExecSpawnTest, CancelKillsChildAndSetsCancelled)
{
    taz_exec_spec_t spec{};
    spec.file = TAZ_TEST_SLEEPER_PATH;
    spec.max_output_bytes = 1024;

    ExecOutcome outcome;
    taz_exec_t *x = nullptr;
    ASSERT_EQ(taz_exec_start(Loop(), &spec, record_outcome, &outcome, &x), 0);

    taz_exec_cancel(x);

    auto start = std::chrono::steady_clock::now();
    ASSERT_EQ(uv_run(Loop(), UV_RUN_DEFAULT), 0);
    auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_TRUE(outcome.called);
    EXPECT_TRUE(outcome.cancelled);
    EXPECT_FALSE(outcome.timed_out);
    EXPECT_LT(elapsed, std::chrono::seconds(10));
}

TEST_F(ExecSpawnTest, ZeroTimeoutMeansChildRunsToNormalCompletion)
{
    const char *const args[] = {"--gtest_filter=NoSuchSuite.*"};
    taz_exec_spec_t spec{};
    spec.file = Exe().c_str();
    spec.args = args;
    spec.args_count = 1;
    spec.max_output_bytes = 1U << 20;
    spec.timeout_ms = 0;

    ExecOutcome outcome;
    taz_exec_t *x = nullptr;
    ASSERT_EQ(taz_exec_start(Loop(), &spec, record_outcome, &outcome, &x), 0);

    ASSERT_EQ(uv_run(Loop(), UV_RUN_DEFAULT), 0);

    EXPECT_TRUE(outcome.called);
    EXPECT_EQ(outcome.exit_status, 0);
    EXPECT_FALSE(outcome.timed_out);
}

} // namespace
