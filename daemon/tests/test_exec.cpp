// Unit tests for exec.c/h: the output capture buffer and the taz_exec_start
// spawn engine.

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <uv.h>

#ifdef _WIN32
#include <windows.h>

#include "win32_token_test_support.h"
#endif

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

// --- taz_exec_apply_identity ------------------------------------------------
//
// Pure option-building: exercised against a bare uv_process_options_t, no
// spawn involved.

TEST(ExecApplyIdentityTest, SwitchSetAppliesUidGidAndFlagsOnPosix)
{
    taz_exec_spec_t spec{};
    spec.switch_identity = true;
    // uv_uid_t/uv_gid_t is a 1-byte stub on Windows (unused there - RUN_AS
    // is deferred - but this TU still compiles for that target), so this
    // value must fit an unsigned char on every platform.
    spec.uid = 99;
    spec.gid = 99;
    uv_process_options_t options{};

    taz_exec_apply_identity(&options, &spec);

#ifndef _WIN32
    EXPECT_EQ(options.uid, spec.uid);
    EXPECT_EQ(options.gid, spec.gid);
    EXPECT_NE(options.flags & UV_PROCESS_SETUID, 0U);
    EXPECT_NE(options.flags & UV_PROCESS_SETGID, 0U);
#else
    EXPECT_EQ(options.flags, 0U);
#endif
}

TEST(ExecApplyIdentityTest, SwitchClearTouchesNeitherUidGidNorFlags)
{
    taz_exec_spec_t spec{};
    spec.switch_identity = false;
    spec.uid = 99;
    spec.gid = 99;
    uv_process_options_t options{};

    taz_exec_apply_identity(&options, &spec);

    EXPECT_EQ(options.uid, 0U);
    EXPECT_EQ(options.gid, 0U);
    EXPECT_EQ(options.flags, 0U);
}

TEST(ExecApplyIdentityTest, PreservesFlagsAlreadySetOnOptions)
{
    taz_exec_spec_t spec{};
    spec.switch_identity = true;
    uv_process_options_t options{};
    options.flags = UV_PROCESS_DETACHED;

    taz_exec_apply_identity(&options, &spec);

#ifndef _WIN32
    EXPECT_NE(options.flags & UV_PROCESS_DETACHED, 0U);
    EXPECT_NE(options.flags & UV_PROCESS_SETUID, 0U);
#endif
}

// --- taz_exec_build_env ---------------------------------------------------
//
// Pure: builds the child's environment without spawning anything.

namespace
{

// Every NAME=... entry in env for name, matched exactly.
std::vector<std::string> EnvValues(char **env, const char *name)
{
    std::vector<std::string> values;
    const size_t len = std::strlen(name);
    for (size_t i = 0U; env[i] != nullptr; i++)
    {
        if (std::strncmp(env[i], name, len) == 0 && env[i][len] == '=')
        {
            values.emplace_back(env[i] + len + 1U);
        }
    }
    return values;
}

// The daemon's own value for name; empty when it is unset.
std::vector<std::string> DaemonValues(const char *name)
{
    std::vector<char> value(4096);
    size_t size = value.size();
    if (uv_os_getenv(name, value.data(), &size) != 0)
    {
        return {};
    }
    return {std::string(value.data(), size)};
}

} // namespace

TEST(ExecBuildEnvTest, SwitchedIdentitySetsUserLognameAndHomeOnce)
{
    taz_exec_spec_t spec{};
    spec.switch_identity = true;
    (void)std::snprintf(spec.identity_user, sizeof(spec.identity_user), "%s",
                        "alice");
    (void)std::snprintf(spec.identity_home, sizeof(spec.identity_home), "%s",
                        "/home/alice");
    char **env = nullptr;

    ASSERT_EQ(taz_exec_build_env(&spec, &env), 0);

    EXPECT_EQ(EnvValues(env, "USER"), std::vector<std::string>{"alice"});
    EXPECT_EQ(EnvValues(env, "LOGNAME"), std::vector<std::string>{"alice"});
    EXPECT_EQ(EnvValues(env, "HOME"), std::vector<std::string>{"/home/alice"});
    taz_exec_free_env(env);
}

