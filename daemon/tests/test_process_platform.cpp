// Unit tests for the cross-platform process API in taz/process.h:
// enumerate/inspect/watch against the live host (finding this test binary
// itself) and kill against a spawned, cross-platform sleeper child.
// Platform-specific assertions (open_files, POSIX signal mapping, pidfd
// polling, the starttime-reuse check) are guarded by #ifndef _WIN32 /
// #ifdef _WIN32; everything else runs on both platforms.

#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <fstream>
#include <string>
#include <thread>

#include <gtest/gtest.h>
#include <uv.h>

#include "taz/process.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>

#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
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

// ---------------------------------------------------------------------------
// taz_process_interval_cpu_percent (pure, both platforms)
// ---------------------------------------------------------------------------

TEST(IntervalCpuPercent, HalfSecondOfOneSecondWallIsFiftyPercent)
{
    EXPECT_FLOAT_EQ(
        taz_process_interval_cpu_percent(500000000ULL, 1000000000ULL), 50.0F);
}

TEST(IntervalCpuPercent, CanExceedOneHundredPercent)
{
    EXPECT_FLOAT_EQ(
        taz_process_interval_cpu_percent(2000000000ULL, 1000000000ULL), 200.0F);
}

TEST(IntervalCpuPercent, ZeroWallDeltaIsZero)
{
    EXPECT_FLOAT_EQ(taz_process_interval_cpu_percent(1ULL, 0ULL), 0.0F);
}

// ---------------------------------------------------------------------------
// taz_process_watch_open / _sample / _close (both platforms).
// ---------------------------------------------------------------------------

// Spins until at least cpu_ms milliseconds of *CPU* time (not wall time)
// have been burned, capped by a 5 s wall-clock deadline so a starved CI
// runner cannot hang the test.
void BusySpinCpuMs(long cpu_ms)
{
    const clock_t start = clock();
    const clock_t target = start + ((cpu_ms * CLOCKS_PER_SEC) / 1000);
    const auto wall_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);

    while (clock() < target)
    {
        if (std::chrono::steady_clock::now() > wall_deadline)
        {
            break;
        }
    }
}

#ifndef _WIN32
TEST_F(ProcessPlatformTest, OpenSelfWithPidfdYieldsAPollableNotYetReadableFd)
{
    taz_process_watch_t watch{};
    taz_v1_ErrorCode code = taz_v1_ErrorCode_ERROR_CODE_UNKNOWN;
    const char *detail = nullptr;

    ASSERT_EQ(taz_process_watch_open(SelfPid(), 1, &watch, &code, &detail), 0);
    ASSERT_GE(watch.exit_fd, 0);

    struct pollfd pfd{};
    pfd.fd = watch.exit_fd;
    pfd.events = POLLIN;
    EXPECT_EQ(poll(&pfd, 1, 0), 0)
        << "a live process's pidfd must not be readable yet";

    taz_process_watch_close(&watch);
}
#endif // !_WIN32

TEST_F(ProcessPlatformTest, OpenSelfWithPidfdDisabledLeavesExitFdMinusOne)
{
    taz_process_watch_t watch{};
    taz_v1_ErrorCode code = taz_v1_ErrorCode_ERROR_CODE_UNKNOWN;
    const char *detail = nullptr;

    // use_pidfd is Linux-only; Windows ignores it and always holds a
    // handle, so exit_fd stays -1 there regardless of this argument.
    ASSERT_EQ(taz_process_watch_open(SelfPid(), 0, &watch, &code, &detail), 0);
    EXPECT_EQ(watch.exit_fd, -1);
#ifdef _WIN32
    EXPECT_NE(watch.handle, nullptr);
#endif

    taz_process_watch_close(&watch);
}

