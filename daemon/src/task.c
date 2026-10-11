#include "taz/task.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool is_terminal(taz_v1_TaskState state)
{
    switch (state)
    {
        case taz_v1_TaskState_TASK_STATE_COMPLETED:
        case taz_v1_TaskState_TASK_STATE_FAILED:
        case taz_v1_TaskState_TASK_STATE_CANCELLED:
            return true;
        default:
            return false;
    }
}

static size_t find_free_slot(const taz_task_table_t *table)
{
    for (size_t i = 0; i < TAZ_TASK_SLOTS; i++)
    {
        if (table->slots[i].task_id == 0U)
        {
            return i;
        }
    }
    return TAZ_TASK_SLOTS;
}

/* The finished, monitor == true, unsubscribed slot with the smallest
 * nonzero finish_seq (the oldest), or TAZ_TASK_SLOTS if none is eligible. */
static size_t find_eviction_candidate(const taz_task_table_t *table)
{
    size_t oldest = TAZ_TASK_SLOTS;
    for (size_t i = 0; i < TAZ_TASK_SLOTS; i++)
    {
        const detached_task_t *slot = &table->slots[i];
        if (slot->task_id == 0U || !slot->monitor ||
            !is_terminal(slot->state) || slot->subscribers != 0U)
        {
            continue;
        }
        if (oldest == TAZ_TASK_SLOTS ||
            slot->finish_seq < table->slots[oldest].finish_seq)
        {
            oldest = i;
        }
    }
    return oldest;
}

bool taz_task_has_room(const taz_task_table_t *table)
{
    if (find_free_slot(table) < TAZ_TASK_SLOTS ||
        find_eviction_candidate(table) < TAZ_TASK_SLOTS)
    {
        return true;
    }
    return false;
}

uint32_t taz_task_alloc(taz_task_table_t *table, uint32_t stream_id,
                        taz_v1_ErrorPriority error_priority, bool monitor)
{
    size_t index = find_free_slot(table);
    if (index == TAZ_TASK_SLOTS)
    {
        index = find_eviction_candidate(table);
        if (index == TAZ_TASK_SLOTS)
        {
            return 0U; /* BUSY: no free slot and nothing evictable */
        }
        free(table->slots[index].result);
    }

    detached_task_t *slot = &table->slots[index];

    (void)memset(slot, 0, sizeof(*slot));
    slot->task_id = ++table->next_task_id;
    slot->stream_id = stream_id;
    slot->state = taz_v1_TaskState_TASK_STATE_PENDING;
    slot->error_priority = error_priority;
    slot->monitor = monitor;
    return slot->task_id;
}

detached_task_t *taz_task_find(taz_task_table_t *table, uint32_t task_id)
{
    if (task_id == 0U)
    {
        return NULL;
    }
    for (size_t i = 0; i < TAZ_TASK_SLOTS; i++)
    {
        if (table->slots[i].task_id == task_id)
        {
            return &table->slots[i];
        }
    }
    return NULL;
}

void taz_task_set_state(taz_task_table_t *table, uint32_t task_id,
                        taz_v1_TaskState state)
{
    detached_task_t *slot = taz_task_find(table, task_id);
    if (slot == NULL)
    {
        return;
    }
    slot->state = state;
    if (is_terminal(state) && slot->finish_seq == 0U)
    {
        slot->finish_seq = ++table->next_finish_seq;
    }
}

void taz_task_set_pid(taz_task_table_t *table, uint32_t task_id, uint32_t pid,
                      uint64_t start_key)
{
    detached_task_t *slot = taz_task_find(table, task_id);
    if (slot == NULL)
    {
        return;
    }
    slot->pid = pid;
    slot->start_key = start_key;
}

void taz_task_set_exec(taz_task_table_t *table, uint32_t task_id,
                       taz_exec_t *exec)
{
    detached_task_t *slot = taz_task_find(table, task_id);
    if (slot == NULL)
    {
        return;
    }
    slot->exec = exec;
}

void taz_task_store_result(taz_task_table_t *table, uint32_t task_id,
                           const uint8_t *result, size_t result_len)
{
    detached_task_t *slot = taz_task_find(table, task_id);
    if (slot == NULL)
    {
        return;
    }

    free(slot->result);
    slot->result = NULL;
    slot->result_len = 0U;
    if (result_len == 0U)
    {
        return;
    }

    uint8_t *copy = (uint8_t *)malloc(result_len);
    if (copy == NULL)
    {
        return; /* leave the slot's result empty rather than crashing */
    }
    (void)memcpy(copy, result, result_len);
    slot->result = copy;
    slot->result_len = result_len;
}

void taz_task_store_error(taz_task_table_t *table, uint32_t task_id,
                          taz_v1_ErrorCode code, const char *message)
{
    detached_task_t *slot = taz_task_find(table, task_id);
    if (slot == NULL)
    {
        return;
    }
    slot->error_code = code;
    (void)snprintf(slot->error_message, sizeof(slot->error_message), "%s",
                   (message != NULL) ? message : "");
}

void taz_task_set_cancel_requested(taz_task_table_t *table, uint32_t task_id)
{
    detached_task_t *slot = taz_task_find(table, task_id);
    if (slot == NULL)
    {
        return;
    }
    slot->cancel_requested = true;
}

void taz_task_free(taz_task_table_t *table, uint32_t task_id)
{
    detached_task_t *slot = taz_task_find(table, task_id);
    if (slot == NULL)
    {
        return;
    }
    free(slot->result);
    (void)memset(slot, 0, sizeof(*slot));
}

void taz_task_subscribe(taz_task_table_t *table, uint32_t task_id)
{
    detached_task_t *slot = taz_task_find(table, task_id);
    if (slot == NULL)
    {
        return;
    }
    slot->subscribers++;
}

void taz_task_unsubscribe(taz_task_table_t *table, uint32_t task_id)
{
    detached_task_t *slot = taz_task_find(table, task_id);
    if (slot == NULL || slot->subscribers == 0U)
    {
        return;
    }
    slot->subscribers--;
    if (slot->subscribers == 0U && !slot->monitor && is_terminal(slot->state))
    {
        taz_task_free(table, task_id);
    }
}

void taz_task_table_free(taz_task_table_t *table)
{
    for (size_t i = 0; i < TAZ_TASK_SLOTS; i++)
    {
        /* Free through a copy, as exec.c's free_env does: MSVC /analyze
         * treats every slots[i].result as one location, so a direct free
         * makes the next iteration's read look uninitialized (C6001). */
        uint8_t *result = table->slots[i].result;
        free(result);
    }
    (void)memset(table, 0, sizeof(*table));
}

void taz_task_table_abort(taz_task_table_t *table)
{
    for (size_t i = 0; i < TAZ_TASK_SLOTS; i++)
    {
        detached_task_t *slot = &table->slots[i];
        if (slot->task_id != 0U && slot->exec != NULL)
        {
            taz_exec_cancel(slot->exec);
        }
    }
}
