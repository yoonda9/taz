#include "detach.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pb_decode.h>
#include <uv.h>

#include "command.h"
#include "task_status.h"
#include "taz/config.h"
#include "taz/error.h"
#include "taz/log.h"
#include "taz/response.h"
#include "taz/task.h"
#include "taz/task_registry.h"
#include "taz/v1/advanced.pb.h"
#include "taz/v1/command.pb.h"
#include "taz/v1/common.pb.h"
#include "taz/work.h"

/* Carries what on_exec_done needs to update d->tasks and, on a
 * CRITICAL-priority FAILED finish, send a PRIORITY ERROR on the task's
 * originating stream. Heap-allocated per detached task, freed once
 * on_exec_done runs; d outlives it via the connection reference taken in
 * handle_detach. */
typedef struct
{
    taz_dispatch_t *d;
    taz_dispatch_write_fn_t write_fn;
    void *write_ctx;
    uint32_t stream_id; /* the DETACH request's stream_id */
    uint32_t task_id;
} detach_ctx_t;

/* The exec on-exit hook: fires at process exit, strictly before on_done,
 * so the exit-code registry already has the authoritative exit code by the
 * time any connection's PROCESS_MONITOR can observe it - independent of how
 * long on_done takes to fire (e.g. a grandchild still holding stdout open).
 * Recorded the way CommandExecResponse reports it: -signal for a process
 * killed by a signal. */
static void on_task_exit(uint32_t pid, uint64_t start_key, int64_t exit_status,
                         int term_signal, void *arg)
{
    (void)arg;
    taz_task_registry_record_exit(pid, start_key,
                                  (term_signal != 0) ? -(int64_t)term_signal
                                                     : exit_status);
}

static const char *task_state_name(taz_v1_TaskState state)
{
    switch (state)
    {
        case taz_v1_TaskState_TASK_STATE_COMPLETED:
            return "COMPLETED";
        case taz_v1_TaskState_TASK_STATE_FAILED:
            return "FAILED";
        case taz_v1_TaskState_TASK_STATE_CANCELLED:
            return "CANCELLED";
        default:
            return "UNKNOWN";
    }
}

static void on_exec_done(const taz_exec_result_t *result, void *arg)
{
    detach_ctx_t *dctx = (detach_ctx_t *)arg;
    taz_dispatch_t *d = dctx->d;
    detached_task_t *task = taz_task_find(&d->tasks, dctx->task_id);
    taz_v1_TaskState state;
    taz_v1_ErrorCode error_code = taz_v1_ErrorCode_ERROR_CODE_UNKNOWN;
    char error_message[TAZ_TASK_ERROR_MESSAGE_MAX];
    taz_v1_ErrorPriority priority;
    int closing;

    error_message[0] = '\0';

    if (task == NULL)
    {
        /* Only when handle_detach could not allocate a slot after the spawn
         * and cancelled the exec instead: a still-running task's slot is
         * never evicted or freed out from under an in-flight exec. */
        taz_dispatch_conn_unref(d);
        free(dctx);
        return;
    }

    /* Drop the exec pointer before anything else: on_done is about to
     * free it, so a racing TASK_CANCEL/TASK_STATUS must never see a
     * dangling reference past this point. */
    taz_task_set_exec(&d->tasks, dctx->task_id, NULL);

    /* result->cancelled is set only by a signal-0 (whole-tree) cancel;
     * task->cancel_requested also covers a nonzero-signal cancel, which
     * taz_exec_signal delivers without touching the exec's own cancelled
     * flag. Either one accepted by TASK_CANCEL reports CANCELLED once the
     * process actually exits. */
    if (result->cancelled || task->cancel_requested)
    {
        state = taz_v1_TaskState_TASK_STATE_CANCELLED;
    }
    else if (result->timed_out || result->term_signal != 0 ||
             result->exit_status != 0)
    {
        state = taz_v1_TaskState_TASK_STATE_FAILED;
    }
    else
    {
        state = taz_v1_TaskState_TASK_STATE_COMPLETED;
    }

    /* result is attached on COMPLETED and FAILED, never on CANCELLED. */
    if (state != taz_v1_TaskState_TASK_STATE_CANCELLED)
    {
        uint8_t *buf = NULL;
        size_t len = 0U;
        if (taz_command_encode_exec_response(result, &buf, &len))
        {
            taz_task_store_result(&d->tasks, dctx->task_id, buf, len);
            free(buf);
        }
    }

    if (state == taz_v1_TaskState_TASK_STATE_FAILED)
    {
        if (result->timed_out)
        {
            error_code = taz_v1_ErrorCode_ERROR_CODE_TIMEOUT;
        }
        else
        {
            error_code = taz_v1_ErrorCode_ERROR_CODE_INTERNAL;
        }
        if (result->term_signal != 0)
        {
            (void)snprintf(error_message, sizeof(error_message), "signal %d",
                           result->term_signal);
        }
        else
        {
            (void)snprintf(error_message, sizeof(error_message),
                           "exit code %lld", (long long)result->exit_status);
        }
        taz_task_store_error(&d->tasks, dctx->task_id, error_code,
                             error_message);
    }

    taz_log(TAZ_LOG_INFO, "task %u finished: state=%s pid=%u", dctx->task_id,
            task_state_name(state), task->pid);

    taz_task_set_state(&d->tasks, dctx->task_id, state);
    taz_task_status_on_task_finished(d, dctx->task_id);

    priority = task->error_priority;

    /* Checked (and, below, acted on) before conn_unref: conn_unref may free
     * the connection, and a CANCELLED task never escalates regardless. */
    closing = taz_dispatch_conn_closing(d);
    if (state == taz_v1_TaskState_TASK_STATE_FAILED &&
        priority == taz_v1_ErrorPriority_ERROR_PRIORITY_CRITICAL && !closing)
    {
        taz_error_send_priority(dctx->write_fn, dctx->write_ctx,
                                dctx->stream_id,
                                (uint16_t)taz_v1_Opcode_OPCODE_DETACH,
                                error_code, error_message, NULL);
    }

    /* A monitor=false task is freed as soon as it finishes, unless a
     * subscription still has to send it its final frame; the last
     * taz_task_unsubscribe frees it then. Its registry entry stays either
     * way, so PROCESS_MONITOR can still report the exit code. */
    if (!task->monitor && task->subscribers == 0U)
    {
        taz_task_free(&d->tasks, dctx->task_id);
    }

    taz_dispatch_conn_unref(d);
    free(dctx);
}

