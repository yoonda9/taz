// Unit tests for taz/task.h: the per-connection fixed array of
// TAZ_TASK_SLOTS detached-task slots, driven purely through its API (no
// exec spawned). Covers retention/eviction and task_id lookup exactly as
// context.md's behaviour table specifies.

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "taz/task.h"

namespace
{

class TaskTableTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        std::memset(&table_, 0, sizeof(table_));
    }

    void TearDown() override
    {
        taz_task_table_free(&table_);
    }

    taz_task_table_t *Table()
    {
        return &table_;
    }

  private:
    taz_task_table_t table_;
};

} // namespace

TEST_F(TaskTableTest, ZeroedTableIsAValidEmptyTable)
{
    EXPECT_EQ(taz_task_find(Table(), 1U), nullptr);
    EXPECT_EQ(taz_task_find(Table(), 0U), nullptr);
}

TEST_F(TaskTableTest, AllocOnAFreeSlotReturnsAUniquePendingTask)
{
    const uint32_t id = taz_task_alloc(
        Table(), 42U, taz_v1_ErrorPriority_ERROR_PRIORITY_CRITICAL, true);
    ASSERT_NE(id, 0U);

    const detached_task_t *slot = taz_task_find(Table(), id);
    ASSERT_NE(slot, nullptr);
    EXPECT_EQ(slot->task_id, id);
    EXPECT_EQ(slot->stream_id, 42U);
    EXPECT_EQ(slot->state, taz_v1_TaskState_TASK_STATE_PENDING);
    EXPECT_EQ(slot->error_priority,
              taz_v1_ErrorPriority_ERROR_PRIORITY_CRITICAL);
    EXPECT_TRUE(slot->monitor);
    EXPECT_EQ(slot->exec, nullptr);
    EXPECT_EQ(slot->result, nullptr);
    EXPECT_EQ(slot->finish_seq, 0U);
}

TEST_F(TaskTableTest, TaskIdsAreUniqueAcrossManyAllocations)
{
    std::vector<uint32_t> ids;
    for (int i = 0; i < 10; i++)
    {
        const uint32_t id =
            taz_task_alloc(Table(), static_cast<uint32_t>(i),
                           taz_v1_ErrorPriority_ERROR_PRIORITY_NONE, false);
        ASSERT_NE(id, 0U);
        for (const uint32_t prior : ids)
        {
            EXPECT_NE(prior, id);
        }
        ids.push_back(id);
        taz_task_free(Table(), id); // free immediately: slots are reused,
                                    // but ids must still never repeat
    }
}

TEST_F(TaskTableTest, FindUnknownTaskIdReturnsNull)
{
    EXPECT_EQ(taz_task_find(Table(), 999U), nullptr);
}

TEST_F(TaskTableTest, FindAfterFreeReturnsNull)
{
    const uint32_t id = taz_task_alloc(
        Table(), 1U, taz_v1_ErrorPriority_ERROR_PRIORITY_NONE, true);
    ASSERT_NE(id, 0U);

    taz_task_free(Table(), id);
    EXPECT_EQ(taz_task_find(Table(), id), nullptr);
}

TEST_F(TaskTableTest, SetStateAdvancesStateAndStampsFinishSeqOnlyOnce)
{
    const uint32_t id = taz_task_alloc(
        Table(), 1U, taz_v1_ErrorPriority_ERROR_PRIORITY_NONE, true);
    ASSERT_NE(id, 0U);

    taz_task_set_state(Table(), id, taz_v1_TaskState_TASK_STATE_RUNNING);
    const detached_task_t *slot = taz_task_find(Table(), id);
    ASSERT_NE(slot, nullptr);
    EXPECT_EQ(slot->state, taz_v1_TaskState_TASK_STATE_RUNNING);
    EXPECT_EQ(slot->finish_seq, 0U);

    taz_task_set_state(Table(), id, taz_v1_TaskState_TASK_STATE_COMPLETED);
    EXPECT_EQ(slot->state, taz_v1_TaskState_TASK_STATE_COMPLETED);
    const uint64_t first_seq = slot->finish_seq;
    EXPECT_NE(first_seq, 0U);

    // A second terminal transition (should not happen in practice, but
    // must not reorder eviction) leaves finish_seq untouched.
    taz_task_set_state(Table(), id, taz_v1_TaskState_TASK_STATE_FAILED);
    EXPECT_EQ(slot->finish_seq, first_seq);
}