TEST(ExecBuildEnvTest, RequestEnvOverridesTheIdentityVariables)
{
    taz_exec_spec_t spec{};
    spec.switch_identity = true;
    (void)std::snprintf(spec.identity_user, sizeof(spec.identity_user), "%s",
                        "alice");
    (void)std::snprintf(spec.identity_home, sizeof(spec.identity_home), "%s",
                        "/home/alice");
    taz_v1_KeyValue extra{};
    (void)std::snprintf(extra.key, sizeof(extra.key), "%s", "HOME");
    (void)std::snprintf(extra.value, sizeof(extra.value), "%s", "/custom");
    spec.env = &extra;
    spec.env_count = 1U;
    char **env = nullptr;

    ASSERT_EQ(taz_exec_build_env(&spec, &env), 0);

    EXPECT_EQ(EnvValues(env, "HOME"), std::vector<std::string>{"/custom"});
    EXPECT_EQ(EnvValues(env, "USER"), std::vector<std::string>{"alice"});
    taz_exec_free_env(env);
}

TEST(ExecBuildEnvTest, EmptyHomeBecomesRoot)
{
    taz_exec_spec_t spec{};
    spec.switch_identity = true;
    (void)std::snprintf(spec.identity_user, sizeof(spec.identity_user), "%s",
                        "nohome");
    char **env = nullptr;

    ASSERT_EQ(taz_exec_build_env(&spec, &env), 0);

    EXPECT_EQ(EnvValues(env, "HOME"), std::vector<std::string>{"/"});
    taz_exec_free_env(env);
}

TEST(ExecBuildEnvTest, NoIdentityKeepsTheDaemonsOwnValues)
{
    taz_exec_spec_t spec{};
    (void)std::snprintf(spec.identity_user, sizeof(spec.identity_user), "%s",
                        "alice");
    char **env = nullptr;

    ASSERT_EQ(taz_exec_build_env(&spec, &env), 0);

    EXPECT_EQ(EnvValues(env, "USER"), DaemonValues("USER"));
    EXPECT_EQ(EnvValues(env, "HOME"), DaemonValues("HOME"));
    taz_exec_free_env(env);
}

#ifdef _WIN32

namespace
{

// The current process's own value for a wide env var name, converted to
// UTF-8; empty when it is unset. Sized dynamically - PATH under a loaded
// MSVC dev environment routinely runs well past a four-figure character
// count.
std::string OwnEnvVarUtf8(const wchar_t *name)
{
    const DWORD needed_w = GetEnvironmentVariableW(name, nullptr, 0);
    if (needed_w == 0)
    {
        return std::string();
    }
    std::vector<wchar_t> buf(needed_w);
    const DWORD n = GetEnvironmentVariableW(name, buf.data(), needed_w);
    if ((n == 0) || (n >= needed_w))
    {
        return std::string();
    }
    const int needed =
        WideCharToMultiByte(CP_UTF8, 0, buf.data(), static_cast<int>(n),
                            nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(needed), '\0');
    (void)WideCharToMultiByte(CP_UTF8, 0, buf.data(), static_cast<int>(n),
                              out.data(), needed, nullptr, nullptr);
    return out;
}

} // namespace

// These rows duplicate the test process's own token (no second account
// exists on the host yet), so the token's environment block is expected
// to equal the current process's own session values.
class ExecBuildEnvTokenTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        std::string reason;
        token_ = OwnPrimaryToken(&reason);
        if (!token_.valid())
        {
            GTEST_SKIP() << reason;
        }
    }

    HANDLE Token() const
    {
        return token_.get();
    }

  private:
    ScopedHandle token_;
};

TEST_F(ExecBuildEnvTokenTest, BaseComesFromTheTokensOwnEnvironmentBlock)
{
    set_parent_env("TAZ_ONLY_IN_DAEMON", "yes");
    taz_exec_spec_t spec{};
    spec.token = Token();
    char **env = nullptr;

    const int rc = taz_exec_build_env(&spec, &env);
    unset_parent_env("TAZ_ONLY_IN_DAEMON");
    ASSERT_EQ(rc, 0);

    EXPECT_EQ(EnvValues(env, "USERPROFILE"),
              std::vector<std::string>{OwnEnvVarUtf8(L"USERPROFILE")});
    EXPECT_EQ(EnvValues(env, "USERNAME"),
              std::vector<std::string>{OwnEnvVarUtf8(L"USERNAME")});
    EXPECT_TRUE(EnvValues(env, "TAZ_ONLY_IN_DAEMON").empty());
    EXPECT_TRUE(EnvValues(env, "USER").empty());
    EXPECT_TRUE(EnvValues(env, "LOGNAME").empty());
    EXPECT_TRUE(EnvValues(env, "HOME").empty());
    // CreateEnvironmentBlock's machine-wide half doesn't carry SystemRoot on
    // every host; ensure_system_root backfills it (CreateProcessAsUserW's
    // child otherwise dies immediately with STATUS_DLL_NOT_FOUND).
    EXPECT_EQ(EnvValues(env, "SystemRoot"),
              std::vector<std::string>{OwnEnvVarUtf8(L"SystemRoot")});
    taz_exec_free_env(env);
}