TEST_F(ProcessPlatformTest, SampleSelfLiveReportsCpuTimeAndInfoFields)
{
    taz_process_watch_t watch{};
    taz_process_sample_t sample{};
    taz_v1_ErrorCode code = taz_v1_ErrorCode_ERROR_CODE_UNKNOWN;
    const char *detail = nullptr;

    ASSERT_EQ(taz_process_watch_open(SelfPid(), 1, &watch, &code, &detail), 0);
    BusySpinCpuMs(20);

    ASSERT_EQ(taz_process_watch_sample(&watch, &sample, &code, &detail), 0);
    EXPECT_EQ(sample.state, TAZ_PROCESS_SAMPLE_LIVE);
    EXPECT_GT(sample.cpu_time_ns, 0ULL);
    EXPECT_EQ(sample.info.pid, SelfPid());
    EXPECT_EQ(sample.info.name, ExpectedSelfName());
    EXPECT_NE(sample.info.state[0], '\0');
    EXPECT_GT(sample.info.memory_bytes, 0ULL);
    EXPECT_NE(sample.info.user[0], '\0');

    taz_process_watch_close(&watch);
}

TEST_F(ProcessPlatformTest, TwoSamplesFiftyMillisecondsApartAreNonDecreasing)
{
    taz_process_watch_t watch{};
    taz_process_sample_t first{};
    taz_process_sample_t second{};
    taz_v1_ErrorCode code = taz_v1_ErrorCode_ERROR_CODE_UNKNOWN;
    const char *detail = nullptr;

    ASSERT_EQ(taz_process_watch_open(SelfPid(), 1, &watch, &code, &detail), 0);
    ASSERT_EQ(taz_process_watch_sample(&watch, &first, &code, &detail), 0);

    BusySpinCpuMs(20);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    ASSERT_EQ(taz_process_watch_sample(&watch, &second, &code, &detail), 0);
    EXPECT_GE(second.cpu_time_ns, first.cpu_time_ns);

    taz_process_watch_close(&watch);
}

// On Windows OpenProcess fails for an impossible pid regardless of
// use_pidfd (it is ignored there), so this covers both platforms: Linux via
// the pidfd syscall's ESRCH, Windows via OpenProcess's
// ERROR_INVALID_PARAMETER.
TEST_F(ProcessPlatformTest, ImpossiblePidIsNotFoundFromOpen)
{
    taz_process_watch_t watch{};
    taz_v1_ErrorCode code = taz_v1_ErrorCode_ERROR_CODE_UNKNOWN;
    const char *detail = nullptr;
    const uint32_t pid = ImpossiblePid();

    EXPECT_EQ(taz_process_watch_open(pid, 1, &watch, &code, &detail), -1);
    EXPECT_EQ(code, taz_v1_ErrorCode_ERROR_CODE_NOT_FOUND);
}

#ifndef _WIN32
// Only meaningful on the fallback path: with pidfd disabled, open cannot
// tell an impossible pid from a real one (it makes no syscall at all), so
// the first proof of nonexistence comes from the pool-side /proc read in
// sample. Windows has no equivalent: OpenProcess always validates the pid
// at open time (ImpossiblePidIsNotFoundFromOpen above already covers it).
TEST_F(ProcessPlatformTest, ImpossiblePidWithPidfdDisabledIsNotFoundFromSample)
{
    taz_process_watch_t watch{};
    taz_process_sample_t sample{};
    taz_v1_ErrorCode code = taz_v1_ErrorCode_ERROR_CODE_UNKNOWN;
    const char *detail = nullptr;
    const uint32_t pid = ImpossiblePid();

    ASSERT_EQ(taz_process_watch_open(pid, 0, &watch, &code, &detail), 0);
    EXPECT_EQ(watch.exit_fd, -1);
    EXPECT_EQ(taz_process_watch_sample(&watch, &sample, &code, &detail), -1);
    EXPECT_EQ(code, taz_v1_ErrorCode_ERROR_CODE_NOT_FOUND);

    taz_process_watch_close(&watch);
}
#endif // !_WIN32

