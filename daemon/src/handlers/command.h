#ifndef TAZ_HANDLERS_COMMAND_H
#define TAZ_HANDLERS_COMMAND_H

#include <stdbool.h>
#include <stddef.h>
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
     * NOT_FOUND, via the platform-neutral taz_run_as_resolve) - on success
     * spec->token is the resolved identity's token (Windows: a fresh
     * primary HANDLE spec now owns; POSIX: always NULL). An empty as_user
     * falls back to the connection's RUN_AS identity (d->run_as_active):
     * POSIX unchanged; Windows duplicates d->run_as_token
     * (taz_run_as_token_dup) so spec gets its own handle for this spawn
     * only, never the connection's. Neither req->as_user nor an active
     * connection identity set means no identity switch at all. Never spawns
     * anything, so this can be unit tested directly: on success (0)
     * spec->uid/gid/switch_identity/identity_user/identity_home/token are
     * set accordingly (spec is otherwise untouched); on failure (nonzero)
     * *error_code and *error_message are set and spec is left untouched -
     * including spec->token, so a failing call never hands out a token the
     * caller must remember to close. error_message points either at a
     * literal or at message_buf (caller-owned, at least message_buf_size
     * bytes, used only when the real failure text does not fit a literal -
     * the resolve's own message on Windows, or taz_run_as_token_dup's on a
     * connection-identity duplication failure); callers must keep
     * message_buf alive at least as long as they read *error_message. */
    int taz_command_resolve_as_user(const taz_dispatch_t *d,
                                    const taz_v1_CommandExecRequest *req,
                                    taz_exec_spec_t *spec,
                                    taz_v1_ErrorCode *error_code,
                                    const char **error_message,
                                    char *message_buf, size_t message_buf_size);

    /* Maps a taz_exec_start spawn-failure return (a UV_E* code) to the
     * ErrorCode a caller sends back on its own request. Shared by
     * handle_command_exec and handle_detach so a spawn failure answers
     * COMMAND_EXEC and DETACH identically. */
    taz_v1_ErrorCode taz_command_map_spawn_error(int rc);

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

    /* Encode *result as a CommandExecResponse (the same content
     * taz_command_send_exec_response would send) into a newly malloc'd
     * buffer. On success, out_buf and out_len together describe the
     * encoding and the caller owns the returned buffer (free() it); a
     * fully-default encoding (exit 0, no output, no flags) legitimately
     * yields *out_buf == NULL and *out_len == 0. On failure (out of memory
     * or an encode error) returns false and leaves *out_buf and *out_len
     * untouched. */
    bool taz_command_encode_exec_response(const taz_exec_result_t *result,
                                          uint8_t **out_buf, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* TAZ_HANDLERS_COMMAND_H */
