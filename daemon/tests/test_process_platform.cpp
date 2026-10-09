// Unit tests for the cross-platform process API in taz/process.h:
// enumerate/inspect against the live host (finding this test binary
// itself) and kill against a spawned, cross-platform sleeper child.
// Linux-specific assertions (open_files, POSIX signal mapping) are
// guarded by #ifndef _WIN32; everything else runs on both platforms once
// src/platform/process_win32.c exists.

#include <cstdint>
#include <cstring>
#include <ctime>
#include <fstream>
#include <string>

#include <gtest/gtest.h>
#include <uv.h>

#include "taz/process.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>

#include <unistd.h>
#endif

namespace
{

uint32_t SelfPid()
{
#ifdef _WIN32
    return static_cast<uint32_t>(GetCurrentProcessId());
#else
    return static_cast<uint32_t>(getpid());
#endif
}

// A pid that cannot exist: on Linux, one past the kernel's configured
// ceiling, read rather than hardcoded because it varies by host.
uint32_t ImpossiblePid()
{
#ifndef _WIN32
    std::ifstream f("/proc/sys/kernel/pid_max");
    unsigned long long pid_max = 0ULL;
    f >> pid_max;
    return static_cast<uint32_t>(pid_max + 1ULL);
#else
    return 0x7FFFFFFCU;
#endif
}

// The name taz_process_enumerate/_inspect must report for this process.
// Not hardcoded to "taz_tests": under valgrind, the kernel's /proc/self/comm
// for *every* process is the wrapping tool's own name (e.g.
// "memcheck-amd64-"), because valgrind's client runs inside the tool's own
// process image rather than a separate exec. Reading /proc/self/comm
// ourselves, the same source the production code reads, keeps this
// assertion meaningful (it still exercises the full read/sanitize/truncate
// pipeline) without depending on a test runner that happens not to rename
// the process.
std::string ExpectedSelfName()
{
#ifndef _WIN32
    std::ifstream f("/proc/self/comm");
    std::string comm;
    std::getline(f, comm);
    return comm;
#else
    return "taz_tests.exe";
#endif
}

class ProcessPlatformTest : public ::testing::Test
{
  protected:
    void TearDown() override
    {
        taz_process_list_free(&list_);
        taz_process_detail_free(&detail_);
    }

    taz_process_list_t *List()
    {
        return &list_;
    }

    taz_process_detail_t *Detail()
    {
        return &detail_;
    }

