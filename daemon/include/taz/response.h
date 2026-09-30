#ifndef TAZ_RESPONSE_H
#define TAZ_RESPONSE_H

#include <stdint.h>

#include <pb.h>

#include "taz/dispatch.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /* Encode msg (described by fields) as one or more RESPONSE frames and
     * deliver them via write_fn.  If the encoded size fits within the RESPONSE
     * payload limit the message is sent as a single frame.  Otherwise it is
     * split across multiple frames with CONTINUATION set on every frame except
     * the last, following protocol §6.1:
     *
     *   - Wire-type-2 (LEN) entries whose total encoding fits in one frame are
     *     kept atomic; those that exceed the limit are chunked as successive
     *     (tag, sub-length, sub-bytes) entries — correct for bytes/string
     *     fields whose receiver concatenates them.
     *   - VARINT, I32, and I64 entries (scalars) are deferred to the final
     *     frame so receivers that take the last value see the right scalars.
     *
     * Large message structs should be heap-allocated by the caller before
     * passing to this function. */
    void taz_response_send(taz_dispatch_write_fn_t write_fn, void *ctx,
                           uint32_t stream_id, uint16_t opcode,
                           const pb_msgdesc_t *fields, const void *msg);

#ifdef __cplusplus
}
#endif

#endif /* TAZ_RESPONSE_H */