TEST_F(TaskTableTest, SetStateOnUnknownTaskIsANoOp)
{
    taz_task_set_state(Table(), 123U, taz_v1_TaskState_TASK_STATE_RUNNING);
    EXPECT_EQ(taz_task_find(Table(), 123U), nullptr);
}

TEST_F(TaskTableTest, SetPidAndExecUpdateTheSlot)
{
    const uint32_t id = taz_task_alloc(
        Table(), 1U, taz_v1_ErrorPriority_ERROR_PRIORITY_NONE, true);
    ASSERT_NE(id, 0U);

    taz_task_set_pid(Table(), id, 4242U, 99U);
    const detached_task_t *slot = taz_task_find(Table(), id);
    ASSERT_NE(slot, nullptr);
    EXPECT_EQ(slot->pid, 4242U);
    EXPECT_EQ(slot->start_key, 99U);

    auto *fake_exec = reinterpret_cast<taz_exec_t *>(0x1234);
    taz_task_set_exec(Table(), id, fake_exec);
    EXPECT_EQ(slot->exec, fake_exec);

    // on_done drops the pointer once the exec has finished.
    taz_task_set_exec(Table(), id, nullptr);
    EXPECT_EQ(slot->exec, nullptr);
}

TEST_F(TaskTableTest, SetPidAndExecOnUnknownTaskAreNoOps)
{
    taz_task_set_pid(Table(), 7U, 1U, 1U);
    taz_task_set_exec(Table(), 7U, reinterpret_cast<taz_exec_t *>(0x1));
    EXPECT_EQ(taz_task_find(Table(), 7U), nullptr);
}

TEST_F(TaskTableTest, StoreResultCopiesBytesAndReplacesAnyPriorResult)
{
    const uint32_t id = taz_task_alloc(
        Table(), 1U, taz_v1_ErrorPriority_ERROR_PRIORITY_NONE, true);
    ASSERT_NE(id, 0U);

    const uint8_t first[] = {1, 2, 3};
    taz_task_store_result(Table(), id, first, sizeof(first));
    const detached_task_t *slot = taz_task_find(Table(), id);
    ASSERT_NE(slot, nullptr);
    ASSERT_NE(slot->result, nullptr);
    ASSERT_EQ(slot->result_len, sizeof(first));
    EXPECT_EQ(std::memcmp(slot->result, first, sizeof(first)), 0);

    const uint8_t second[] = {9, 9};
    taz_task_store_result(Table(), id, second, sizeof(second));
    ASSERT_NE(slot->result, nullptr);
    ASSERT_EQ(slot->result_len, sizeof(second));
    EXPECT_EQ(std::memcmp(slot->result, second, sizeof(second)), 0);

    taz_task_store_result(Table(), id, nullptr, 0U);
    EXPECT_EQ(slot->result, nullptr);
    EXPECT_EQ(slot->result_len, 0U);
}

TEST_F(TaskTableTest, StoreResultOnUnknownTaskIsANoOp)
{
    const uint8_t data[] = {1};
    taz_task_store_result(Table(), 55U, data, sizeof(data));
    EXPECT_EQ(taz_task_find(Table(), 55U), nullptr);
}

TEST_F(TaskTableTest, StoreErrorRecordsCodeAndTruncatesAnOverlongMessage)
{
    const uint32_t id = taz_task_alloc(
        Table(), 1U, taz_v1_ErrorPriority_ERROR_PRIORITY_NONE, true);
    ASSERT_NE(id, 0U);

    taz_task_store_error(Table(), id, taz_v1_ErrorCode_ERROR_CODE_TIMEOUT,
                         "timed out");
    const detached_task_t *slot = taz_task_find(Table(), id);
    ASSERT_NE(slot, nullptr);
    EXPECT_EQ(slot->error_code, taz_v1_ErrorCode_ERROR_CODE_TIMEOUT);
    EXPECT_STREQ(slot->error_message, "timed out");

    const std::string overlong(
        static_cast<std::string::size_type>(TAZ_TASK_ERROR_MESSAGE_MAX) * 4U,
        'x');
    taz_task_store_error(Table(), id, taz_v1_ErrorCode_ERROR_CODE_INTERNAL,
                         overlong.c_str());
    EXPECT_EQ(slot->error_code, taz_v1_ErrorCode_ERROR_CODE_INTERNAL);
    EXPECT_LT(std::strlen(slot->error_message), TAZ_TASK_ERROR_MESSAGE_MAX);

    taz_task_store_error(Table(), id, taz_v1_ErrorCode_ERROR_CODE_INTERNAL,
                         nullptr);
    EXPECT_STREQ(slot->error_message, "");
}