TEST_F(ProcessPlatformTest, CloseTwiceIsNoop)
{
    taz_process_watch_t watch{};
    taz_v1_ErrorCode code = taz_v1_ErrorCode_ERROR_CODE_UNKNOWN;
    const char *detail = nullptr;

    ASSERT_EQ(taz_process_watch_open(SelfPid(), 1, &watch, &code, &detail), 0);
#ifndef _WIN32
    const int fd = watch.exit_fd;
    ASSERT_GE(fd, 0);
#else
    ASSERT_NE(watch.handle, nullptr);
#endif

    taz_process_watch_close(&watch);
    EXPECT_EQ(watch.exit_fd, -1);
#ifndef _WIN32
    EXPECT_EQ(fcntl(fd, F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);
#else
    EXPECT_EQ(watch.handle, nullptr);
#endif

    taz_process_watch_close(&watch);
    EXPECT_EQ(watch.exit_fd, -1);
#ifdef _WIN32
    EXPECT_EQ(watch.handle, nullptr);
#endif
}

#ifndef _WIN32
// The starttime-reuse check is Linux-only: a held Windows handle keeps the
// pid's kernel object alive, so the same pid can never be reassigned to a
// different process while the watch is open, and taz_process_watch_sample
// does not implement the check there.
TEST_F(ProcessPlatformTest, FirstStartTimeMismatchReportsExited)
{
    taz_process_watch_t watch{};
    taz_process_sample_t sample{};
    taz_v1_ErrorCode code = taz_v1_ErrorCode_ERROR_CODE_UNKNOWN;
    const char *detail = nullptr;

    ASSERT_EQ(taz_process_watch_open(SelfPid(), 1, &watch, &code, &detail), 0);
    ASSERT_EQ(taz_process_watch_sample(&watch, &sample, &code, &detail), 0);
    ASSERT_EQ(sample.state, TAZ_PROCESS_SAMPLE_LIVE);

    watch.first_starttime = sample.starttime + 1U;
    ASSERT_EQ(taz_process_watch_sample(&watch, &sample, &code, &detail), 0);
    EXPECT_EQ(sample.state, TAZ_PROCESS_SAMPLE_EXITED);

    taz_process_watch_close(&watch);
}
#endif // !_WIN32

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

#ifndef _WIN32
// libuv reaps a uv_spawn child itself (its SIGCHLD watcher calls waitpid
// before the exit callback runs), so by the time outcome.called is true
// /proc/<pid> is already gone: the fallback sample path reports NOT_FOUND,
// not EXITED (documented divergence from the pidfd path, which the
// PROCESS_MONITOR handler reconciles in a later step). The pidfd opened
// before the kill does observe the exit, independent of the reap.
TEST_F(ProcessSpawnTest, SleeperKilledAndReapedIsPidfdReadableButSampleNotFound)
{
    uv_process_t process{};
    SpawnOutcome outcome;
    const uint32_t pid = SpawnSleeper(Loop(), &process, &outcome);
    ASSERT_NE(pid, 0U);

    taz_process_watch_t watch{};
    taz_v1_ErrorCode code = taz_v1_ErrorCode_ERROR_CODE_UNKNOWN;
    const char *detail = nullptr;
    ASSERT_EQ(taz_process_watch_open(pid, 1, &watch, &code, &detail), 0);
    ASSERT_GE(watch.exit_fd, 0);

    ASSERT_EQ(taz_process_kill(pid, 0, &code, &detail), 0);
    ASSERT_EQ(uv_run(Loop(), UV_RUN_DEFAULT), 0);
    EXPECT_TRUE(outcome.called);

    struct pollfd pfd{};
    pfd.fd = watch.exit_fd;
    pfd.events = POLLIN;
    ASSERT_EQ(poll(&pfd, 1, 2000), 1);
    EXPECT_NE(pfd.revents & POLLIN, 0);

    taz_process_sample_t sample{};
    EXPECT_EQ(taz_process_watch_sample(&watch, &sample, &code, &detail), -1);
    EXPECT_EQ(code, taz_v1_ErrorCode_ERROR_CODE_NOT_FOUND);

    taz_process_watch_close(&watch);
    uv_close(reinterpret_cast<uv_handle_t *>(&process), nullptr);
    ASSERT_EQ(uv_run(Loop(), UV_RUN_DEFAULT), 0);
}
#endif // !_WIN32

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

// A watch opened before the kill holds the sleeper's handle for the
// stream's life, so (unlike the Linux fallback, where libuv's own reap
// beats the sample to /proc) the held handle survives the exit and the
// sample reports it with the real exit code. The handle also keeps the pid
// from being reused, so an aliased or impossible pid opened afterward is
// still NOT_FOUND.
TEST_F(ProcessSpawnTest, WatchHeldHandleSurvivesSleeperExitWithExitCode)
{
    uv_process_t process{};
    SpawnOutcome outcome;
    const uint32_t pid = SpawnSleeper(Loop(), &process, &outcome);
    ASSERT_NE(pid, 0U);

    taz_process_watch_t watch{};
    taz_v1_ErrorCode code = taz_v1_ErrorCode_ERROR_CODE_UNKNOWN;
    const char *detail = nullptr;
    ASSERT_EQ(taz_process_watch_open(pid, 1, &watch, &code, &detail), 0);
    EXPECT_NE(watch.handle, nullptr);
    EXPECT_EQ(watch.exit_fd, -1);

    ASSERT_EQ(uv_process_kill(&process, SIGTERM), 0);
    ASSERT_EQ(uv_run(Loop(), UV_RUN_DEFAULT), 0);
    EXPECT_TRUE(outcome.called);
    uv_close(reinterpret_cast<uv_handle_t *>(&process), nullptr);
    ASSERT_EQ(uv_run(Loop(), UV_RUN_DEFAULT), 0);

    taz_process_sample_t sample{};
    ASSERT_EQ(taz_process_watch_sample(&watch, &sample, &code, &detail), 0);
    EXPECT_EQ(sample.state, TAZ_PROCESS_SAMPLE_EXITED);
    EXPECT_EQ(sample.exit_code_known, 1);
    EXPECT_EQ(sample.exit_code, 1);

    code = taz_v1_ErrorCode_ERROR_CODE_UNKNOWN;
    taz_process_watch_t aliased_watch{};
    EXPECT_EQ(
        taz_process_watch_open(pid + 1U, 1, &aliased_watch, &code, &detail),
        -1);
    EXPECT_EQ(code, taz_v1_ErrorCode_ERROR_CODE_NOT_FOUND);

    code = taz_v1_ErrorCode_ERROR_CODE_UNKNOWN;
    taz_process_watch_t impossible_watch{};
    EXPECT_EQ(taz_process_watch_open(ImpossiblePid(), 1, &impossible_watch,
                                     &code, &detail),
              -1);
    EXPECT_EQ(code, taz_v1_ErrorCode_ERROR_CODE_NOT_FOUND);

    taz_process_watch_close(&watch);
    EXPECT_EQ(watch.handle, nullptr);
    taz_process_watch_close(&watch);
    EXPECT_EQ(watch.handle, nullptr);
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

#ifndef _WIN32
// libuv's SIGCHLD watcher reaps every uv_spawn child itself, so a zombie
// (killed but not yet waited) needs a raw fork+execv child instead - this
// test waitpid()s it only at the very end, after the sample assertions.
TEST(ProcessWatchZombie, SampleReportsExitedWithZombieState)
{
    const pid_t child = fork();
    ASSERT_NE(child, -1);
    if (child == 0)
    {
        char *const args[] = {const_cast<char *>(TAZ_TEST_SLEEPER_PATH),
                              nullptr};
        execv(TAZ_TEST_SLEEPER_PATH, args);
        _exit(127);
    }

    const uint32_t pid = static_cast<uint32_t>(child);
    taz_process_watch_t watch{};
    taz_v1_ErrorCode code = taz_v1_ErrorCode_ERROR_CODE_UNKNOWN;
    const char *detail = nullptr;
    ASSERT_EQ(taz_process_watch_open(pid, 1, &watch, &code, &detail), 0);

    ASSERT_EQ(kill(child, SIGKILL), 0);

    taz_process_sample_t sample{};
    bool exited = false;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if ((taz_process_watch_sample(&watch, &sample, &code, &detail) == 0) &&
            (sample.state == TAZ_PROCESS_SAMPLE_EXITED))
        {
            exited = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_TRUE(exited);
    EXPECT_STREQ(sample.info.state, "zombie");

    taz_process_watch_close(&watch);

    int status = 0;
    EXPECT_EQ(waitpid(child, &status, 0), child);
}
#endif // !_WIN32

} // namespace