  private:
    taz_process_list_t list_{};
    taz_process_detail_t detail_{};
};

TEST_F(ProcessPlatformTest, EnumerateEmptyFilterContainsSelfWithExpectedFields)
{
    taz_v1_ErrorCode code = taz_v1_ErrorCode_ERROR_CODE_UNKNOWN;
    const char *detail = nullptr;

    ASSERT_EQ(taz_process_enumerate("", List(), &code, &detail), 0);
    ASSERT_GT(List()->count, 0U);

    const uint32_t self_pid = SelfPid();
    const taz_process_entry_t *self_entry = nullptr;
    for (size_t i = 0U; i < List()->count; i++)
    {
        if (List()->entries[i].pid == self_pid)
        {
            self_entry = &List()->entries[i];
        }
        if (i > 0U)
        {
            EXPECT_LT(List()->entries[i - 1U].pid, List()->entries[i].pid)
                << "entries must be strictly ascending by pid";
        }
    }
    ASSERT_NE(self_entry, nullptr);
    EXPECT_EQ(self_entry->name, ExpectedSelfName());
    EXPECT_TRUE((std::strcmp(self_entry->state, "running") == 0) ||
                (std::strcmp(self_entry->state, "sleeping") == 0))
        << "state was \"" << self_entry->state << "\"";
    EXPECT_NE(self_entry->user[0], '\0');
    EXPECT_GT(self_entry->memory_bytes, 0ULL);
}

TEST_F(ProcessPlatformTest, EnumerateFilterMatchesSelfBySubstring)
{
    taz_v1_ErrorCode code = taz_v1_ErrorCode_ERROR_CODE_UNKNOWN;
    const char *detail = nullptr;
    const uint32_t self_pid = SelfPid();
    // A strict (non-full-string) prefix of the expected name, so this
    // exercises substring matching rather than an exact match.
    const std::string filter = ExpectedSelfName().substr(0, 4);

    ASSERT_EQ(taz_process_enumerate(filter.c_str(), List(), &code, &detail), 0);

    bool found = false;
    for (size_t i = 0U; i < List()->count; i++)
    {
        found = found || (List()->entries[i].pid == self_pid);
    }
    EXPECT_TRUE(found);
}

// pid 0 is not addressable (KILL and INFO reject it), so the list never
// offers it; on Windows Toolhelp reports the System Idle Process as pid 0.
TEST_F(ProcessPlatformTest, EnumerateNeverListsPidZero)
{
    taz_v1_ErrorCode code = taz_v1_ErrorCode_ERROR_CODE_UNKNOWN;
    const char *detail = nullptr;

    ASSERT_EQ(taz_process_enumerate("", List(), &code, &detail), 0);
    for (size_t i = 0U; i < List()->count; i++)
    {
        EXPECT_NE(List()->entries[i].pid, 0U);
    }
}

TEST_F(ProcessPlatformTest, EnumerateNoMatchFilterReturnsEmptyListNotError)
{
    taz_v1_ErrorCode code = taz_v1_ErrorCode_ERROR_CODE_UNKNOWN;
    const char *detail = nullptr;

    ASSERT_EQ(
        taz_process_enumerate("no-such-name-7f3a", List(), &code, &detail), 0);
    EXPECT_EQ(List()->count, 0U);
}

#ifndef _WIN32
TEST_F(ProcessPlatformTest, InspectSelfReportsCommandLineStartTimeAndOpenFiles)
{
    const std::string tmp_dir = ::testing::TempDir();
    const std::string path_a = tmp_dir + "taz_process_test_scratch_a";
    const std::string path_b = tmp_dir + "taz_process_test_scratch_b";

    uv_fs_t open_req_a;
    const int fd_a = uv_fs_open(nullptr, &open_req_a, path_a.c_str(),
                                UV_FS_O_CREAT | UV_FS_O_RDWR, 0600, nullptr);
    ASSERT_GE(fd_a, 0);
    uv_fs_req_cleanup(&open_req_a);

    uv_fs_t open_req_b;
    const int fd_b = uv_fs_open(nullptr, &open_req_b, path_b.c_str(),
                                UV_FS_O_CREAT | UV_FS_O_RDWR, 0600, nullptr);
    ASSERT_GE(fd_b, 0);
    uv_fs_req_cleanup(&open_req_b);
    ASSERT_LT(fd_a, fd_b) << "fd_b must be the later (higher) fd for the "
                             "ascending-order assertion below to mean "
                             "anything";

    taz_v1_ErrorCode code = taz_v1_ErrorCode_ERROR_CODE_UNKNOWN;
    const char *detail = nullptr;
    const uint32_t self_pid = SelfPid();

    ASSERT_EQ(taz_process_inspect(self_pid, Detail(), &code, &detail), 0);

    EXPECT_EQ(Detail()->info.pid, self_pid);
    ASSERT_NE(Detail()->command_line, nullptr);
    EXPECT_NE(std::strstr(Detail()->command_line, "taz_tests"), nullptr);

    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    const int64_t diff = now - static_cast<int64_t>(Detail()->start_time);
    EXPECT_LE((diff < 0) ? -diff : diff, 60)
        << "start_time was " << Detail()->start_time << ", now is " << now;

    long index_a = -1;
    long index_b = -1;
    for (size_t i = 0U; i < Detail()->open_files_count; i++)
    {
        if (path_a == Detail()->open_files[i])
        {
            index_a = static_cast<long>(i);
        }
        if (path_b == Detail()->open_files[i])
        {
            index_b = static_cast<long>(i);
        }
    }
    EXPECT_GE(index_a, 0) << "scratch file a not found in open_files";
    EXPECT_GE(index_b, 0) << "scratch file b not found in open_files";
    EXPECT_LT(index_a, index_b)
        << "open_files must be reported in ascending fd order";

    uv_fs_t close_req;
    (void)uv_fs_close(nullptr, &close_req, fd_a, nullptr);
    uv_fs_req_cleanup(&close_req);
    (void)uv_fs_close(nullptr, &close_req, fd_b, nullptr);
    uv_fs_req_cleanup(&close_req);

    uv_fs_t unlink_req;
    (void)uv_fs_unlink(nullptr, &unlink_req, path_a.c_str(), nullptr);
    uv_fs_req_cleanup(&unlink_req);
    (void)uv_fs_unlink(nullptr, &unlink_req, path_b.c_str(), nullptr);
    uv_fs_req_cleanup(&unlink_req);
}
#endif // !_WIN32

TEST_F(ProcessPlatformTest, InspectImpossiblePidIsNotFound)
{
    taz_v1_ErrorCode code = taz_v1_ErrorCode_ERROR_CODE_UNKNOWN;
    const char *detail = nullptr;

    EXPECT_EQ(taz_process_inspect(ImpossiblePid(), Detail(), &code, &detail),
              -1);
    EXPECT_EQ(code, taz_v1_ErrorCode_ERROR_CODE_NOT_FOUND);
}

TEST_F(ProcessPlatformTest, KillImpossiblePidIsNotFound)
{
    taz_v1_ErrorCode code = taz_v1_ErrorCode_ERROR_CODE_UNKNOWN;
    const char *detail = nullptr;

    EXPECT_EQ(taz_process_kill(ImpossiblePid(), 0, &code, &detail), -1);
    EXPECT_EQ(code, taz_v1_ErrorCode_ERROR_CODE_NOT_FOUND);
}

TEST_F(ProcessPlatformTest, FreeFunctionsOnZeroedStructsAreNoops)
{
    taz_process_list_t zeroed_list{};
    taz_process_detail_t zeroed_detail{};

    taz_process_list_free(&zeroed_list);
    taz_process_detail_free(&zeroed_detail);

    EXPECT_EQ(zeroed_list.count, 0U);
    EXPECT_EQ(zeroed_list.entries, nullptr);
    EXPECT_EQ(zeroed_detail.open_files_count, 0U);
    EXPECT_EQ(zeroed_detail.command_line, nullptr);
}

// --- spawn / kill against a real child -------------------------------------
//
// taz_test_sleeper (built from sleeper.c) sleeps for 60 s with no shell
// involved, so a broken kill path leaves a process still running long
// after these assertions.

struct SpawnOutcome
{
    bool called = false;
    int64_t exit_status = -1;
    int term_signal = 0;
};

void OnSleeperExit(uv_process_t *process, int64_t exit_status, int term_signal)
{
    auto *outcome = static_cast<SpawnOutcome *>(process->data);

    outcome->called = true;
    outcome->exit_status = exit_status;
    outcome->term_signal = term_signal;
}

// Returns the spawned pid, or 0 on failure.
uint32_t SpawnSleeper(uv_loop_t *loop, uv_process_t *process,
                      SpawnOutcome *outcome)
{
    // uv_process_options_t.args is a plain char** (libuv never declares it
    // const, even though uv_spawn only reads it); args itself is never
    // reassigned after this initializer, so the elements are declared
    // const and cast away only at the one point libuv's API demands it.
    char *const args[] = {const_cast<char *>(TAZ_TEST_SLEEPER_PATH), nullptr};
    uv_process_options_t options;

    std::memset(&options, 0, sizeof(options));
    options.exit_cb = OnSleeperExit;
    options.file = TAZ_TEST_SLEEPER_PATH;
    options.args = const_cast<char **>(args);

    process->data = outcome;
    if (uv_spawn(loop, process, &options) != 0)
    {
        return 0U;
    }
    return static_cast<uint32_t>(uv_process_get_pid(process));
}

class ProcessSpawnTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        ASSERT_EQ(uv_loop_init(&loop_), 0);
    }

    void TearDown() override
    {
        taz_process_detail_free(&detail_);
        ASSERT_EQ(uv_run(&loop_, UV_RUN_DEFAULT), 0);
        ASSERT_EQ(uv_loop_close(&loop_), 0);
    }

    uv_loop_t *Loop()
    {
        return &loop_;
    }

    taz_process_detail_t *Detail()
    {
        return &detail_;
    }

  private:
    uv_loop_t loop_{};
    taz_process_detail_t detail_{};
};

