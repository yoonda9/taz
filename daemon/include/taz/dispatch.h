#ifndef TAZ_DISPATCH_H
#define TAZ_DISPATCH_H

#include <stddef.h>
#include <stdint.h>

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

    /* Per-connection dispatch state. */
    typedef struct taz_dispatch_s
    {
        uint32_t active_streams[TAZ_DISPATCH_MAX_STREAMS];
        size_t active_count;
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
     * automatically; this is for future async handlers. */
    void taz_dispatch_stream_done(taz_dispatch_t *d, uint32_t stream_id);

#ifdef __cplusplus
}
#endif

#endif /* TAZ_DISPATCH_H */
