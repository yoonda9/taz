#ifndef TAZ_HANDLERS_RUN_AS_H
#define TAZ_HANDLERS_RUN_AS_H

#include <stdint.h>

#include "taz/dispatch.h"
#include "taz/frame.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /* Async RUN_AS handler: not privileged -> NOT_SUPPORTED (defence in
     * depth; a non-privileged daemon never advertises RUN_AS either).
     * Privileged + user == "" -> reset the connection identity
     * (taz_run_as_reset) and respond success with the daemon's own user
     * name (taz_run_as_daemon_user) as effective_user. Privileged +
     * non-empty user -> resolve it with taz_run_as_resolve (passwd lookup
     * on POSIX, S4U logon on Windows); its error code and message on a
     * failure, otherwise store the identity (and Windows token) on d,
     * replacing any previous one, and respond success with the sanitized
     * effective_user. Responds and calls taz_dispatch_stream_done
     * synchronously, before returning, so the identity is in force for
     * the connection's next frame. */
    void handle_run_as(taz_dispatch_t *d, const taz_frame_header_t *header,
                       const uint8_t *payload, taz_dispatch_write_fn_t write_fn,
                       void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* TAZ_HANDLERS_RUN_AS_H */