/* Sends an ERROR for the DETACH request, frees req (may be NULL) and
 * releases the request's stream. */
static void detach_fail(taz_dispatch_t *d, const taz_frame_header_t *header,
                        taz_dispatch_write_fn_t write_fn, void *ctx,
                        taz_v1_DetachRequest *req, taz_v1_ErrorCode code,
                        const char *message, const char *detail)
{
    free(req);
    taz_error_send(write_fn, ctx, header->stream_id, header->opcode, code,
                   message, detail);
    taz_dispatch_stream_done(d, header->stream_id);
}

void handle_detach(taz_dispatch_t *d, const taz_frame_header_t *header,
                   const uint8_t *payload, taz_dispatch_write_fn_t write_fn,
                   void *ctx)
{
    taz_v1_DetachRequest *req;
    taz_v1_CommandExecRequest *inner;
    const char *argv_ptrs[32];
    taz_exec_spec_t spec;
    taz_exec_t *exec_handle = NULL;
    detach_ctx_t *dctx;
    taz_v1_ErrorPriority error_priority;
    bool monitor;
    uint32_t task_id;
    pb_size_t i;
    int rc;

    /* ~39 KiB; never on the stack (MSVC /analyze C6262 flags stack frames
     * over 16 KiB, and Windows threads default to a 1 MiB stack). */
    req = (taz_v1_DetachRequest *)malloc(sizeof(*req));
    if (req == NULL)
    {
        detach_fail(d, header, write_fn, ctx, NULL,
                    taz_v1_ErrorCode_ERROR_CODE_INTERNAL, "out of memory",
                    NULL);
        return;
    }

    if (payload != NULL && header->length > 0U)
    {
        pb_istream_t istream =
            pb_istream_from_buffer(payload, (size_t)header->length);
        if (!pb_decode(&istream, taz_v1_DetachRequest_fields, req))
        {
            detach_fail(d, header, write_fn, ctx, req,
                        taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST,
                        "decode DetachRequest failed", NULL);
            return;
        }
    }
    else
    {
        (void)memset(req, 0, sizeof(*req));
    }

    /* Only the exec oneof is implemented; file_put/file_get and an unset
     * oneof all answer NOT_SUPPORTED and create no task. */
    if (req->which_task != taz_v1_DetachRequest_exec_tag)
    {
        detach_fail(d, header, write_fn, ctx, req,
                    taz_v1_ErrorCode_ERROR_CODE_NOT_SUPPORTED,
                    "DETACH only supports the exec task", NULL);
        return;
    }

    /* Once SIGTERM has started the shutdown, every task tree has already
     * been killed; a task spawned now would outlive the daemon. */
    if (taz_work_shutdown_requested())
    {
        detach_fail(d, header, write_fn, ctx, req,
                    taz_v1_ErrorCode_ERROR_CODE_BUSY, "daemon is shutting down",
                    NULL);
        return;
    }

    /* Both checked before spawning, and the slot itself is allocated (which
     * may evict a finished task) only once the spawn has succeeded, so a
     * request that fails never costs an existing task. Only this loop
     * thread allocates slots or registers pids, so both still hold after
     * the spawn below. */
    if (!taz_task_has_room(&d->tasks))
    {
        detach_fail(d, header, write_fn, ctx, req,
                    taz_v1_ErrorCode_ERROR_CODE_BUSY,
                    "detached task table is full", NULL);
        return;
    }
    if (!taz_task_registry_has_room())
    {
        detach_fail(d, header, write_fn, ctx, req,
                    taz_v1_ErrorCode_ERROR_CODE_BUSY,
                    "too many detached processes are running", NULL);
        return;
    }

    inner = &req->task.exec;
    for (i = 0; i < inner->args_count; i++)
    {
        argv_ptrs[i] = inner->args[i];
    }

    (void)memset(&spec, 0, sizeof(spec));
    spec.file = inner->command;
    spec.args = argv_ptrs;
    spec.args_count = (size_t)inner->args_count;
    spec.env = inner->env;
    spec.env_count = (size_t)inner->env_count;
    spec.cwd = inner->working_dir;
    /* A detached exec honours only its own timeout - never the
     * connection's TIMEOUT_SET default. */
    spec.timeout_ms = inner->timeout_ms;
    spec.max_output_bytes = taz_config_exec_max_output_bytes();

    {
        taz_v1_ErrorCode as_user_code = taz_v1_ErrorCode_ERROR_CODE_UNKNOWN;
        const char *as_user_message = NULL;

        if (taz_command_resolve_as_user(d, inner, &spec, &as_user_code,
                                        &as_user_message) != 0)
        {
            detach_fail(d, header, write_fn, ctx, req, as_user_code,
                        as_user_message, NULL);
            return;
        }
    }

    dctx = (detach_ctx_t *)malloc(sizeof(*dctx));
    if (dctx == NULL)
    {
        detach_fail(d, header, write_fn, ctx, req,
                    taz_v1_ErrorCode_ERROR_CODE_INTERNAL, "out of memory",
                    NULL);
        return;
    }
    dctx->d = d;
    dctx->write_fn = write_fn;
    dctx->write_ctx = ctx;
    dctx->stream_id = header->stream_id;
    dctx->task_id = 0U;

    error_priority = req->error_priority;
    monitor = req->monitor;
    rc = taz_exec_start(d->loop, &spec, on_exec_done, dctx, &exec_handle);
    free(req);

    if (rc != 0)
    {
        free(dctx);
        detach_fail(d, header, write_fn, ctx, NULL,
                    taz_command_map_spawn_error(rc), "spawn failed",
                    uv_strerror(rc));
        return;
    }

    /* Owns a reference on the connection until on_exec_done unrefs it,
     * mirroring handle_command_exec. on_exec_done cannot run before this
     * function returns to the loop, so dctx->task_id is set in time. */
    taz_dispatch_conn_ref(d);

    task_id =
        taz_task_alloc(&d->tasks, header->stream_id, error_priority, monitor);
    if (task_id == 0U)
    {
        /* Not reachable while taz_task_has_room held above; never leave a
         * process running without a task. on_exec_done sees no task and
         * just releases dctx and the connection reference. */
        taz_exec_cancel(exec_handle);
        detach_fail(d, header, write_fn, ctx, NULL,
                    taz_v1_ErrorCode_ERROR_CODE_BUSY,
                    "detached task table is full", NULL);
        return;
    }
    dctx->task_id = task_id;

    {
        const uint32_t pid = taz_exec_pid(exec_handle);
        uint64_t start_key = 0U;
        taz_v1_DetachResponse resp = taz_v1_DetachResponse_init_zero;

        /* The hook records the exit under the key it reads here, so the
         * registry entry uses that same key. An unreadable key (a platform
         * read failure) skips registration: PROCESS_MONITOR could not match
         * the pid without one anyway. */
        taz_exec_set_on_exit(exec_handle, on_task_exit, NULL);
        start_key = taz_exec_start_key(exec_handle);
        if (start_key != 0U)
        {
            (void)taz_task_registry_register(pid, start_key);
        }
        taz_task_set_pid(&d->tasks, task_id, pid, start_key);
        taz_task_set_exec(&d->tasks, task_id, exec_handle);
        taz_task_set_state(&d->tasks, task_id,
                           taz_v1_TaskState_TASK_STATE_RUNNING);

        resp.task_id = task_id;
        resp.pid = pid;
        taz_response_send(write_fn, ctx, header->stream_id, header->opcode,
                          taz_v1_DetachResponse_fields, &resp);
    }

    taz_dispatch_stream_done(d, header->stream_id);
}