TEST_F(ExecBuildEnvTokenTest, RequestEnvOverridesTheTokenBase)
{
    taz_exec_spec_t spec{};
    spec.token = Token();
    taz_v1_KeyValue extra{};
    (void)std::snprintf(extra.key, sizeof(extra.key), "%s", "USERPROFILE");
    (void)std::snprintf(extra.value, sizeof(extra.value), "%s", "C:\\custom");
    spec.env = &extra;
    spec.env_count = 1U;
    char **env = nullptr;

    ASSERT_EQ(taz_exec_build_env(&spec, &env), 0);

    EXPECT_EQ(EnvValues(env, "USERPROFILE"),
              std::vector<std::string>{"C:\\custom"});
    EXPECT_EQ(EnvValues(env, "USERNAME"),
              std::vector<std::string>{OwnEnvVarUtf8(L"USERNAME")});
    taz_exec_free_env(env);
}

#endif // _WIN32

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
    // x is only valid up to uv_run below (on_done's close_all frees it), so
    // taz_exec_as_user must be read now, not after.
    EXPECT_FALSE(taz_exec_as_user(x));

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

// --- a non-NULL token (H8) ---------------------------------------------
//
// On POSIX taz_exec_start still has no token-aware spawn path, so this stays
// purely defensive: close whatever it was handed and refuse it, rather than
// spawn with it ignored or leak it.
//
// On Windows a non-NULL token now enters spawn_as_user (table S2). A handle
// that is not a real token (here, a plain event) makes LoadUserProfileW fail
// before anything is spawned, so the row instead proves: a non-UV_ENOTSUP
// UV_E* code, no callback, nothing spawned, and the token closed once the
// loop has run the close callbacks close_all schedules
// (close_all -> on_handle_closed -> taz_run_as_token_close).

#ifdef _WIN32
TEST_F(ExecSpawnTest, NonNullTokenFailsSpawnAsUserAndClosesTheToken)
{
    HANDLE token = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ASSERT_NE(token, nullptr);

    taz_exec_spec_t spec{};
    spec.file = Exe().c_str();
    spec.token = token;
    spec.max_output_bytes = 1024;

    ExecOutcome outcome;
    taz_exec_t *x = nullptr;
    const int rc = taz_exec_start(Loop(), &spec, record_outcome, &outcome, &x);
    EXPECT_NE(rc, 0);
    EXPECT_NE(rc, UV_ENOTSUP);
    EXPECT_FALSE(outcome.called);
    EXPECT_EQ(x, nullptr);

    ASSERT_EQ(uv_run(Loop(), UV_RUN_DEFAULT), 0);

    DWORD flags = 0;
    EXPECT_FALSE(GetHandleInformation(token, &flags));
}

