#ifndef TAZ_HANDLERS_PROCESS_H
#define TAZ_HANDLERS_PROCESS_H

#include <stddef.h>
#include <stdint.h>

#include <pb.h>

#include "taz/dispatch.h"
#include "taz/frame.h"
#include "taz/process.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /* NULL when pid is valid (1..INT32_MAX); otherwise the exact message
     * every pid-validating handler (KILL, INFO, MONITOR) sends as an
     * INVALID_REQUEST ERROR. The returned pointer is a string literal,
     * valid for the life of the program. */
    const char *taz_process_pid_check(uint32_t pid);

    /* Encodes msg (described by fields) and sends it as exactly one
     * RESPONSE frame with the given flags - never chunks, unlike
     * taz_response_send, and never clears CONTINUATION on its own: the
     * caller decides every frame's flags. Used for small, fixed-shape
     * messages (ProcessKillResponse, ProcessMonitorResponse) that are
     * always well under TAZ_FRAME_MAX_PAYLOAD_RESPONSE. A malloc/encode
     * failure drops the frame silently, matching
     * taz_process_list_send/taz_process_info_send. */
    void taz_process_send_frame(taz_dispatch_write_fn_t write_fn, void *ctx,
                                uint32_t stream_id, uint16_t opcode,
                                const pb_msgdesc_t *fields, const void *msg,
                                uint8_t flags);

    /* Async PROCESS_LIST handler: decode the request (an empty/absent
     * payload means filter == ""), then on the thread pool
     * taz_process_enumerate the host, filtered by a plain substring match
     * on name. Replies via taz_process_list_send with the matching
     * entries (zero matches is one RESPONSE with zero entries, never an
     * error) or an ERROR frame. Closes the stream itself
     * (taz_dispatch_stream_done), either synchronously on a decode
     * failure or later from the work-done callback. */
    void handle_process_list(taz_dispatch_t *d,
                             const taz_frame_header_t *header,
                             const uint8_t *payload,
                             taz_dispatch_write_fn_t write_fn, void *ctx);

    /* Loop thread (pure computation, no I/O): sends entries as one RESPONSE
     * frame per batch of at most 128 taz_v1_ProcessInfo
     * (taz_v1_ProcessListResponse's own max_count) - CONTINUATION set on
     * every frame but the last. count == 0 still sends one RESPONSE frame
     * with zero entries. Exposed separately from handle_process_list so a
     * large list can be exercised without actually running that many live
     * processes. */
    void taz_process_list_send(taz_dispatch_write_fn_t write_fn, void *ctx,
                               uint32_t stream_id, uint16_t opcode,
                               const taz_process_entry_t *entries,
                               size_t count);

    /* Inline PROCESS_KILL handler: decode the request, validate pid and
     * signal, then call taz_process_kill synchronously. Replies with a
     * ProcessKillResponse on success or an ERROR frame on the loop thread. */
    void handle_process_kill(const taz_frame_header_t *header,
                             const uint8_t *payload,
                             taz_dispatch_write_fn_t write_fn, void *ctx);

    /* Async PROCESS_INFO handler: decode the request, validate pid, then on
     * the thread pool taz_process_inspect the process. Replies via
     * taz_process_info_send with the detail or an ERROR frame. Closes the
     * stream itself (taz_dispatch_stream_done), either synchronously on a
     * decode failure or later from the work-done callback. */
    void handle_process_info(taz_dispatch_t *d,
                             const taz_frame_header_t *header,
                             const uint8_t *payload,
                             taz_dispatch_write_fn_t write_fn, void *ctx);

    /* Loop thread (pure computation, no I/O): sends detail (info, command
     * line, and open_files) as one or more RESPONSE frames - CONTINUATION
     * set on every frame but the last. Exposed separately from
     * handle_process_info so a large open_files array can be exercised
     * without actually opening that many files. */
    void taz_process_info_send(taz_dispatch_write_fn_t write_fn, void *ctx,
                               uint32_t stream_id, uint16_t opcode,
                               const taz_process_detail_t *detail);

#ifdef __cplusplus
}
#endif

#endif /* TAZ_HANDLERS_PROCESS_H */
