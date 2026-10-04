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

    /* Per-connection dispatch state. */
    typedef struct taz_dispatch_s
    {
        uint32_t active_streams[TAZ_DISPATCH_MAX_STREAMS];
        /* stream_execs[i] is the in-flight exec (if any) owning
         * active_streams[i], kept at the same index; NULL for streams with
         * no exec (every sync stream, and async streams before the handler
         * has one to register). */
        taz_exec_t *stream_execs[TAZ_DISPATCH_MAX_STREAMS];
        size_t active_count;
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
        /* Queried by taz_work.c after a pool work item finishes, to decide
         * whether its completion may still write to the connection. Set
         * alongside conn_ref/conn_unref by connection.c; left NULL by
         * taz_dispatch_init, in which case taz_dispatch_conn_closing
         * reports 0, which is what pure unit tests see. */
        taz_dispatch_conn_closing_fn_t conn_closing;
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

    /* True if opcode's handler manages its own stream lifetime (does not get
     * closed automatically by taz_dispatch_frame); false for synchronous and
     * unknown opcodes. */
    bool taz_dispatch_opcode_is_async(uint16_t opcode);

#ifdef __cplusplus
}
#endif

#endif /* TAZ_DISPATCH_H */