TEST_F(TaskTableTest, StoreErrorOnUnknownTaskIsANoOp)
{
    taz_task_store_error(Table(), 55U, taz_v1_ErrorCode_ERROR_CODE_INTERNAL,
                         "x");
    EXPECT_EQ(taz_task_find(Table(), 55U), nullptr);
}

TEST_F(TaskTableTest, FreeOnUnknownTaskIsANoOp)
{
    taz_task_free(Table(), 55U); // never allocated; must not crash
    SUCCEED();
}

// --- Retention table (context.md) ------------------------------------------

TEST_F(TaskTableTest, MonitorFalseTaskFreedImmediatelyOnFinishAnswersNotFound)
{
    const uint32_t id = taz_task_alloc(
        Table(), 1U, taz_v1_ErrorPriority_ERROR_PRIORITY_NONE, false);
    ASSERT_NE(id, 0U);

    taz_task_set_state(Table(), id, taz_v1_TaskState_TASK_STATE_COMPLETED);
    ASSERT_NE(taz_task_find(Table(), id),
              nullptr);         // still pollable until freed
    taz_task_free(Table(), id); // the DETACH handler's finish path

    EXPECT_EQ(taz_task_find(Table(), id), nullptr);
}

TEST_F(TaskTableTest, MonitorTrueTaskStaysPollableAfterFinishing)
{
    const uint32_t id = taz_task_alloc(
        Table(), 1U, taz_v1_ErrorPriority_ERROR_PRIORITY_NONE, true);
    ASSERT_NE(id, 0U);

    taz_task_set_state(Table(), id, taz_v1_TaskState_TASK_STATE_COMPLETED);

    const detached_task_t *slot = taz_task_find(Table(), id);
    ASSERT_NE(slot, nullptr);
    EXPECT_EQ(slot->state, taz_v1_TaskState_TASK_STATE_COMPLETED);
}

TEST_F(TaskTableTest, AllocEvictsTheOldestFinishedMonitorTrueSlotWhenFull)
{
    std::vector<uint32_t> ids;
    for (uint32_t i = 0; i < TAZ_TASK_SLOTS; i++)
    {
        const uint32_t id = taz_task_alloc(
            Table(), i, taz_v1_ErrorPriority_ERROR_PRIORITY_NONE, true);
        ASSERT_NE(id, 0U) << "i=" << i;
        ids.push_back(id);
    }

    // Finish three of them out of allocation order, so finish order (not
    // allocation order) must drive eviction order.
    taz_task_set_state(Table(), ids[10], taz_v1_TaskState_TASK_STATE_COMPLETED);
    taz_task_set_state(Table(), ids[3], taz_v1_TaskState_TASK_STATE_FAILED);
    taz_task_set_state(Table(), ids[20], taz_v1_TaskState_TASK_STATE_CANCELLED);

    // Table is full; the oldest *finished* slot (ids[10]) is evicted first.
    const uint32_t replacement1 = taz_task_alloc(
        Table(), 100U, taz_v1_ErrorPriority_ERROR_PRIORITY_NONE, true);
    ASSERT_NE(replacement1, 0U);
    EXPECT_EQ(taz_task_find(Table(), ids[10]), nullptr);
    EXPECT_NE(taz_task_find(Table(), ids[3]), nullptr);
    EXPECT_NE(taz_task_find(Table(), ids[20]), nullptr);

    const uint32_t replacement2 = taz_task_alloc(
        Table(), 101U, taz_v1_ErrorPriority_ERROR_PRIORITY_NONE, true);
    ASSERT_NE(replacement2, 0U);
    EXPECT_EQ(taz_task_find(Table(), ids[3]), nullptr);
    EXPECT_NE(taz_task_find(Table(), ids[20]), nullptr);

    const uint32_t replacement3 = taz_task_alloc(
        Table(), 102U, taz_v1_ErrorPriority_ERROR_PRIORITY_NONE, true);
    ASSERT_NE(replacement3, 0U);
    EXPECT_EQ(taz_task_find(Table(), ids[20]), nullptr);

    // Every other still-running task survived untouched.
    for (size_t i = 0; i < ids.size(); i++)
    {
        if (i == 10U || i == 3U || i == 20U)
        {
            continue;
        }
        EXPECT_NE(taz_task_find(Table(), ids[i]), nullptr) << "i=" << i;
    }
}