TEST_F(ExecSpawnTest, OwnTokenSpawnSucceedsAndMarksAsUser)
{
    std::string reason;
    ScopedHandle token = OwnPrimaryToken(&reason);
    if (!token.valid())
    {
        GTEST_SKIP() << reason;
    }
    const std::string username = OwnEnvVarUtf8(L"USERNAME");
    ASSERT_FALSE(username.empty());

    // The spawned child is this same test binary, so under an ASan build it
    // needs the ASan runtime DLL's directory; that's on this process's own
    // PATH (a loaded MSVC dev environment), not on the duplicated token's
    // default profile PATH (table S2 deliberately builds the latter from
    // the token alone). Request env overrides the token base, same as any
    // other caller supplying a PATH the target identity should use.
    const std::string own_path = OwnEnvVarUtf8(L"PATH");
    taz_v1_KeyValue env[] = {{"PATH", own_path.c_str()}};

    const char *const args[] = {"--gtest_filter=NoSuchSuite.*"};
    taz_exec_spec_t spec{};
    spec.file = Exe().c_str();
    spec.args = args;
    spec.args_count = 1;
    spec.max_output_bytes = 1U << 20;
    spec.token = token.release();
    if (!own_path.empty())
    {
        spec.env = env;
        spec.env_count = 1;
    }
    (void)std::snprintf(spec.identity_user, sizeof(spec.identity_user), "%s",
                        username.c_str());

    ExecOutcome outcome;
    taz_exec_t *x = nullptr;
    ASSERT_EQ(taz_exec_start(Loop(), &spec, record_outcome, &outcome, &x), 0);
    // x is only valid up to uv_run below (on_done's close_all frees it), so
    // taz_exec_as_user/taz_exec_pid must be read now, not after.
    EXPECT_TRUE(taz_exec_as_user(x));
    EXPECT_NE(taz_exec_pid(x), 0U);

    ASSERT_EQ(uv_run(Loop(), UV_RUN_DEFAULT), 0);

    EXPECT_TRUE(outcome.called);
    EXPECT_EQ(outcome.exit_status, 0);
    EXPECT_FALSE(outcome.out.empty());
    EXPECT_FALSE(outcome.timed_out);
    EXPECT_FALSE(outcome.truncated);
}
#else
TEST_F(ExecSpawnTest, NonNullTokenReturnsNotSupported)
{
    taz_exec_spec_t spec{};
    spec.file = Exe().c_str();
    spec.token = reinterpret_cast<void *>(1);
    spec.max_output_bytes = 1024;

    ExecOutcome outcome;
    taz_exec_t *x = nullptr;
    EXPECT_EQ(taz_exec_start(Loop(), &spec, record_outcome, &outcome, &x),
              UV_ENOTSUP);
    EXPECT_FALSE(outcome.called);
    EXPECT_EQ(x, nullptr);
}
#endif

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
// request would be an override), so a differently-cased extra entry
// must be added alongside the original rather than replacing it.
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

// --- taz_exec_pid / taz_exec_signal ----------------------------------------
//
// taz_test_sleeper (see above) gives these a live child to probe without
// racing a fast exit; each test cancels it afterward so no sleeper outlives
// its test.

TEST_F(ExecSpawnTest, PidIsNonzeroAfterSpawnAndMatchesTheRealProcess)
{
    taz_exec_spec_t spec{};
    spec.file = TAZ_TEST_SLEEPER_PATH;
    spec.max_output_bytes = 1024;

    ExecOutcome outcome;
    taz_exec_t *x = nullptr;
    ASSERT_EQ(taz_exec_start(Loop(), &spec, record_outcome, &outcome, &x), 0);

    const uint32_t pid = taz_exec_pid(x);
    EXPECT_GT(pid, 0U);
    // uv_kill with signum 0 is a pure existence check on every platform
    // (POSIX kill(pid, 0); Windows GetExitCodeProcess), never a terminate.
    EXPECT_EQ(uv_kill(static_cast<int>(pid), 0), 0);

    taz_exec_cancel(x);
    ASSERT_EQ(uv_run(Loop(), UV_RUN_DEFAULT), 0);

    EXPECT_TRUE(outcome.called);
    EXPECT_TRUE(outcome.cancelled);
}

// Probes taz_exec_pid/taz_exec_signal from inside the on_done callback
// itself, where x->done is already true but x has not yet been freed (that
// only happens once close_all's handles finish closing, after on_done
// returns) - the one place a test can safely observe the "already finished"
// guard on a still-valid handle.
struct DuringDoneProbe
{
    taz_exec_t *x = nullptr;
    bool called = false;
    uint32_t pid_during_done = 0xFFFFFFFFU;
    int signal_rc_during_done = -1;
};

void probe_during_done(const taz_exec_result_t *result, void *arg)
{
    (void)result;
    auto *probe = static_cast<DuringDoneProbe *>(arg);
    probe->called = true;
    probe->pid_during_done = taz_exec_pid(probe->x);
    probe->signal_rc_during_done = taz_exec_signal(probe->x, 15);
}

