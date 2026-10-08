#ifndef TAZ_HANDLERS_CANCEL_H
#define TAZ_HANDLERS_CANCEL_H

#include <stdint.h>

#include "taz/dispatch.h"
#include "taz/frame.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /* Async CANCEL handler: decode CancelRequest; an undecodable payload is
     * INVALID_REQUEST. target_stream_id == 0 or the CANCEL's own stream_id
     * answers cancelled=false synchronously, as does any target that is not
     * active, has no stream ops registered (e.g. a COMMAND_EXEC stream,
     * which is bounded by timeout rather than CANCEL), or whose ops refuse
     * (taz_dispatch_cancel_stream returning 0). Otherwise the CANCEL stream
     * stays open until the target's ops call back, at which point
     * cancelled=true is sent (unless the connection is already closing) and
     * the CANCEL stream is released. Closes its own stream in every path. */
    void handle_cancel(taz_dispatch_t *d, const taz_frame_header_t *header,
                       const uint8_t *payload, taz_dispatch_write_fn_t write_fn,
                       void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* TAZ_HANDLERS_CANCEL_H */
