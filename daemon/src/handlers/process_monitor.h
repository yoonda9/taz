#ifndef TAZ_HANDLERS_PROCESS_MONITOR_H
#define TAZ_HANDLERS_PROCESS_MONITOR_H

#include <stdint.h>

#include "taz/dispatch.h"
#include "taz/frame.h"
#include "taz/process.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /* Below this (including 0), the daemon raises interval_ms to this
     * floor. */
#define TAZ_PROCESS_MONITOR_MIN_INTERVAL_MS 100U

    /* max(requested, TAZ_PROCESS_MONITOR_MIN_INTERVAL_MS). */
    uint64_t taz_process_monitor_interval_ms(uint32_t requested);

    /* Unit-test hook (loop thread, call before dispatching a MONITOR
     * REQUEST): 0 makes every watch open with use_pidfd 0 regardless of
     * TAZ_MONITOR_NO_PIDFD; defaults to 1 (pidfd allowed, still subject to
     * the environment variable and to ENOSYS/EPERM at open time). */
    void taz_process_monitor_set_pidfd_enabled(int enabled);
    int taz_process_monitor_pidfd_enabled(void);

    /* The signature of taz_process_watch_sample. */
    typedef int (*taz_process_sample_fn_t)(const taz_process_watch_t *w,
                                           taz_process_sample_t *out,
                                           taz_v1_ErrorCode *code,
                                           const char **detail);

    /* Unit-test hook (loop thread): every later sample calls fn instead of
     * taz_process_watch_sample; NULL restores it. Each stream picks the
     * function up on the loop thread when it submits a sample, so swapping
     * it between ticks never races a pool thread. */
    void taz_process_monitor_set_sample_fn(taz_process_sample_fn_t fn);

    /* Async PROCESS_MONITOR handler: decodes the request, validates pid,
     * opens a taz_process_watch_t and streams ProcessMonitorResponse
     * RESPONSE frames - one immediately, then one per (clamped) interval -
     * on a uv_timer_t, plus (Linux, pidfd available) a uv_poll_t on the
     * watch's pidfd that notices the exit sooner. Ends the stream with
     * exactly one final RESPONSE (CONTINUATION clear, reason "exited",
     * "cancelled" or "error") or, for a failure before the first update,
     * one ERROR frame, and
     * closes it itself (taz_dispatch_stream_done), either synchronously on
     * a decode/validation failure or later from a timer/poll/work
     * callback. */
    void handle_process_monitor(taz_dispatch_t *d,
                                const taz_frame_header_t *header,
                                const uint8_t *payload,
                                taz_dispatch_write_fn_t write_fn, void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* TAZ_HANDLERS_PROCESS_MONITOR_H */
