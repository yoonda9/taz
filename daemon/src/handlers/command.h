#ifndef TAZ_HANDLERS_COMMAND_H
#define TAZ_HANDLERS_COMMAND_H

#include <stdint.h>

#include "taz/dispatch.h"
#include "taz/exec.h"
#include "taz/frame.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /* Async COMMAND_EXEC handler: decode the request, reject a non-empty
     * as_user with NOT_SUPPORTED (RUN_AS support comes later), then start
     * taz_exec_start on d->loop and register the resulting taz_exec_t with
     * the dispatch layer via taz_dispatch_set_stream_exec. Closes the
     * stream itself (taz_dispatch_stream_done) either synchronously, on a
     * decode/validation/spawn failure, or later from the exec's on_done
     * callback via taz_command_send_exec_response. */
    void handle_command_exec(taz_dispatch_t *d,
                             const taz_frame_header_t *header,
                             const uint8_t *payload,
                             taz_dispatch_write_fn_t write_fn, void *ctx);

    /* Encode *result as a CommandExecResponse and send it via
     * taz_response_send (splitting into several RESPONSE frames when the
     * combined output exceeds the single-frame limit; see response.c).
     * exit_code is exit_status on a normal exit, -term_signal when the
     * process was killed by a signal (POSIX), or the terminate code when
     * a Windows Job Object killed the tree (already reflected in
     * exit_status, since term_signal is always 0 there). stdout_data /
     * stderr_data are omitted entirely when the corresponding length is
     * zero. */
    void taz_command_send_exec_response(taz_dispatch_write_fn_t write_fn,
                                        void *ctx, uint32_t stream_id,
                                        uint16_t opcode,
                                        const taz_exec_result_t *result);

#ifdef __cplusplus
}
#endif

#endif /* TAZ_HANDLERS_COMMAND_H */