TEST_F(ExecSpawnTest, PidAndSignalAreSafeNoOpsOnceOnDoneHasFired)
{
    const char *const args[] = {"--gtest_filter=NoSuchSuite.*"};
    taz_exec_spec_t spec{};
    spec.file = Exe().c_str();
    spec.args = args;
    spec.args_count = 1;
    spec.max_output_bytes = 1U << 20;

    DuringDoneProbe probe;
    taz_exec_t *x = nullptr;
    ASSERT_EQ(taz_exec_start(Loop(), &spec, probe_during_done, &probe, &x), 0);
    probe.x = x;

    ASSERT_EQ(uv_run(Loop(), UV_RUN_DEFAULT), 0);

    EXPECT_TRUE(probe.called);
    EXPECT_EQ(probe.pid_during_done, 0U);
    EXPECT_EQ(probe.signal_rc_during_done, 0);
}

// --- taz_exec_set_on_exit --------------------------------------------------
//
// taz_test_grandchild (see grandchild.c) forks a grandchild that inherits
// stdout and briefly outlives it: the immediate child (the exec's own
// process) exits almost instantly, while its stdout pipe stays open - and
// EOF-less - until the grandchild exits later. This separates "the on-exit
// hook fires" (at the immediate child's own exit) from "on_done fires" (once
// the pipe finally drains), which is exactly the ordering the hook exists to
// prove. The test asserts that ordering directly (a sequence of event
// names), not via wall-clock deltas, so it stays robust under ASan/TSan/
// Valgrind slowdowns.

struct OnExitOrderProbe
{
    std::vector<std::string> order;
    int on_exit_calls = 0;
    uint32_t pid = 0;
    uint64_t start_time = 0;
};

void record_on_exit_order(uint32_t pid, uint64_t start_time,
                          int64_t exit_status, int term_signal, void *arg)
{
    (void)exit_status;
    (void)term_signal;
    auto *probe = static_cast<OnExitOrderProbe *>(arg);

    probe->on_exit_calls++;
    probe->pid = pid;
    probe->start_time = start_time;
    probe->order.emplace_back("on_exit");
}

#ifndef _WIN32
void record_on_done_order(const taz_exec_result_t *result, void *arg)
{
    (void)result;
    auto *probe = static_cast<OnExitOrderProbe *>(arg);

    probe->order.emplace_back("on_done");
}

TEST_F(ExecSpawnTest, OnExitHookFiresBeforeOnDoneWhenGrandchildHoldsStdout)
{
    taz_exec_spec_t spec{};
    spec.file = TAZ_TEST_GRANDCHILD_PATH;
    spec.max_output_bytes = 1024;

    OnExitOrderProbe probe;
    taz_exec_t *x = nullptr;
    ASSERT_EQ(taz_exec_start(Loop(), &spec, record_on_done_order, &probe, &x),
              0);
    taz_exec_set_on_exit(x, record_on_exit_order, &probe);
    const uint32_t pid = taz_exec_pid(x);

    ASSERT_EQ(uv_run(Loop(), UV_RUN_DEFAULT), 0);

    ASSERT_EQ(probe.on_exit_calls, 1);
    ASSERT_EQ(probe.order.size(), 2U);
    EXPECT_EQ(probe.order[0], "on_exit");
    EXPECT_EQ(probe.order[1], "on_done");
    EXPECT_EQ(probe.pid, pid);
    EXPECT_GT(probe.start_time, 0U);
}
#endif

// Probes taz_exec_set_on_exit from inside the on_done callback itself,
// where x->done is already true but x has not yet been freed (that only
// happens once close_all's handles finish closing, after on_done returns) -
// the same safe-to-observe window PidAndSignalAreSafeNoOpsOnceOnDoneHasFired
// above uses for taz_exec_pid/taz_exec_signal. x must never be touched after
// uv_run returns: by then on_done's close_all has already freed it.
struct SetOnExitDuringDoneProbe
{
    taz_exec_t *x = nullptr;
    OnExitOrderProbe *hook_probe = nullptr;
    bool called = false;
};

void probe_set_on_exit_during_done(const taz_exec_result_t *result, void *arg)
{
    (void)result;
    auto *probe = static_cast<SetOnExitDuringDoneProbe *>(arg);

    probe->called = true;
    taz_exec_set_on_exit(probe->x, record_on_exit_order, probe->hook_probe);
}

