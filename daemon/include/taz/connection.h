#ifndef TAZ_CONNECTION_H
#define TAZ_CONNECTION_H

#include <stdint.h>

#include <uv.h>

#include "taz/dispatch.h"
#include "taz/frame.h"
#include "taz/reassembly.h"

#ifdef __cplusplus
extern "C"
{
#endif

/* libuv read buffer size per connection. */
#define TAZ_CONN_READ_BUF_SIZE 4096U

    /* Per-connection state.  The handle member must come first so that a
     * taz_conn_t * can be cast to uv_tcp_t *, uv_stream_t *, or
     * uv_handle_t *. */
    typedef struct taz_conn_s
    {
        uv_tcp_t handle;
        taz_reassembly_state_t reassembly;
        taz_dispatch_t dispatch;
        unsigned int refcount; /* 1 = alive; uv_write increments, done/close
                                  decrements */
        int closing;           /* 1 once teardown begins; gates new writes */
        /* Links in connection.c's list of every connection not yet freed,
         * which the SIGTERM tree kill walks. */
        struct taz_conn_s *prev;
        struct taz_conn_s *next;
        uint8_t read_buf[TAZ_CONN_READ_BUF_SIZE];
    } taz_conn_t;

    /* Write the CAPABILITY frame sent first on every connection (header and
     * payload, stream_id 0, opcode 0) into buf. Returns its length, or 0 if
     * buf is too small or encoding fails. */
    size_t taz_conn_capability_frame(uint8_t *buf, size_t bufsize);

    /* Passed to uv_listen as on_connect: accepts, allocates taz_conn_t,
     * sends CAPABILITY, starts reading. */
    void taz_conn_on_new_connection(uv_stream_t *server, int status);

    /* Gracefully close every connection open on loop that is not already
     * closing: mark it closing, stop reads, cancel every in-flight exec
     * and detached task tree and abort every active stream
     * (taz_dispatch_cancel_all), then uv_shutdown (FIN) followed by
     * uv_close once that completes (or immediately, on a synchronous
     * uv_shutdown error). Part of the shutdown walk; a no-op if no
     * connection is open on loop. */
    void taz_conn_shutdown_all(const uv_loop_t *loop);

    /* Force-close every connection open on loop that has not finished
     * closing yet (uv_is_closing false): a peer that never drains its read
     * side can leave taz_conn_shutdown_all's uv_shutdown pending forever,
     * so the shutdown walk's drain deadline calls this to bound the wait.
     * Pending writes complete with UV_ECANCELED, same as any cancelled
     * write. A no-op if every connection is already closing. */
    void taz_conn_force_close_all(const uv_loop_t *loop);

    /* Number of connections open on loop (in g_conns, whether closing or
     * not): tests only. */
    size_t taz_conn_open_count(const uv_loop_t *loop);

    /* Process one frame event from the reassembly state machine.
     * Exposed for unit tests; production code uses a static wrapper.
     * write_fn delivers outgoing frames; *close_out is set to 1 when the
     * connection must be terminated (OVERSIZED verdict). */
    void taz_conn_handle_frame(taz_dispatch_t *d,
                               const taz_frame_header_t *header,
                               const uint8_t *payload,
                               taz_frame_verdict_t verdict,
                               taz_dispatch_write_fn_t write_fn, void *ctx,
                               int *close_out);

#ifdef __cplusplus
}
#endif

#endif /* TAZ_CONNECTION_H */
