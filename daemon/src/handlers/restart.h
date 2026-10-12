#ifndef TAZ_HANDLERS_RESTART_H
#define TAZ_HANDLERS_RESTART_H

#include <stdint.h>

#include "taz/dispatch.h"
#include "taz/frame.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /* Below this (including 0), the daemon raises delay_ms to this floor
     * so the response can be flushed before the process exits. */
#define TAZ_RESTART_MIN_DELAY_MS 100U

    /* Async RESTART handler: decode RestartRequest (an undecodable payload
     * is INVALID_REQUEST, no stream opened), refuse with BUSY once shutdown
     * has already begun (taz_work_shutdown_requested, same gate DETACH
     * uses) without opening a stream, otherwise clamp delay_ms up to
     * TAZ_RESTART_MIN_DELAY_MS and reply {acknowledged=true,
     * effective_delay_ms} via taz_response_send.
     *
     * The delay timer is armed only once the response has actually left
     * the connection's write queue (taz_dispatch_conn_write_queue_size
     * reaching 0, checked right after the response is sent and, if still
     * nonzero then, on every later on_writable) - api.md's "fully write the
     * response ... before terminating". Until then the stream stays open
     * with stream_ops: cancel always refuses (a restart cannot be cancelled
     * once acknowledged), and abort (the connection closing first) arms
     * the timer at once. Either way the stream then ends and the timer
     * carries on without the connection: an acknowledged restart happens
     * even if its requester disconnects. The daemon has one restart timer:
     * a later RESTART with a shorter delay moves it earlier, a longer one
     * leaves it be. When it fires it calls
     * taz_shutdown_request(TAZ_SHUTDOWN_RESTART). */
    void handle_restart(taz_dispatch_t *d, const taz_frame_header_t *header,
                        const uint8_t *payload,
                        taz_dispatch_write_fn_t write_fn, void *ctx);

    /* Close the daemon's restart timer (armed or not) and end every
     * RESTART stream still waiting for its response to be written. The
     * shutdown walk calls this: once a STOP has begun shutdown, a pending
     * restart may not keep the loop alive for the rest of its delay. Unit
     * tests call it to clean up a request they never let fire. */
    void taz_restart_close_pending(void);

#ifdef __cplusplus
}
#endif

#endif /* TAZ_HANDLERS_RESTART_H */
