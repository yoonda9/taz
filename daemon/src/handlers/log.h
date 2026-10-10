#ifndef TAZ_HANDLERS_LOG_H
#define TAZ_HANDLERS_LOG_H

#include <stdint.h>

#include "taz/dispatch.h"
#include "taz/frame.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /* Inline LOG handler: decode LogRequest (an empty/absent payload means
     * level "" -> DEBUG, lines 0 -> all, since 0), parse level (unknown ->
     * INVALID_REQUEST, stream released), then taz_log_collect the matching
     * entries from the in-memory ring and reply with one or more
     * LogResponse RESPONSE frames (CONTINUATION set on every frame but the
     * last; zero matches is one RESPONSE with zero entries, never an
     * error). Synchronous: taz_dispatch_frame closes the stream itself. */
    void handle_log(const taz_frame_header_t *header, const uint8_t *payload,
                    taz_dispatch_write_fn_t write_fn, void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* TAZ_HANDLERS_LOG_H */
