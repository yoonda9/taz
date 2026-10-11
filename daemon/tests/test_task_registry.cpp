// Unit tests for taz/task_registry.h: the daemon-wide (pid, start_key) ->
// exit code registry that gives PROCESS_MONITOR an authoritative exit code
// for detached pids.

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>
#include <uv.h>

#include "taz/task_registry.h"

namespace
{

class TaskRegistryTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        taz_task_registry_reset_for_tests();
    }

    void TearDown() override
    {
        taz_task_registry_reset_for_tests();
    }
};

// Registers pids first_pid.. until every slot is taken, all running.
void fill_with_running(uint32_t first_pid, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++)
    {
        ASSERT_EQ(taz_task_registry_register(first_pid + i, 1U), 0)
            << "i=" << i;
    }
}

} // namespace

TEST_F(TaskRegistryTest, RegisterRecordLookupHit)
{
    ASSERT_EQ(taz_task_registry_register(100U, 1U), 0);
    taz_task_registry_record_exit(100U, 1U, 7);

    int64_t status = -1;
    ASSERT_EQ(taz_task_registry_lookup(100U, 1U, &status), 1);
    EXPECT_EQ(status, 7);
}

TEST_F(TaskRegistryTest, NegativeSignalCodeIsStoredAsIs)
{
    ASSERT_EQ(taz_task_registry_register(100U, 1U), 0);
    taz_task_registry_record_exit(100U, 1U, -15);

    int64_t status = 0;
    ASSERT_EQ(taz_task_registry_lookup(100U, 1U, &status), 1);
    EXPECT_EQ(status, -15);
}

TEST_F(TaskRegistryTest, LookupWithDifferentStartKeyMissesOnPidReuse)
{
    ASSERT_EQ(taz_task_registry_register(100U, 1U), 0);
    taz_task_registry_record_exit(100U, 1U, 7);

    int64_t status = -1;
    EXPECT_EQ(taz_task_registry_lookup(100U, 2U, &status), 0);
}

TEST_F(TaskRegistryTest, BothStartKeysCoexistForAReusedPid)
{
    ASSERT_EQ(taz_task_registry_register(100U, 1U), 0);
    ASSERT_EQ(taz_task_registry_register(100U, 2U), 0);
    taz_task_registry_record_exit(100U, 1U, 5);
    taz_task_registry_record_exit(100U, 2U, 9);

    int64_t status = -1;
    ASSERT_EQ(taz_task_registry_lookup(100U, 1U, &status), 1);
    EXPECT_EQ(status, 5);
    ASSERT_EQ(taz_task_registry_lookup(100U, 2U, &status), 1);
    EXPECT_EQ(status, 9);
}

TEST_F(TaskRegistryTest, LookupBeforeExitMisses)
{
    ASSERT_EQ(taz_task_registry_register(100U, 1U), 0);

    int64_t status = -1;
    EXPECT_EQ(taz_task_registry_lookup(100U, 1U, &status), 0);
}

TEST_F(TaskRegistryTest, IsPendingTrueOnlyWhileRunningAndKeysMatch)
{
    // Never registered.
    EXPECT_EQ(taz_task_registry_is_pending(100U, 1U), 0);

    ASSERT_EQ(taz_task_registry_register(100U, 1U), 0);
    EXPECT_EQ(taz_task_registry_is_pending(100U, 1U), 1);
    // A different start_key (pid reuse) never matches.
    EXPECT_EQ(taz_task_registry_is_pending(100U, 2U), 0);

    taz_task_registry_record_exit(100U, 1U, 7);
    // Exited: no longer "pending".
    EXPECT_EQ(taz_task_registry_is_pending(100U, 1U), 0);
}

TEST_F(TaskRegistryTest, RecordExitWithNoMatchIsIgnored)
{
    // Never registered.
    taz_task_registry_record_exit(100U, 1U, 7);
    int64_t status = -1;
    EXPECT_EQ(taz_task_registry_lookup(100U, 1U, &status), 0);

    // Registered under a different start_key (pid reuse).
    ASSERT_EQ(taz_task_registry_register(100U, 1U), 0);
    taz_task_registry_record_exit(100U, 2U, 7);
    EXPECT_EQ(taz_task_registry_lookup(100U, 1U, &status), 0);
    EXPECT_EQ(taz_task_registry_lookup(100U, 2U, &status), 0);
}

TEST_F(TaskRegistryTest, SecondExitRecordDoesNotOverwriteTheFirst)
{
    ASSERT_EQ(taz_task_registry_register(100U, 1U), 0);
    taz_task_registry_record_exit(100U, 1U, 7);
    taz_task_registry_record_exit(100U, 1U, 9);

    int64_t status = -1;
    ASSERT_EQ(taz_task_registry_lookup(100U, 1U, &status), 1);
    EXPECT_EQ(status, 7);
}