TEST_F(ProcessSpawnTest, KillZeroSendsTerminateAndReportsNotFoundAfterReap)
{
    uv_process_t process{};
    SpawnOutcome outcome;
    const uint32_t pid = SpawnSleeper(Loop(), &process, &outcome);
    ASSERT_NE(pid, 0U);

    taz_v1_ErrorCode code = taz_v1_ErrorCode_ERROR_CODE_UNKNOWN;
    const char *detail = nullptr;
    ASSERT_EQ(taz_process_kill(pid, 0, &code, &detail), 0);

    ASSERT_EQ(uv_run(Loop(), UV_RUN_DEFAULT), 0);
    EXPECT_TRUE(outcome.called);
#ifndef _WIN32
    EXPECT_EQ(outcome.term_signal, SIGTERM);
#else
    EXPECT_EQ(outcome.exit_status, 1);
#endif

    EXPECT_EQ(taz_process_kill(pid, 0, &code, &detail), -1);
    EXPECT_EQ(code, taz_v1_ErrorCode_ERROR_CODE_NOT_FOUND);

    EXPECT_EQ(taz_process_inspect(pid, Detail(), &code, &detail), -1);
    EXPECT_EQ(code, taz_v1_ErrorCode_ERROR_CODE_NOT_FOUND);

    uv_close(reinterpret_cast<uv_handle_t *>(&process), nullptr);
    ASSERT_EQ(uv_run(Loop(), UV_RUN_DEFAULT), 0);
}

