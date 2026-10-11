#ifndef TAZ_HANDLERS_TASK_STATUS_H
#define TAZ_HANDLERS_TASK_STATUS_H

#include <stdint.h>

#include "taz/dispatch.h"
#include "taz/frame.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /* Below this (including 0), the daemon raises interval_ms to this
     * floor - the same clamp PROCESS_MONITOR applies. */
#define TAZ_TASK_STATUS_MIN_INTERVAL_MS 100U

    /* max(requested, TAZ_TASK_STATUS_MIN_INTERVAL_MS). */
    uint64_t taz_task_status_interval_ms(uint32_t requested);

    /* Async TASK_STATUS handler: decodes the request, looks task_id up in
     * d->tasks (NOT_FOUND if unknown, evicted, or already freed). A
     * non-subscribed request replies with one TaskStatusResponse - result
     * attached on COMPLETED/FAILED, error on FAILED - via taz_response_send
     * (which itself splits a large result across RESPONSE frames per
     * protocol §6.1) and closes its own stream immediately. A subscribed
     * request clamps interval_ms to TAZ_TASK_STATUS_MIN_INTERVAL_MS,
     * registers a stream (taking a slot, BUSY when the table is full) and a
     * uv_timer_t, and sends one TaskStatusResponse per tick - CONTINUATION
     * set, never carrying result - until the task reaches a terminal state
     * or CANCEL (0x0046) targets the stream; either way a final frame
     * (CONTINUATION clear, still no result) ends it, before the CANCEL's
     * reply in the second case. A subscribed task is never evicted or
     * freed, so the final frame always carries its state. Every path
     * handles the connection already closing the same way
     * PROCESS_MONITOR's stream does. */
    void handle_task_status(taz_dispatch_t *d, const taz_frame_header_t *header,
                            const uint8_t *payload,
                            taz_dispatch_write_fn_t write_fn, void *ctx);

    /* Called when task_id reaches a terminal state: every subscription on it
     * ticks on the next loop iteration instead of at its next interval, so
     * its final frame goes out right away (and a finished monitor=false
     * task, kept only for that frame, is freed right after). */
    void taz_task_status_on_task_finished(taz_dispatch_t *d, uint32_t task_id);

#ifdef __cplusplus
}
#endif

#endif /* TAZ_HANDLERS_TASK_STATUS_H */
