#include "run_as.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pb_decode.h>

#include "taz/error.h"
#include "taz/fsutil.h"
#include "taz/response.h"
#include "taz/run_as.h"
#include "taz/v1/advanced.pb.h"
#include "taz/work.h"

/* Carries everything run_as_resolve_work/run_as_resolve_done need: the
 * request's name, snapshotted for the pool thread (the work function
 * touches only *user, never d), and the write sink/stream/opcode/
 * connection the loop thread needs once the resolve comes back.
 * Heap-allocated per in-flight named RUN_AS, freed once
 * run_as_resolve_done runs. */
typedef struct
{
    taz_dispatch_t *d;
    taz_dispatch_write_fn_t write_fn;
    void *write_ctx;
    uint32_t stream_id;
    uint16_t opcode;
    char name[TAZ_EXEC_USER_MAX];

    /* Filled in by run_as_resolve_work; read only by run_as_resolve_done. */
    int ok;
    taz_run_as_identity_t identity;
    taz_v1_ErrorCode code;
    char message[256];
} run_as_resolve_ctx_t;

static void run_as_resolve_work(void *user)
{
    run_as_resolve_ctx_t *rctx = (run_as_resolve_ctx_t *)user;

    rctx->ok = (taz_run_as_resolve(rctx->name, &rctx->identity, &rctx->code,
                                   rctx->message, sizeof(rctx->message)) == 0);
}

static void run_as_resolve_done(void *user, int closing)
{
    run_as_resolve_ctx_t *rctx = (run_as_resolve_ctx_t *)user;

    if (closing)
    {
        if (rctx->ok)
        {
            taz_run_as_identity_close(&rctx->identity);
        }
        free(rctx);
        return;
    }

    if (!rctx->ok)
    {
        /* POSIX's taz_run_as_resolve leaves message "" on a passwd miss,
         * so the wire behaviour here stays byte-identical to the old
         * inline lookup's literal; Windows always fills message with the
         * real LsaLogonUser/DuplicateTokenEx failure text. */
        const char *detail_message =
            (rctx->message[0] != '\0') ? rctx->message : "user not found";

        taz_error_send(rctx->write_fn, rctx->write_ctx, rctx->stream_id,
                       rctx->opcode, rctx->code, detail_message, NULL);
        free(rctx);
        return;
    }

    {
        taz_v1_RunAsResponse resp = taz_v1_RunAsResponse_init_zero;

        taz_run_as_reset(rctx->d);
        rctx->d->run_as_active = 1;
        rctx->d->run_as_uid = rctx->identity.uid;
        rctx->d->run_as_gid = rctx->identity.gid;
        (void)snprintf(rctx->d->run_as_user, sizeof(rctx->d->run_as_user), "%s",
                       rctx->identity.user);
        (void)snprintf(rctx->d->run_as_home, sizeof(rctx->d->run_as_home), "%s",
                       rctx->identity.home);
        rctx->d->run_as_token = rctx->identity.token;

        resp.success = true;
        (void)snprintf(resp.effective_user, sizeof(resp.effective_user), "%s",
                       rctx->d->run_as_user);

        taz_response_send(rctx->write_fn, rctx->write_ctx, rctx->stream_id,
                          rctx->opcode, taz_v1_RunAsResponse_fields, &resp);
    }
    free(rctx);
}

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
        run_as_resolve_ctx_t *rctx =
            (run_as_resolve_ctx_t *)calloc(1U, sizeof(*rctx));

        if (rctx == NULL)
        {
            taz_error_send(write_fn, ctx, header->stream_id, header->opcode,
                           taz_v1_ErrorCode_ERROR_CODE_INTERNAL,
                           "out of memory", NULL);
            taz_dispatch_stream_done(d, header->stream_id);
            return;
        }
        rctx->d = d;
        rctx->write_fn = write_fn;
        rctx->write_ctx = ctx;
        rctx->stream_id = header->stream_id;
        rctx->opcode = header->opcode;
        (void)snprintf(rctx->name, sizeof(rctx->name), "%s", req.user);

        /* taz_work_submit releases the stream itself once done returns, so
         * this path - unlike the two synchronous ones above - must not
         * call taz_dispatch_stream_done. */
        if (taz_work_submit(d, header->stream_id, run_as_resolve_work,
                            run_as_resolve_done, rctx) != 0)
        {
            free(rctx);
            taz_error_send(write_fn, ctx, header->stream_id, header->opcode,
                           taz_v1_ErrorCode_ERROR_CODE_INTERNAL,
                           "work submit failed", NULL);
            taz_dispatch_stream_done(d, header->stream_id);
            return;
        }
    }
}
