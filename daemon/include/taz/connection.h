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
        unsigned int refcount; /* 1 = alive; uv_write increments, done/close decrements */
        int closing;           /* 1 once teardown begins; gates new writes */
        uint8_t read_buf[TAZ_CONN_READ_BUF_SIZE];
    } taz_conn_t;

    /* Passed to uv_listen as on_connect: accepts, allocates taz_conn_t,
     * sends CAPABILITY, starts reading. */
    void taz_conn_on_new_connection(uv_stream_t *server, int status);

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
