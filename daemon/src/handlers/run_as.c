#include "run_as.h"

#include <stdio.h>
#include <string.h>

#include <pb_decode.h>

#include "taz/error.h"
#include "taz/fsutil.h"
#include "taz/response.h"
#include "taz/run_as.h"
#include "taz/v1/advanced.pb.h"

void handle_run_as(taz_dispatch_t *d, const taz_frame_header_t *header,
                   const uint8_t *payload, taz_dispatch_write_fn_t write_fn,
                   void *ctx)
{
    taz_v1_RunAsRequest req = taz_v1_RunAsRequest_init_zero;

    if (payload != NULL && header->length > 0U)
    {
        pb_istream_t istream =
            pb_istream_from_buffer(payload, (size_t)header->length);
        if (!pb_decode(&istream, taz_v1_RunAsRequest_fields, &req))
        {
            taz_error_send(write_fn, ctx, header->stream_id, header->opcode,
                           taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST,
                           "decode RunAsRequest failed", NULL);
            taz_dispatch_stream_done(d, header->stream_id);
            return;
        }
    }

    if (taz_run_as_privileged() == 0)
    {
        taz_error_send(write_fn, ctx, header->stream_id, header->opcode,
                       taz_v1_ErrorCode_ERROR_CODE_NOT_SUPPORTED,
                       "RUN_AS requires a privileged daemon", NULL);
        taz_dispatch_stream_done(d, header->stream_id);
        return;
    }

    if (req.user[0] == '\0')
    {
        taz_v1_RunAsResponse resp = taz_v1_RunAsResponse_init_zero;

        taz_run_as_reset(d);

        resp.success = true;
        {
            char daemon_user[sizeof(resp.effective_user)];

            taz_run_as_daemon_user(daemon_user, sizeof(daemon_user));
            taz_fsutil_sanitize_utf8(daemon_user, strlen(daemon_user),
                                     resp.effective_user,
                                     sizeof(resp.effective_user));
        }

        taz_response_send(write_fn, ctx, header->stream_id, header->opcode,
                          taz_v1_RunAsResponse_fields, &resp);
        taz_dispatch_stream_done(d, header->stream_id);
        return;
    }

    {
        /* Resolved inline, on the loop thread, as for every other
         * identity change: a client may pipeline RUN_AS with the requests
         * it governs, so the identity must be in force before the
         * connection's next frame is dispatched. */
        taz_run_as_identity_t identity;
        taz_v1_ErrorCode code = taz_v1_ErrorCode_ERROR_CODE_INTERNAL;
        char message[256];
        taz_v1_RunAsResponse resp = taz_v1_RunAsResponse_init_zero;

        (void)memset(&identity, 0, sizeof(identity));
        if (taz_run_as_resolve(req.user, &identity, &code, message,
                               sizeof(message)) != 0)
        {
            /* POSIX's taz_run_as_resolve leaves message "" on a passwd
             * miss; Windows always fills it with the real LsaLogonUser/
             * DuplicateTokenEx failure text. */
            taz_error_send(
                write_fn, ctx, header->stream_id, header->opcode, code,
                (message[0] != '\0') ? message : "user not found", NULL);
            taz_dispatch_stream_done(d, header->stream_id);
            return;
        }

        taz_run_as_reset(d);
        d->run_as_active = 1;
        d->run_as_uid = identity.uid;
        d->run_as_gid = identity.gid;
        (void)snprintf(d->run_as_user, sizeof(d->run_as_user), "%s",
                       identity.user);
        (void)snprintf(d->run_as_home, sizeof(d->run_as_home), "%s",
                       identity.home);
        d->run_as_token = identity.token;

        resp.success = true;
        (void)snprintf(resp.effective_user, sizeof(resp.effective_user), "%s",
                       d->run_as_user);

        taz_response_send(write_fn, ctx, header->stream_id, header->opcode,
                          taz_v1_RunAsResponse_fields, &resp);
        taz_dispatch_stream_done(d, header->stream_id);
    }
}
