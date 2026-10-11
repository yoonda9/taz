#ifndef TAZ_TASK_H
#define TAZ_TASK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "taz/exec.h"
#include "taz/v1/advanced.pb.h"
#include "taz/v1/common.pb.h"

/* Fixed number of detached-task slots per connection (the detailed
 * design's 2026-09-29 revision; no uthash, no new dependency). */
#define TAZ_TASK_SLOTS 64U

/* Big enough for every message this module stores ("exit code N" /
 * "signal N" for any int64_t/int). */
#define TAZ_TASK_ERROR_MESSAGE_MAX 64U

#ifdef __cplusplus
extern "C"
{
#endif

    /* One detached task. A zeroed slot (task_id == 0) is free; every other
     * field is only meaningful once task_id is nonzero. */
    typedef struct
    {
        uint32_t task_id;   /* 0 = slot free; otherwise unique for the
                             * connection's life */
        uint32_t stream_id; /* the originating DETACH request's stream_id */
        taz_v1_TaskState state;
        taz_v1_ErrorPriority error_priority;
        bool monitor;
        uint32_t pid;
        uint64_t start_key; /* taz_process_start_key(pid); 0 if unread */
        taz_exec_t *exec;   /* NULL before spawn and once on_done has run */
        uint8_t *result;    /* heap-owned encoded CommandExecResponse, or
                             * NULL */
        size_t result_len;
        taz_v1_ErrorCode error_code;
        char error_message[TAZ_TASK_ERROR_MESSAGE_MAX];
        /* Set once, when the task first reaches a terminal state
         * (COMPLETED/FAILED/CANCELLED); 0 while PENDING/RUNNING. Orders
         * eviction: taz_task_alloc evicts the finished, monitor-true slot
         * with the smallest nonzero finish_seq. */
        uint64_t finish_seq;
        /* Set by TASK_CANCEL once it accepts a cancel (either signal) for
         * this task, before the process has necessarily exited yet. The
         * task's on_done consults this - together with the exec result's
         * own cancelled flag, which only a signal-0 (whole-tree) cancel
         * sets - to report CANCELLED instead of FAILED once the process
         * exits as a result of either kind of accepted cancel. */
        bool cancel_requested;
        /* Open subscribed TASK_STATUS streams on this task. A slot with a
         * subscriber is never evicted, and a finished monitor == false task
         * is freed only once its last subscriber has had its final frame,
         * so a subscription always ends with the terminal state. */
        uint32_t subscribers;
    } detached_task_t;

    /* A connection's fixed-size task table. Zeroed by taz_dispatch_init,
     * which is a valid empty table: every slot free, no task_id ever
     * handed out. */
    typedef struct
    {
        detached_task_t slots[TAZ_TASK_SLOTS];
        uint32_t next_task_id;    /* monotonic; next id handed out is this
                                   * plus one, so 0 is never a real id */
        uint64_t next_finish_seq; /* monotonic finish-order counter */
    } taz_task_table_t;

    /* Allocate a task slot for a request arriving on stream_id, with the
     * given retention flag and PRIORITY policy. Tries a free slot first;
     * if none exists, evicts the oldest *finished* slot with monitor ==
     * true and no subscriber (the id that slot held now answers
     * taz_task_find with NULL).
     * Returns the new task_id (never 0) with state PENDING, or 0 if every
     * slot holds a task that is still running or pending (BUSY - the
     * caller sends that response and creates nothing). */
    uint32_t taz_task_alloc(taz_task_table_t *table, uint32_t stream_id,
                            taz_v1_ErrorPriority error_priority, bool monitor);

    /* The slot for task_id, or NULL if task_id is unknown, was evicted, or
     * was freed. */
    detached_task_t *taz_task_find(taz_task_table_t *table, uint32_t task_id);

    /* True if taz_task_alloc would succeed right now (a free slot, or a
     * slot it may evict). Lets a caller check for BUSY before doing work it
     * cannot undo, such as spawning, and allocate (evicting) only after
     * that work succeeded. Read-only. */
    bool taz_task_has_room(const taz_task_table_t *table);

    /* Move task_id's state forward. The first time it becomes a terminal
     * state (COMPLETED/FAILED/CANCELLED) its finish_seq is stamped for
     * eviction ordering. A no-op if task_id is unknown. */
    void taz_task_set_state(taz_task_table_t *table, uint32_t task_id,
                            taz_v1_TaskState state);

    /* Record the pid and taz_process_start_key of task_id's spawned
     * process. A no-op if task_id is unknown. */
    void taz_task_set_pid(taz_task_table_t *table, uint32_t task_id,
                          uint32_t pid, uint64_t start_key);

    /* Attach (or, with exec == NULL, detach) task_id's in-flight exec
     * handle - callers drop it here before anything else can reach it
     * once on_done has run. A no-op if task_id is unknown. */
    void taz_task_set_exec(taz_task_table_t *table, uint32_t task_id,
                           taz_exec_t *exec);

    /* Copy result_len bytes from result into task_id's slot, replacing
     * anything stored before. A no-op if task_id is unknown; on an
     * allocation failure the slot's result is left empty rather than
     * crashing. */
    void taz_task_store_result(taz_task_table_t *table, uint32_t task_id,
                               const uint8_t *result, size_t result_len);

    /* Record a FAILED task's error (code plus a message, truncated to fit
     * TAZ_TASK_ERROR_MESSAGE_MAX; NULL is stored as an empty message). A
     * no-op if task_id is unknown. */
    void taz_task_store_error(taz_task_table_t *table, uint32_t task_id,
                              taz_v1_ErrorCode code, const char *message);

    /* Mark task_id as having had a TASK_CANCEL accepted. A no-op if
     * task_id is unknown. */
    void taz_task_set_cancel_requested(taz_task_table_t *table,
                                       uint32_t task_id);

    /* Free task_id's slot (its result buffer and every field), making the
     * slot available for reuse and task_id answer NOT_FOUND from then on.
     * A no-op if task_id is unknown. */
    void taz_task_free(taz_task_table_t *table, uint32_t task_id);

    /* Count a subscribed TASK_STATUS stream on task_id. A no-op if task_id
     * is unknown. */
    void taz_task_subscribe(taz_task_table_t *table, uint32_t task_id);

    /* Drop one subscriber from task_id. When that was the last one and the
     * task is a finished monitor == false task, frees it (its finish had to
     * wait for this). A no-op if task_id is unknown. */
    void taz_task_unsubscribe(taz_task_table_t *table, uint32_t task_id);

    /* Free every slot's result buffer and empty the table. For the owning
     * connection's final release, once no exec or subscription can touch
     * the table again. */
    void taz_task_table_free(taz_task_table_t *table);

    /* Kill the process tree of every still-running task in the table (every
     * slot whose exec handle is non-NULL). Each affected task's own on_done
     * fires later, exactly as taz_exec_cancel's own contract already
     * promises; this function does not touch any slot's state or free
     * anything itself. A no-op on a table with nothing running. Used by
     * connection close and SIGTERM, both of which must not leave a detached
     * task's process tree running past the connection/daemon that owned it. */
    void taz_task_table_abort(taz_task_table_t *table);

#ifdef __cplusplus
}
#endif

#endif /* TAZ_TASK_H */
