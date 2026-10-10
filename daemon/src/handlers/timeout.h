#ifndef TAZ_HANDLERS_TIMEOUT_H
#define TAZ_HANDLERS_TIMEOUT_H

#include <stdint.h>

#include "taz/dispatch.h"
#include "taz/frame.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /* Async TIMEOUT_SET handler: decode TimeoutSetRequest (an empty/absent
     * payload decodes to timeout_ms 0, a valid "disable the default" - only
     * a genuine decode failure is INVALID_REQUEST), swap it with
     * d->conn_timeout_ms, and respond with the previous value. Responds and
     * calls taz_dispatch_stream_done synchronously, before returning. */
    void handle_timeout_set(taz_dispatch_t *d, const taz_frame_header_t *header,
                            const uint8_t *payload,
                            taz_dispatch_write_fn_t write_fn, void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* TAZ_HANDLERS_TIMEOUT_H */
