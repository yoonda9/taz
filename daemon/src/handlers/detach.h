#ifndef TAZ_HANDLERS_DETACH_H
#define TAZ_HANDLERS_DETACH_H

#include <stdint.h>

#include "taz/dispatch.h"
#include "taz/frame.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /* Async DETACH handler: decode the request, accept only its exec
     * oneof (file_put/file_get/no oneof set answer NOT_SUPPORTED), then
     * allocate a task slot and taz_exec_start on d->loop exactly as
     * handle_command_exec does for the inner CommandExecRequest, except
     * the spawn's timeout comes from the inner request alone (no
     * d->conn_timeout_ms fallback). On a successful spawn, replies with
     * DetachResponse{task_id, pid} immediately and closes its own stream
     * (taz_dispatch_stream_done) - the task then runs independently,
     * tracked in d->tasks until TASK_STATUS/TASK_CANCEL or its own
     * on_done frees it. A decode failure, an unsupported oneof, a full
     * task table (BUSY), an identity-resolution failure, or a spawn
     * failure all reply with an ERROR and close the stream the same way,
     * synchronously, without starting a task. */
    void handle_detach(taz_dispatch_t *d, const taz_frame_header_t *header,
                       const uint8_t *payload, taz_dispatch_write_fn_t write_fn,
                       void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* TAZ_HANDLERS_DETACH_H */