TEST_F(TaskTableTest, AllocAnswersBusyWhenEveryFinishedSlotHasMonitorFalse)
{
    std::vector<uint32_t> ids;
    for (uint32_t i = 0; i < TAZ_TASK_SLOTS; i++)
    {
        const uint32_t id = taz_task_alloc(
            Table(), i, taz_v1_ErrorPriority_ERROR_PRIORITY_NONE, false);
        ASSERT_NE(id, 0U) << "i=" << i;
        ids.push_back(id);
    }

    // A monitor=false slot that finished but has not yet been freed by its
    // handler is not an eviction candidate - only still-running slots and
    // such a slot are left, so the table is effectively BUSY.
    taz_task_set_state(Table(), ids[0], taz_v1_TaskState_TASK_STATE_COMPLETED);

    EXPECT_EQ(taz_task_alloc(Table(), 999U,
                             taz_v1_ErrorPriority_ERROR_PRIORITY_NONE, true),
              0U);
}

TEST_F(TaskTableTest, AllocAnswersBusyOnlyWhenEverySlotIsStillRunning)
{
    for (uint32_t i = 0; i < TAZ_TASK_SLOTS; i++)
    {
        const uint32_t id = taz_task_alloc(
            Table(), i, taz_v1_ErrorPriority_ERROR_PRIORITY_NONE, true);
        ASSERT_NE(id, 0U) << "i=" << i;
        taz_task_set_state(Table(), id, taz_v1_TaskState_TASK_STATE_RUNNING);
    }

    EXPECT_EQ(taz_task_alloc(Table(), 999U,
                             taz_v1_ErrorPriority_ERROR_PRIORITY_NONE, true),
              0U);
}

TEST_F(TaskTableTest, AllocSucceedsAgainOnceASingleRunningTaskFinishes)
{
    std::vector<uint32_t> ids;
    for (uint32_t i = 0; i < TAZ_TASK_SLOTS; i++)
    {
        const uint32_t id = taz_task_alloc(
            Table(), i, taz_v1_ErrorPriority_ERROR_PRIORITY_NONE, true);
        ASSERT_NE(id, 0U) << "i=" << i;
        ids.push_back(id);
    }

    ASSERT_EQ(taz_task_alloc(Table(), 999U,
                             taz_v1_ErrorPriority_ERROR_PRIORITY_NONE, true),
              0U);

    taz_task_set_state(Table(), ids[5], taz_v1_TaskState_TASK_STATE_COMPLETED);
    const uint32_t id = taz_task_alloc(
        Table(), 999U, taz_v1_ErrorPriority_ERROR_PRIORITY_NONE, true);
    EXPECT_NE(id, 0U);
    EXPECT_EQ(taz_task_find(Table(), ids[5]), nullptr);
}

TEST_F(TaskTableTest, HasRoomMatchesWhetherAllocWouldSucceed)
{
    EXPECT_TRUE(taz_task_has_room(Table()));

    std::vector<uint32_t> ids;
    for (uint32_t i = 0; i < TAZ_TASK_SLOTS; i++)
    {
        const uint32_t id = taz_task_alloc(
            Table(), i, taz_v1_ErrorPriority_ERROR_PRIORITY_NONE, true);
        ASSERT_NE(id, 0U) << "i=" << i;
        ids.push_back(id);
    }
    EXPECT_FALSE(taz_task_has_room(Table()));

    taz_task_set_state(Table(), ids[7], taz_v1_TaskState_TASK_STATE_COMPLETED);
    EXPECT_TRUE(taz_task_has_room(Table()));
    // has_room never evicts anything itself.
    EXPECT_NE(taz_task_find(Table(), ids[7]), nullptr);
}