TEST_F(TaskRegistryTest, RegisteringTheSameKeyTwiceUsesOneEntry)
{
    ASSERT_EQ(taz_task_registry_register(100U, 1U), 0);
    taz_task_registry_record_exit(100U, 1U, 7);

    // Re-registering resets the one entry to running...
    ASSERT_EQ(taz_task_registry_register(100U, 1U), 0);
    EXPECT_EQ(taz_task_registry_is_pending(100U, 1U), 1);
    int64_t status = -1;
    EXPECT_EQ(taz_task_registry_lookup(100U, 1U, &status), 0);

    // ...and takes no second slot: every other slot still fits.
    fill_with_running(1000U, TAZ_TASK_REGISTRY_CAPACITY - 1U);
    EXPECT_EQ(taz_task_registry_register(999999U, 1U), UV_ENOMEM);
}

TEST_F(TaskRegistryTest, ExitedEntryStaysUntilItsSlotIsNeeded)
{
    ASSERT_EQ(taz_task_registry_register(100U, 1U), 0);
    taz_task_registry_record_exit(100U, 1U, 7);
    fill_with_running(1000U, TAZ_TASK_REGISTRY_CAPACITY - 1U);

    // The table is full, but nothing removed the exited entry.
    int64_t status = -1;
    ASSERT_EQ(taz_task_registry_lookup(100U, 1U, &status), 1);
    EXPECT_EQ(status, 7);

    // A new process takes its slot.
    ASSERT_EQ(taz_task_registry_register(999999U, 1U), 0);
    EXPECT_EQ(taz_task_registry_lookup(100U, 1U, &status), 0);
    EXPECT_EQ(taz_task_registry_is_pending(999999U, 1U), 1);
}

TEST_F(TaskRegistryTest, ReuseTakesTheEntryThatExitedLongestAgo)
{
    fill_with_running(1U, TAZ_TASK_REGISTRY_CAPACITY);
    // Exit order, not registration order, decides: 3000 first, then 2.
    taz_task_registry_record_exit(3000U, 1U, 30);
    taz_task_registry_record_exit(2U, 1U, 20);

    ASSERT_EQ(taz_task_registry_register(999999U, 1U), 0);

    int64_t status = -1;
    EXPECT_EQ(taz_task_registry_lookup(3000U, 1U, &status), 0);
    ASSERT_EQ(taz_task_registry_lookup(2U, 1U, &status), 1);
    EXPECT_EQ(status, 20);
}

TEST_F(TaskRegistryTest, FullOfRunningProcessesHasNoRoom)
{
    EXPECT_EQ(taz_task_registry_has_room(), 1);

    fill_with_running(1U, TAZ_TASK_REGISTRY_CAPACITY);
    EXPECT_EQ(taz_task_registry_has_room(), 0);
    EXPECT_EQ(taz_task_registry_register(999999U, 1U), UV_ENOMEM);

    // One exit makes a slot reusable again.
    taz_task_registry_record_exit(1U, 1U, 0);
    EXPECT_EQ(taz_task_registry_has_room(), 1);
    EXPECT_EQ(taz_task_registry_register(999999U, 1U), 0);
}

TEST_F(TaskRegistryTest, ConcurrentRegisterRecordLookupIsRaceFree)
{
    constexpr int kThreads = 8;
    constexpr int kItersPerThread = 200;

    struct ThreadCtx
    {
        int thread_index = 0;
        bool saw_mismatch = false;
    };

    auto work = [](void *arg)
    {
        auto *ctx = static_cast<ThreadCtx *>(arg);
        for (int i = 0; i < kItersPerThread; i++)
        {
            const uint32_t pid =
                1000000U + (static_cast<uint32_t>(ctx->thread_index) * 10000U) +
                static_cast<uint32_t>(i);
            const uint64_t start_key = static_cast<uint64_t>(i) + 1U;
            const int64_t exit_code = (static_cast<int64_t>(i) * 2) + 1;

            if (taz_task_registry_register(pid, start_key) != 0)
            {
                ctx->saw_mismatch = true;
                continue;
            }
            taz_task_registry_record_exit(pid, start_key, exit_code);

            int64_t observed = -1;
            if (taz_task_registry_lookup(pid, start_key, &observed) != 1 ||
                observed != exit_code)
            {
                ctx->saw_mismatch = true;
            }
        }
    };

    std::vector<uv_thread_t> threads(static_cast<size_t>(kThreads));
    std::vector<ThreadCtx> ctxs(static_cast<size_t>(kThreads));
    for (size_t i = 0; i < static_cast<size_t>(kThreads); i++)
    {
        ctxs[i].thread_index = static_cast<int>(i);
        ASSERT_EQ(uv_thread_create(&threads[i], work, &ctxs[i]), 0);
    }
    for (size_t i = 0; i < static_cast<size_t>(kThreads); i++)
    {
        ASSERT_EQ(uv_thread_join(&threads[i]), 0);
    }
    for (const auto &ctx : ctxs)
    {
        EXPECT_FALSE(ctx.saw_mismatch);
    }
}