TEST_F(ExecSpawnTest, OnExitHookCannotBeArmedFromInsideOnDone)
{
    const char *const args[] = {"--gtest_filter=NoSuchSuite.*"};
    taz_exec_spec_t spec{};
    spec.file = Exe().c_str();
    spec.args = args;
    spec.args_count = 1;
    spec.max_output_bytes = 1U << 20;

    OnExitOrderProbe hook_probe;
    SetOnExitDuringDoneProbe probe;
    taz_exec_t *x = nullptr;
    ASSERT_EQ(taz_exec_start(Loop(), &spec, probe_set_on_exit_during_done,
                             &probe, &x),
              0);
    probe.x = x;
    probe.hook_probe = &hook_probe;

    ASSERT_EQ(uv_run(Loop(), UV_RUN_DEFAULT), 0);

    EXPECT_TRUE(probe.called);
    // on_done had already set x->done by the time it ran, so the hook never
    // got armed - there is no future exit event left to fire it from.
    EXPECT_EQ(hook_probe.on_exit_calls, 0);
}

#ifndef _WIN32
TEST_F(ExecSpawnTest, SignalReachesTheChildsProcessGroupOnPosix)
{
    taz_exec_spec_t spec{};
    spec.file = TAZ_TEST_SLEEPER_PATH;
    spec.max_output_bytes = 1024;

    ExecOutcome outcome;
    taz_exec_t *x = nullptr;
    ASSERT_EQ(taz_exec_start(Loop(), &spec, record_outcome, &outcome, &x), 0);

    EXPECT_EQ(taz_exec_signal(x, SIGTERM), 0);

    auto start = std::chrono::steady_clock::now();
    ASSERT_EQ(uv_run(Loop(), UV_RUN_DEFAULT), 0);
    auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_TRUE(outcome.called);
    EXPECT_EQ(outcome.term_signal, SIGTERM);
    EXPECT_FALSE(outcome.cancelled);
    EXPECT_FALSE(outcome.timed_out);
    EXPECT_LT(elapsed, std::chrono::seconds(10));
}
#else
TEST_F(ExecSpawnTest, SignalIgnoresSignumAndTerminatesTheJobOnWindows)
{
    taz_exec_spec_t spec{};
    spec.file = TAZ_TEST_SLEEPER_PATH;
    spec.max_output_bytes = 1024;

    ExecOutcome outcome;
    taz_exec_t *x = nullptr;
    ASSERT_EQ(taz_exec_start(Loop(), &spec, record_outcome, &outcome, &x), 0);

    // The null signal delivers nothing, as on POSIX: the job keeps running.
    EXPECT_EQ(taz_exec_signal(x, 0), 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    (void)uv_run(Loop(), UV_RUN_NOWAIT);
    EXPECT_FALSE(outcome.called);

    EXPECT_EQ(taz_exec_signal(x, SIGTERM), 0);

    auto start = std::chrono::steady_clock::now();
    ASSERT_EQ(uv_run(Loop(), UV_RUN_DEFAULT), 0);
    auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_TRUE(outcome.called);
    EXPECT_FALSE(outcome.cancelled);
    EXPECT_FALSE(outcome.timed_out);
    EXPECT_LT(elapsed, std::chrono::seconds(10));
}

TEST_F(ExecSpawnTest, OutOfRangeSignumIsEinvalAndLeavesJobIntactOnWindows)
{
    taz_exec_spec_t spec{};
    spec.file = TAZ_TEST_SLEEPER_PATH;
    spec.max_output_bytes = 1024;

    ExecOutcome outcome;
    taz_exec_t *x = nullptr;
    ASSERT_EQ(taz_exec_start(Loop(), &spec, record_outcome, &outcome, &x), 0);

    EXPECT_EQ(taz_exec_signal(x, 99999), UV_EINVAL);

    taz_exec_cancel(x);
    ASSERT_EQ(uv_run(Loop(), UV_RUN_DEFAULT), 0);

    EXPECT_TRUE(outcome.called);
    EXPECT_TRUE(outcome.cancelled);
}
#endif

TEST_F(ExecSpawnTest, OutOfRangeSignumDoesNotCrashAndLeavesChildSignalable)
{
    taz_exec_spec_t spec{};
    spec.file = TAZ_TEST_SLEEPER_PATH;
    spec.max_output_bytes = 1024;

    ExecOutcome outcome;
    taz_exec_t *x = nullptr;
    ASSERT_EQ(taz_exec_start(Loop(), &spec, record_outcome, &outcome, &x), 0);

    (void)taz_exec_signal(x, 99999);

    taz_exec_cancel(x);
    ASSERT_EQ(uv_run(Loop(), UV_RUN_DEFAULT), 0);

    EXPECT_TRUE(outcome.called);
    EXPECT_TRUE(outcome.cancelled);
}

} // namespace