#ifdef _WIN32
// OpenProcess ignores a pid's low two bits, so pid + 1 would open the
// sleeper itself. Windows pids are multiples of 4, so pid + 1 is never a
// real process and this cannot touch anything but the sleeper.
TEST_F(ProcessSpawnTest, AliasedPidIsNotFoundAndSleeperStaysAlive)
{
    uv_process_t process{};
    SpawnOutcome outcome;
    const uint32_t pid = SpawnSleeper(Loop(), &process, &outcome);
    ASSERT_NE(pid, 0U);
    ASSERT_EQ(pid % 4U, 0U);

    taz_v1_ErrorCode code = taz_v1_ErrorCode_ERROR_CODE_UNKNOWN;
    const char *detail = nullptr;
    EXPECT_EQ(taz_process_inspect(pid + 1U, Detail(), &code, &detail), -1);
    EXPECT_EQ(code, taz_v1_ErrorCode_ERROR_CODE_NOT_FOUND);
    taz_process_detail_free(Detail());

    code = taz_v1_ErrorCode_ERROR_CODE_UNKNOWN;
    EXPECT_EQ(taz_process_kill(pid + 1U, 0, &code, &detail), -1);
    EXPECT_EQ(code, taz_v1_ErrorCode_ERROR_CODE_NOT_FOUND);
    EXPECT_NE(uv_run(Loop(), UV_RUN_NOWAIT), 0);
    EXPECT_FALSE(outcome.called);

    EXPECT_EQ(taz_process_kill(pid, 0, &code, &detail), 0);
    ASSERT_EQ(uv_run(Loop(), UV_RUN_DEFAULT), 0);
    EXPECT_TRUE(outcome.called);

    uv_close(reinterpret_cast<uv_handle_t *>(&process), nullptr);
    ASSERT_EQ(uv_run(Loop(), UV_RUN_DEFAULT), 0);
}
#endif // _WIN32

#ifndef _WIN32
TEST_F(ProcessSpawnTest, UnknownSignalIsInvalidRequestAndSleeperStaysAlive)
{
    uv_process_t process{};
    SpawnOutcome outcome;
    const uint32_t pid = SpawnSleeper(Loop(), &process, &outcome);
    ASSERT_NE(pid, 0U);

    taz_v1_ErrorCode code = taz_v1_ErrorCode_ERROR_CODE_UNKNOWN;
    const char *detail = nullptr;
    EXPECT_EQ(taz_process_kill(pid, 999999, &code, &detail), -1);
    EXPECT_EQ(code, taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST);
    EXPECT_FALSE(outcome.called);

    EXPECT_EQ(taz_process_kill(pid, SIGKILL, &code, &detail), 0);
    ASSERT_EQ(uv_run(Loop(), UV_RUN_DEFAULT), 0);
    EXPECT_TRUE(outcome.called);

    uv_close(reinterpret_cast<uv_handle_t *>(&process), nullptr);
    ASSERT_EQ(uv_run(Loop(), UV_RUN_DEFAULT), 0);
}
#endif // !_WIN32

} // namespace
