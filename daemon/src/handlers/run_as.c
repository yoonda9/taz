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
    taz_v1_RunAsResponse resp = taz_v1_RunAsResponse_init_zero;

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
        unsigned long uid = 0UL;
        unsigned long gid = 0UL;
        /* Resolved into a scratch buffer, not straight into d->run_as_home:
         * the lookup can still fail (NOT_FOUND), which must leave any
         * existing identity on the connection untouched, and
         * taz_run_as_reset (below) zeroes run_as_home as part of clearing
         * the old identity before the new one is stored. */
        char home[TAZ_EXEC_HOME_MAX];

        if (!taz_passwd_lookup_by_name(taz_run_as_passwd_path(), req.user, &uid,
                                       &gid, home, sizeof(home)))
        {
            taz_error_send(write_fn, ctx, header->stream_id, header->opcode,
                           taz_v1_ErrorCode_ERROR_CODE_NOT_FOUND,
                           "user not found", NULL);
            taz_dispatch_stream_done(d, header->stream_id);
            return;
        }

        taz_run_as_reset(d);
        d->run_as_active = 1;
        d->run_as_uid = (uint32_t)uid;
        d->run_as_gid = (uint32_t)gid;
        (void)snprintf(d->run_as_home, sizeof(d->run_as_home), "%s", home);
        taz_fsutil_sanitize_utf8(req.user, strlen(req.user), d->run_as_user,
                                 sizeof(d->run_as_user));
    }

    resp.success = true;
    taz_fsutil_sanitize_utf8(d->run_as_user, strlen(d->run_as_user),
                             resp.effective_user, sizeof(resp.effective_user));

    taz_response_send(write_fn, ctx, header->stream_id, header->opcode,
                      taz_v1_RunAsResponse_fields, &resp);
    taz_dispatch_stream_done(d, header->stream_id);
}
