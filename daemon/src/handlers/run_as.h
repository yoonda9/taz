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
     * depth; a non-privileged daemon never advertises RUN_AS either, and
     * this is also the only answer Windows ever gives, since
     * taz_run_as_privileged is always false there). Privileged + user == ""
     * -> reset the connection identity (d->run_as_active/uid/gid/user/home)
     * and respond success with the daemon's own user name
     * (taz_run_as_daemon_user) as effective_user. Privileged + non-empty
     * user -> resolve it via taz_passwd_lookup_by_name against
     * taz_run_as_passwd_path; NOT_FOUND on a miss, otherwise store
     * uid/gid/home on d and respond success with the sanitized
     * effective_user. Responds
     * and calls taz_dispatch_stream_done synchronously, before returning. */
    void handle_run_as(taz_dispatch_t *d, const taz_frame_header_t *header,
                       const uint8_t *payload, taz_dispatch_write_fn_t write_fn,
                       void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* TAZ_HANDLERS_RUN_AS_H */
