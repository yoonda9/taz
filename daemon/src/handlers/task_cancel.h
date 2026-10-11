#ifndef TAZ_HANDLERS_TASK_CANCEL_H
#define TAZ_HANDLERS_TASK_CANCEL_H

#include <stdint.h>

#include "taz/dispatch.h"
#include "taz/frame.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /* Async TASK_CANCEL handler: decode TaskCancelRequest (an undecodable
     * payload is INVALID_REQUEST). Unknown/evicted/freed task_id is
     * NOT_FOUND. A task that has already finished (its exec pointer
     * already dropped by on_done) replies accepted=false and touches no
     * handle. Otherwise: signal == 0 marks the task cancel_requested and
     * calls taz_exec_cancel (whole-tree SIGKILL on POSIX, TerminateJobObject
     * on Windows); a nonzero signal is sent to the process group via
     * taz_exec_signal (Windows ignores the value and terminates the job
     * regardless) - a signal taz_exec_signal reports as UV_EINVAL is
     * INVALID_REQUEST, anything else marks cancel_requested too. Either
     * accepted path replies accepted=true immediately; the task's state
     * only becomes CANCELLED later, in the exec's on_done, once the
     * process actually exits. Closes its own stream in every path. */
    void handle_task_cancel(taz_dispatch_t *d, const taz_frame_header_t *header,
                            const uint8_t *payload,
                            taz_dispatch_write_fn_t write_fn, void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* TAZ_HANDLERS_TASK_CANCEL_H */
