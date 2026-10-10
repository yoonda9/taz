#ifndef TAZ_DISPATCH_H
#define TAZ_DISPATCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <uv.h>

#include "taz/exec.h"
#include "taz/frame.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /* Maximum number of concurrently active REQUEST streams per connection. */
#define TAZ_DISPATCH_MAX_STREAMS 64U

    /* Callback invoked by the dispatcher to write a packed frame (header +
     * payload) back to the peer.  data is valid only for the duration of the
     * call; the callee MUST copy it if it needs to outlive the callback. */
    typedef void (*taz_dispatch_write_fn_t)(const uint8_t *data, size_t len,
                                            void *ctx);

    /* Pins/unpins the owning connection alive on behalf of an in-flight
     * async handler. ctx is conn_ctx below (the taz_conn_t * in
     * production). */
    typedef void (*taz_dispatch_conn_ref_fn_t)(void *ctx);

    /* Reports whether the owning connection has begun closing. ctx is
     * conn_ctx below (the taz_conn_t * in production). */
    typedef int (*taz_dispatch_conn_closing_fn_t)(void *ctx);

    /* Reports the connection's current outbound write-queue depth in bytes.
     * ctx is conn_ctx below. */
    typedef size_t (*taz_dispatch_conn_write_queue_size_fn_t)(void *ctx);

    /* Pause/resume reads on the connection (ingress backpressure). ctx is
     * conn_ctx below. */
    typedef void (*taz_dispatch_conn_pause_reads_fn_t)(void *ctx);
    typedef void (*taz_dispatch_conn_resume_reads_fn_t)(void *ctx);

    /* Invoked by a stream_ops_t::cancel implementation, exactly once, once
     * the cancellation it accepted has actually taken effect. */
    typedef void (*taz_stream_done_fn_t)(void *arg);

    /* Per-stream hooks for handlers that outlive a single request/response
     * (FILE_PUT/FILE_GET transfers). Registered via
     * taz_dispatch_set_stream_ops once the stream is active. */
    typedef struct taz_stream_ops_s
    {
        /* FILE_CHUNK delivered for this stream (loop thread). payload is
         * valid only during the call and NULL when header->length == 0. */
        void (*on_chunk)(void *user, const taz_frame_header_t *header,
                         const uint8_t *payload);
        /* CANCEL targets this stream. Return 1 and call done(done_arg)
         * exactly once (later or before returning) when accepted; return 0
         * (done never called) when the stream cannot be cancelled in its
         * current state. */
        int (*cancel)(void *user, taz_stream_done_fn_t done, void *done_arg);
        /* The connection is closing (from taz_dispatch_cancel_all). Release
         * everything via pool work; send nothing; must NOT call
         * taz_dispatch_stream_done synchronously. */
        void (*abort)(void *user);
        /* A write on the connection completed (loop thread). May be NULL. */
        void (*on_writable)(void *user);
    } taz_stream_ops_t;

    /* Per-connection dispatch state. */
    typedef struct taz_dispatch_s
    {
        uint32_t active_streams[TAZ_DISPATCH_MAX_STREAMS];
        /* stream_execs[i] is the in-flight exec (if any) owning
         * active_streams[i], kept at the same index; NULL for streams with
         * no exec (every sync stream, and async streams before the handler
         * has one to register). */
        taz_exec_t *stream_execs[TAZ_DISPATCH_MAX_STREAMS];
        /* stream_ops[i]/stream_user[i] are the ops/user pointers (if any)
         * registered via taz_dispatch_set_stream_ops for active_streams[i],
         * kept at the same index by the same swap-remove as stream_execs.
         * NULL for streams with no ops. */
        const taz_stream_ops_t *stream_ops[TAZ_DISPATCH_MAX_STREAMS];
        void *stream_user[TAZ_DISPATCH_MAX_STREAMS];
        size_t active_count;
        /* Connection-default timeout (ms) for COMMAND_EXEC, set by
         * TIMEOUT_SET; 0 means no default. handle_command_exec snapshots
         * this into its taz_exec_spec_t before taz_exec_start, so a later
         * TIMEOUT_SET never retargets an in-flight command. Zeroed by
         * taz_dispatch_init, which is what pure unit tests see. */
        uint32_t conn_timeout_ms;
        /* Loop async handlers (e.g. COMMAND_EXEC) spawn on. Set by
         * connection.c right after taz_dispatch_init; left NULL by
         * taz_dispatch_init itself, which is what pure unit tests see. */
        uv_loop_t *loop;
        /* conn_ref/conn_unref pin the owning connection alive for the
         * duration of an in-flight async handler (e.g. COMMAND_EXEC's
         * exec), so the connection can only be freed once every such
         * handler has finished. Set alongside loop by connection.c; left
         * NULL by taz_dispatch_init, in which case taz_dispatch_conn_ref/
         * _unref are no-ops, which is what pure unit tests see. */
        taz_dispatch_conn_ref_fn_t conn_ref;
        taz_dispatch_conn_ref_fn_t conn_unref;
        void *conn_ctx;
        /* Queried by work.c after a pool work item finishes, to decide
         * whether its completion may still write to the connection. Set
         * alongside conn_ref/conn_unref by connection.c; left NULL by
         * taz_dispatch_init, in which case taz_dispatch_conn_closing
         * reports 0, which is what pure unit tests see. */
        taz_dispatch_conn_closing_fn_t conn_closing;
        /* Backpressure hooks for long-lived stream_ops transfers. Set
         * alongside the conn_ref/conn_unref/conn_closing trio by
         * connection.c; left NULL by taz_dispatch_init, in which case
         * taz_dispatch_conn_write_queue_size reports 0 and
         * taz_dispatch_conn_pause_reads/_resume_reads are no-ops, which is
         * what pure unit tests see. */
        taz_dispatch_conn_write_queue_size_fn_t conn_write_queue_size;
        taz_dispatch_conn_pause_reads_fn_t conn_pause_reads;
        taz_dispatch_conn_resume_reads_fn_t conn_resume_reads;
    } taz_dispatch_t;

    /* Initialise a dispatch context to the empty state. */
    void taz_dispatch_init(taz_dispatch_t *d);

    /* Process one frame event delivered by the reassembly state machine.
     *
     *   TAZ_FRAME_OK:           route the frame to the appropriate handler.
     *   TAZ_FRAME_UNKNOWN_TYPE: send ERROR NOT_SUPPORTED and continue.
     *   TAZ_FRAME_OVERSIZED:    no-op (connection.c sends PROTOCOL_ERROR and
     *                           closes; the DONE reassembly phase prevents
     *                           further calls).
     *
     * write_fn is called synchronously to deliver outgoing frames. */
    void taz_dispatch_frame(taz_dispatch_t *d, const taz_frame_header_t *header,
                            const uint8_t *payload, taz_frame_verdict_t verdict,
                            taz_dispatch_write_fn_t write_fn, void *ctx);

    /* Remove stream_id from the active set after an async handler completes.
     * Synchronous handlers (e.g. VERSION) are removed by taz_dispatch_frame
     * automatically; async handlers (e.g. COMMAND_EXEC) call this themselves
     * once their response has been written. */
    void taz_dispatch_stream_done(taz_dispatch_t *d, uint32_t stream_id);

    /* Associate exec with stream_id so taz_dispatch_cancel_all can reach it.
     * A no-op if stream_id is not currently active (e.g. it already
     * finished). */
    void taz_dispatch_set_stream_exec(taz_dispatch_t *d, uint32_t stream_id,
                                      taz_exec_t *exec);

    /* Kill every exec currently registered via taz_dispatch_set_stream_exec.
     * Streams stay active until their own on_done fires
     * taz_dispatch_stream_done, same as any other cancellation. */
    void taz_dispatch_cancel_all(taz_dispatch_t *d);

    /* Pin/unpin the owning connection alive for an in-flight async handler.
     * No-ops when d->conn_ref/conn_unref is NULL (a pure unit-test
     * dispatch with no connection.c behind it). */
    void taz_dispatch_conn_ref(taz_dispatch_t *d);
    void taz_dispatch_conn_unref(taz_dispatch_t *d);

    /* 0 when conn_closing is unset (a pure unit-test dispatch with no
     * connection.c behind it); otherwise conn_closing(conn_ctx). */
    int taz_dispatch_conn_closing(const taz_dispatch_t *d);

    /* Register (or, with ops == NULL, clear) the stream_ops_t for an active
     * stream, along with the opaque user pointer passed to its callbacks.
     * A no-op when stream_id is not currently active. */
    void taz_dispatch_set_stream_ops(taz_dispatch_t *d, uint32_t stream_id,
                                     const taz_stream_ops_t *ops, void *user);

    /* Ask the ops registered on stream target to cancel. Returns 0 (done
     * never called) when target is not active, has no ops registered, or
     * its cancel callback refuses; returns 1 when accepted (done is called
     * exactly once, possibly before this function returns). */
    int taz_dispatch_cancel_stream(taz_dispatch_t *d, uint32_t target,
                                   taz_stream_done_fn_t done, void *arg);

    /* Notify every active stream with a non-NULL on_writable that a write
     * on the connection just completed. */
    void taz_dispatch_notify_writable(taz_dispatch_t *d);

    /* 0 when the hook is unset (a pure unit-test dispatch with no
     * connection.c behind it); otherwise conn_write_queue_size(conn_ctx). */
    size_t taz_dispatch_conn_write_queue_size(const taz_dispatch_t *d);

    /* No-ops when the respective hook is unset. */
    void taz_dispatch_conn_pause_reads(taz_dispatch_t *d);
    void taz_dispatch_conn_resume_reads(taz_dispatch_t *d);

    /* True if opcode's handler manages its own stream lifetime (does not get
     * closed automatically by taz_dispatch_frame); false for synchronous and
     * unknown opcodes. */
    bool taz_dispatch_opcode_is_async(uint16_t opcode);

#ifdef __cplusplus
}
#endif

#endif /* TAZ_DISPATCH_H */