TEST_F(TaskTableTest, FinishedTaskWithASubscriberIsNeverEvicted)
{
    std::vector<uint32_t> ids;
    for (uint32_t i = 0; i < TAZ_TASK_SLOTS; i++)
    {
        const uint32_t id = taz_task_alloc(
            Table(), i, taz_v1_ErrorPriority_ERROR_PRIORITY_NONE, true);
        ASSERT_NE(id, 0U) << "i=" << i;
        ids.push_back(id);
    }
    taz_task_set_state(Table(), ids[0], taz_v1_TaskState_TASK_STATE_COMPLETED);
    taz_task_set_state(Table(), ids[1], taz_v1_TaskState_TASK_STATE_COMPLETED);
    taz_task_subscribe(Table(), ids[0]);

    // ids[0] finished first but has a subscriber, so ids[1] goes instead.
    ASSERT_NE(taz_task_alloc(Table(), 100U,
                             taz_v1_ErrorPriority_ERROR_PRIORITY_NONE, true),
              0U);
    EXPECT_NE(taz_task_find(Table(), ids[0]), nullptr);
    EXPECT_EQ(taz_task_find(Table(), ids[1]), nullptr);

    // Nothing else is evictable: BUSY until the subscriber leaves.
    EXPECT_FALSE(taz_task_has_room(Table()));
    taz_task_unsubscribe(Table(), ids[0]);
    EXPECT_TRUE(taz_task_has_room(Table()));
}

TEST_F(TaskTableTest, LastUnsubscribeFreesAFinishedMonitorFalseTask)
{
    const uint32_t id = taz_task_alloc(
        Table(), 1U, taz_v1_ErrorPriority_ERROR_PRIORITY_NONE, false);
    ASSERT_NE(id, 0U);
    taz_task_subscribe(Table(), id);
    taz_task_subscribe(Table(), id);

    // Still running: dropping a subscriber keeps it.
    taz_task_unsubscribe(Table(), id);
    ASSERT_NE(taz_task_find(Table(), id), nullptr);

    taz_task_subscribe(Table(), id);
    taz_task_set_state(Table(), id, taz_v1_TaskState_TASK_STATE_COMPLETED);
    taz_task_unsubscribe(Table(), id);
    EXPECT_NE(taz_task_find(Table(), id), nullptr);
    taz_task_unsubscribe(Table(), id);
    EXPECT_EQ(taz_task_find(Table(), id), nullptr);
}

TEST_F(TaskTableTest, UnsubscribeKeepsAFinishedMonitorTrueTask)
{
    const uint32_t id = taz_task_alloc(
        Table(), 1U, taz_v1_ErrorPriority_ERROR_PRIORITY_NONE, true);
    ASSERT_NE(id, 0U);
    taz_task_subscribe(Table(), id);
    taz_task_set_state(Table(), id, taz_v1_TaskState_TASK_STATE_FAILED);
    taz_task_unsubscribe(Table(), id);

    const detached_task_t *slot = taz_task_find(Table(), id);
    ASSERT_NE(slot, nullptr);
    EXPECT_EQ(slot->subscribers, 0U);
}

TEST_F(TaskTableTest, SubscribeAndUnsubscribeOnUnknownOrUnsubscribedAreNoOps)
{
    taz_task_subscribe(Table(), 7U);
    taz_task_unsubscribe(Table(), 7U);

    const uint32_t id = taz_task_alloc(
        Table(), 1U, taz_v1_ErrorPriority_ERROR_PRIORITY_NONE, false);
    ASSERT_NE(id, 0U);
    taz_task_set_state(Table(), id, taz_v1_TaskState_TASK_STATE_COMPLETED);
    // No subscriber to drop: the count never wraps and nothing is freed.
    taz_task_unsubscribe(Table(), id);
    const detached_task_t *slot = taz_task_find(Table(), id);
    ASSERT_NE(slot, nullptr);
    EXPECT_EQ(slot->subscribers, 0U);
}

TEST_F(TaskTableTest, TableFreeReleasesEveryResultAndEmptiesTheTable)
{
    const uint8_t bytes[] = {1, 2, 3, 4};
    std::vector<uint32_t> ids;
    for (uint32_t i = 0; i < 3U; i++)
    {
        const uint32_t id = taz_task_alloc(
            Table(), i, taz_v1_ErrorPriority_ERROR_PRIORITY_NONE, true);
        ASSERT_NE(id, 0U);
        taz_task_store_result(Table(), id, bytes, sizeof(bytes));
        ids.push_back(id);
    }

    // The sanitizer and valgrind lanes catch a result left behind.
    taz_task_table_free(Table());
    for (const uint32_t id : ids)
    {
        EXPECT_EQ(taz_task_find(Table(), id), nullptr);
    }
    EXPECT_TRUE(taz_task_has_room(Table()));
}
