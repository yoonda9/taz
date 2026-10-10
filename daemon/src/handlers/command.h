#ifndef TAZ_HANDLERS_COMMAND_H
#define TAZ_HANDLERS_COMMAND_H

#include <stdint.h>

#include "taz/dispatch.h"
#include "taz/exec.h"
#include "taz/frame.h"
#include "taz/v1/command.pb.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /* Async COMMAND_EXEC handler: decode the request, resolve the identity
     * to spawn under via taz_command_resolve_as_user (answering its error
     * on a per-call as_user that is refused or unresolvable), then start
     * taz_exec_start on d->loop and register the resulting taz_exec_t with
     * the dispatch layer via taz_dispatch_set_stream_exec. Closes the
     * stream itself (taz_dispatch_stream_done) either synchronously, on a
     * decode/validation/spawn failure, or later from the exec's on_done
     * callback via taz_command_send_exec_response. */
    void handle_command_exec(taz_dispatch_t *d,
                             const taz_frame_header_t *header,
                             const uint8_t *payload,
                             taz_dispatch_write_fn_t write_fn, void *ctx);

    /* Resolves which identity (if any) taz_exec_start should switch to for
     * this COMMAND_EXEC, per the as_user field's "for this command only"
     * doc: a non-empty req->as_user is a per-call override - requires a
     * privileged daemon (else NOT_SUPPORTED) and a resolvable name (else
     * NOT_FOUND); an empty one falls back to the connection's RUN_AS
     * identity (d->run_as_active), and failing that, no identity switch at
     * all. Never spawns anything, so this can be unit tested directly: on
     * success (0) spec->uid/gid/switch_identity/identity_user/identity_home
     * are set accordingly (spec is otherwise untouched); on failure
     * (nonzero) *error_code and
     * *error_message are set and spec is left untouched. */
    int taz_command_resolve_as_user(const taz_dispatch_t *d,
                                    const taz_v1_CommandExecRequest *req,
                                    taz_exec_spec_t *spec,
                                    taz_v1_ErrorCode *error_code,
                                    const char **error_message);

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
