#include "file_transfer.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pb_decode.h>
#include <pb_encode.h>
#include <uv.h>

#include "taz/crc32c.h"
#include "taz/error.h"
#include "taz/fsutil.h"
#include "taz/response.h"
#include "taz/v1/file.pb.h"
#include "taz/work.h"

/* POSIX permission bits: owner/group/other rwx plus setuid/setgid/sticky. */
#define TAZ_FS_MODE_BITS 07777U

/* FILE_PUT's default mode when the request's permissions field is 0
 * (proto3 cannot distinguish "unset" from 0; DEC-008). */
#define TAZ_FILE_PUT_DEFAULT_MODE 0644U

/* Ingress backpressure: reads are paused once this many bytes are queued
 * but not yet written, resumed once the backlog drains to zero. */
#define TAZ_FILE_PUT_HIGH_WATER ((size_t)256U * 1024U)

/* Size of the "received N bytes, announced M" error message buffer. */
#define TAZ_FILE_PUT_SIZE_MISMATCH_MSG_LEN 96U

typedef enum
{
    PUT_STATE_OPENING,
    PUT_STATE_RECEIVING,
    PUT_STATE_FINISHING,
    PUT_STATE_ABORTING,
    PUT_STATE_DRAINING
} put_state_t;

/* Carries everything a FILE_PUT transfer needs across its lifetime: the
 * write sink/stream/opcode, the request snapshot, the heap temp path, the
 * open fd, running progress (written bytes + CRC), the growable pending
 * buffer FILE_CHUNK payloads are copied into (the reassembly buffer is only
 * valid for the duration of the on_chunk call), and the taz_stream_ops_t
 * instance registered on the stream. Heap-allocated per transfer, freed
 * exactly once by put_release. */
typedef struct
{
    taz_dispatch_t *d;
    taz_dispatch_write_fn_t write_fn;
    void *write_ctx;
    uint32_t stream_id;
    uint16_t opcode;

    taz_v1_FilePutRequest req;
    char *temp;

    uv_file fd;
    int fd_open;
    int temp_created;

    put_state_t state;

    uint64_t written;
    uint32_t crc;

    /* Bytes queued from on_chunk but not yet handed to a write step. */
    uint8_t *pending;
    size_t pending_len;

    /* The buffer a write step is currently writing; owned by the step. */
    uint8_t *writing;
    size_t writing_len;

    int final_seen;
    int work_in_flight;
    /* Set when this transfer decided to abort while a step was in flight
     * (distinct from the connection itself closing, which work.c reports
     * via the done callback's own closing argument). The in-flight step's
     * done callback runs cleanup instead of continuing. */
    int aborted;
    int paused;

    /* Filled in by a work callback; read only by its done callback.
     * error_message overrides the generic "file.put failed" message when
     * set (e.g. a protocol-level wording the spec pins exactly); detail is
     * always a separate, optional elaboration (e.g. a platform error
     * string, or the colliding temp path for BUSY). */
    int ok;
    taz_v1_ErrorCode error_code;
    const char *error_message;
    const char *error_detail;

    taz_stream_ops_t ops;
} put_ctx_t;

static void put_release(put_ctx_t *pctx);
static void put_submit_cleanup(put_ctx_t *pctx);
static void put_trigger_fail(put_ctx_t *pctx, taz_v1_ErrorCode code,
                             const char *message);
static void put_advance(put_ctx_t *pctx);

/* -------------------------------------------------------------------------
 * Teardown
 * ------------------------------------------------------------------------- */

static void put_release(put_ctx_t *pctx)
{
    taz_dispatch_set_stream_ops(pctx->d, pctx->stream_id, NULL, NULL);
    taz_dispatch_stream_done(pctx->d, pctx->stream_id);
    free(pctx->pending);
    free(pctx->writing);
    free(pctx->temp);
    free(pctx);
}

/* Pool thread: closes the fd if still open and unlinks the temp file, but
 * only when this transfer actually created it (never a foreign temp left
 * behind by someone else, e.g. the BUSY case). Best effort: failures here
 * are not reported, there is nothing left to report them to. */
static void put_cleanup_work(void *user)
{
    put_ctx_t *pctx = (put_ctx_t *)user;
    uv_fs_t req;

    if (pctx->fd_open)
    {
        (void)uv_fs_close(NULL, &req, pctx->fd, NULL);
        uv_fs_req_cleanup(&req);
        pctx->fd_open = 0;
    }
    if (pctx->temp_created)
    {
        (void)uv_fs_unlink(NULL, &req, pctx->temp, NULL);
        uv_fs_req_cleanup(&req);
        pctx->temp_created = 0;
    }
}

/* Nothing is ever written from the cleanup path itself. Once the temp is
 * gone, either the final chunk has already arrived (recorded by on_chunk
 * even while ABORTING) or the connection/process is going away (closing) -
 * either way there is nothing left to wait for, so release now. Otherwise
 * the client may still send more of the stream it does not yet know
 * failed: stay registered as DRAINING so those chunks are swallowed
 * instead of landing on a freed ctx, and release only once the final one
 * (or a close) arrives. */
static void put_cleanup_done(void *user, int closing)
{
    put_ctx_t *pctx = (put_ctx_t *)user;
    pctx->work_in_flight = 0;

    if (closing || pctx->final_seen)
    {
        put_release(pctx);
        return;
    }

    pctx->state = PUT_STATE_DRAINING;
}

static void put_submit_cleanup(put_ctx_t *pctx)
{
    if (taz_work_submit_step(pctx->d, put_cleanup_work, put_cleanup_done,
                             pctx) != 0)
    {
        /* Nothing more we can safely do on the pool; release now rather
         * than touch pctx again (best effort: the fd/temp may leak). */
        put_release(pctx);
        return;
    }
    pctx->work_in_flight = 1;
}

/* Sends the error_code/error_message/error_detail a work callback left,
 * falling back to a generic message when it did not pin an exact one. */
static void put_send_pool_error(const put_ctx_t *pctx)
{
    taz_error_send(pctx->write_fn, pctx->write_ctx, pctx->stream_id,
                   pctx->opcode, pctx->error_code,
                   (pctx->error_message != NULL) ? pctx->error_message
                                                 : "file.put failed",
                   pctx->error_detail);
}

/* Sends ERROR (unless the connection is already closing, in which case a
 * write would be pointless) and discards queued bytes, then either runs
 * cleanup now or - if a step is currently in flight on the pool thread, and
 * so owns the fd - defers it to that step's own done callback. */
static void put_trigger_fail(put_ctx_t *pctx, taz_v1_ErrorCode code,
                             const char *message)
{
    if (pctx->state == PUT_STATE_ABORTING || pctx->state == PUT_STATE_DRAINING)
    {
        return;
    }

    free(pctx->pending);
    pctx->pending = NULL;
    pctx->pending_len = 0U;
    pctx->state = PUT_STATE_ABORTING;

    if (!taz_dispatch_conn_closing(pctx->d))
    {
        taz_error_send(pctx->write_fn, pctx->write_ctx, pctx->stream_id,
                       pctx->opcode, code, message, NULL);
    }

    if (pctx->work_in_flight)
    {
        pctx->aborted = 1;
        return;
    }

    put_submit_cleanup(pctx);
}

/* -------------------------------------------------------------------------
 * Chunk-driven progress: swap pending into writing and submit the next
 * write step; once pending has fully drained, resume reads if paused and,
 * once the final chunk has been seen, submit the finish step.
 * ------------------------------------------------------------------------- */

static void put_write_work(void *user);
static void put_write_done(void *user, int closing);
static void put_finish_work(void *user);
static void put_finish_done(void *user, int closing);

static void put_advance(put_ctx_t *pctx)
{
    if (pctx->work_in_flight)
    {
        return;
    }

    if (pctx->pending_len > 0U)
    {
        pctx->writing = pctx->pending;
        pctx->writing_len = pctx->pending_len;
        pctx->pending = NULL;
        pctx->pending_len = 0U;

        if (taz_work_submit_step(pctx->d, put_write_work, put_write_done,
                                 pctx) != 0)
        {
            free(pctx->writing);
            pctx->writing = NULL;
            pctx->writing_len = 0U;
            put_trigger_fail(pctx, taz_v1_ErrorCode_ERROR_CODE_INTERNAL,
                             "work submit failed");
            return;
        }
        pctx->work_in_flight = 1;
        return;
    }

    if (pctx->paused)
    {
        taz_dispatch_conn_resume_reads(pctx->d);
        pctx->paused = 0;
    }

    if (!pctx->final_seen)
    {
        return;
    }

    if (pctx->written != pctx->req.size)
    {
        char msg[TAZ_FILE_PUT_SIZE_MISMATCH_MSG_LEN];
        (void)snprintf(msg, sizeof(msg), "received %llu bytes, announced %llu",
                       (unsigned long long)pctx->written,
                       (unsigned long long)pctx->req.size);
        put_trigger_fail(pctx, taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST,
                         msg);
        return;
    }

    pctx->state = PUT_STATE_FINISHING;
    if (taz_work_submit_step(pctx->d, put_finish_work, put_finish_done, pctx) !=
        0)
    {
        put_trigger_fail(pctx, taz_v1_ErrorCode_ERROR_CODE_INTERNAL,
                         "work submit failed");
        return;
    }
    pctx->work_in_flight = 1;
}

/* -------------------------------------------------------------------------
 * taz_stream_ops_t
 * ------------------------------------------------------------------------- */

static void put_on_chunk(void *user, const taz_frame_header_t *header,
                         const uint8_t *payload)
{
    put_ctx_t *pctx = (put_ctx_t *)user;
    const int is_final =
        (header->flags & (uint8_t)taz_v1_FrameFlag_FRAME_FLAG_CONTINUATION) ==
        0U;
    const uint32_t len = header->length;

    if (pctx->state == PUT_STATE_DRAINING)
    {
        if (is_final)
        {
            put_release(pctx);
        }
        return;
    }

    if (pctx->state != PUT_STATE_OPENING && pctx->state != PUT_STATE_RECEIVING)
    {
        /* ABORTING (cleanup still in flight) or FINISHING: the payload is
         * dropped either way, but the final flag must still be recorded -
         * put_cleanup_done needs it to know whether DRAINING has anything
         * left to wait for. */
        if (is_final)
        {
            pctx->final_seen = 1;
        }
        return;
    }

    if (pctx->written + (uint64_t)pctx->pending_len + (uint64_t)len >
        pctx->req.size)
    {
        put_trigger_fail(pctx, taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST,
                         "more bytes than announced size");
        return;
    }

    if (len > 0U)
    {
        uint8_t *grown =
            (uint8_t *)realloc(pctx->pending, pctx->pending_len + (size_t)len);
        if (grown == NULL)
        {
            put_trigger_fail(pctx, taz_v1_ErrorCode_ERROR_CODE_INTERNAL,
                             "out of memory");
            return;
        }
        (void)memcpy(grown + pctx->pending_len, payload, (size_t)len);
        pctx->pending = grown;
        pctx->pending_len += (size_t)len;
    }

    if (!pctx->paused && pctx->pending_len >= TAZ_FILE_PUT_HIGH_WATER)
    {
        taz_dispatch_conn_pause_reads(pctx->d);
        pctx->paused = 1;
    }

    if (is_final)
    {
        pctx->final_seen = 1;
    }

    if (pctx->state == PUT_STATE_RECEIVING)
    {
        put_advance(pctx);
    }
}

static int put_cancel(void *user, taz_stream_done_fn_t done, void *done_arg)
{
    /* Accepting a cancel mid-transfer is handlers/cancel.c's job; until it
     * lands every CANCEL against a FILE_PUT stream is refused. */
    (void)user;
    (void)done;
    (void)done_arg;
    return 0;
}

static void put_abort(void *user)
{
    put_ctx_t *pctx = (put_ctx_t *)user;

    if (pctx->state == PUT_STATE_DRAINING)
    {
        /* Nothing is in flight in DRAINING (the temp is already gone, the
         * fd already closed) - free synchronously instead of submitting a
         * pointless cleanup step. */
        put_release(pctx);
        return;
    }
    if (pctx->state == PUT_STATE_ABORTING)
    {
        return;
    }
    pctx->state = PUT_STATE_ABORTING;
    free(pctx->pending);
    pctx->pending = NULL;
    pctx->pending_len = 0U;

    if (pctx->work_in_flight)
    {
        pctx->aborted = 1;
        return;
    }

    put_submit_cleanup(pctx);
}

/* -------------------------------------------------------------------------
 * Open phase
 * ------------------------------------------------------------------------- */

static void put_open_work(void *user)
{
    put_ctx_t *pctx = (put_ctx_t *)user;
    char *dirpath;
    uv_fs_t req;
    uint32_t mode;
    int flags;

    dirpath = taz_fsutil_dirname(pctx->req.dest);
    if (dirpath == NULL)
    {
        pctx->error_code = taz_v1_ErrorCode_ERROR_CODE_INTERNAL;
        pctx->error_message = "out of memory";
        return;
    }

    if (uv_fs_lstat(NULL, &req, dirpath, NULL) < 0)
    {
        uv_fs_req_cleanup(&req);
        free(dirpath);
        pctx->error_code = taz_v1_ErrorCode_ERROR_CODE_NOT_FOUND;
        pctx->error_message = "parent directory not found";
        return;
    }
    if (taz_fsutil_kind_from_mode(req.statbuf.st_mode) != taz_v1_Kind_KIND_DIR)
    {
        uv_fs_req_cleanup(&req);
        free(dirpath);
        pctx->error_code = taz_v1_ErrorCode_ERROR_CODE_NOT_FOUND;
        pctx->error_message = "parent is not a directory";
        return;
    }
    uv_fs_req_cleanup(&req);
    free(dirpath);

    if (uv_fs_lstat(NULL, &req, pctx->req.dest, NULL) == 0)
    {
        const int is_dir = taz_fsutil_kind_from_mode(req.statbuf.st_mode) ==
                           taz_v1_Kind_KIND_DIR;
        uv_fs_req_cleanup(&req);

        if (is_dir)
        {
            pctx->error_code = taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST;
            pctx->error_message = "destination is a directory";
            return;
        }
        if (!pctx->req.overwrite)
        {
            pctx->error_code = taz_v1_ErrorCode_ERROR_CODE_ALREADY_EXISTS;
            pctx->error_message = "destination exists";
            return;
        }
    }
    else
    {
        uv_fs_req_cleanup(&req);
    }

    pctx->temp = taz_fsutil_temp_name(pctx->req.dest, pctx->stream_id);
    if (pctx->temp == NULL)
    {
        pctx->error_code = taz_v1_ErrorCode_ERROR_CODE_INTERNAL;
        pctx->error_message = "out of memory";
        return;
    }

    mode = (pctx->req.permissions != 0U)
               ? (pctx->req.permissions & TAZ_FS_MODE_BITS)
               : TAZ_FILE_PUT_DEFAULT_MODE;
    flags = UV_FS_O_WRONLY | UV_FS_O_CREAT | UV_FS_O_EXCL;

    {
        uv_fs_t open_req;
        const uv_file fd =
            uv_fs_open(NULL, &open_req, pctx->temp, flags, (int)mode, NULL);
        if (fd < 0)
        {
            if (open_req.result == UV_EEXIST)
            {
                pctx->error_code = taz_v1_ErrorCode_ERROR_CODE_BUSY;
                pctx->error_detail = pctx->temp;
            }
            else
            {
                pctx->error_code = taz_error_from_fs_req(&open_req);
                pctx->error_detail = taz_error_fs_detail(&open_req);
            }
            uv_fs_req_cleanup(&open_req);
            return;
        }
        uv_fs_req_cleanup(&open_req);
        pctx->fd = fd;
        pctx->fd_open = 1;
        pctx->temp_created = 1;
    }

    pctx->ok = 1;
}

static void put_open_done(void *user, int closing)
{
    put_ctx_t *pctx = (put_ctx_t *)user;
    const int ok = pctx->ok;

    pctx->work_in_flight = 0;
    pctx->ok = 0;

    if (closing || pctx->aborted)
    {
        if (ok)
        {
            pctx->state = PUT_STATE_ABORTING;
            put_submit_cleanup(pctx);
        }
        else
        {
            put_release(pctx);
        }
        return;
    }

    if (!ok)
    {
        put_send_pool_error(pctx);
        put_release(pctx);
        return;
    }

    {
        taz_v1_FilePutResponse resp = taz_v1_FilePutResponse_init_zero;
        resp.which_phase = taz_v1_FilePutResponse_ack_tag;
        resp.phase.ack.ready = true;
        taz_response_send(pctx->write_fn, pctx->write_ctx, pctx->stream_id,
                          pctx->opcode, taz_v1_FilePutResponse_fields, &resp);
    }

    pctx->state = PUT_STATE_RECEIVING;
    put_advance(pctx);
}

/* -------------------------------------------------------------------------
 * Chunk phase: write everything queued, looping over short writes, at the
 * running offset, updating the CRC per byte actually persisted.
 * ------------------------------------------------------------------------- */

static void put_write_work(void *user)
{
    put_ctx_t *pctx = (put_ctx_t *)user;
    size_t off = 0U;

    while (off < pctx->writing_len)
    {
        uv_buf_t buf = uv_buf_init((char *)pctx->writing + off,
                                   (unsigned int)(pctx->writing_len - off));
        uv_fs_t req;
        const int n = uv_fs_write(NULL, &req, pctx->fd, &buf, 1,
                                  (int64_t)pctx->written, NULL);
        if (n <= 0)
        {
            pctx->error_code = taz_error_from_fs_req(&req);
            pctx->error_detail = taz_error_fs_detail(&req);
            uv_fs_req_cleanup(&req);
            return;
        }
        uv_fs_req_cleanup(&req);

        pctx->crc =
            taz_crc32c_update(pctx->crc, pctx->writing + off, (size_t)n);
        pctx->written += (uint64_t)n;
        off += (size_t)n;
    }

    pctx->ok = 1;
}

static void put_write_done(void *user, int closing)
{
    put_ctx_t *pctx = (put_ctx_t *)user;
    const int ok = pctx->ok;

    pctx->work_in_flight = 0;
    pctx->ok = 0;
    free(pctx->writing);
    pctx->writing = NULL;
    pctx->writing_len = 0U;

    if (closing || pctx->aborted)
    {
        pctx->state = PUT_STATE_ABORTING;
        put_submit_cleanup(pctx);
        return;
    }

    if (!ok)
    {
        put_send_pool_error(pctx);
        pctx->state = PUT_STATE_ABORTING;
        put_submit_cleanup(pctx);
        return;
    }

    put_advance(pctx);
}

/* -------------------------------------------------------------------------
 * Finish phase: fsync, close, atomically rename the temp into place.
 * ------------------------------------------------------------------------- */

static void put_finish_work(void *user)
{
    put_ctx_t *pctx = (put_ctx_t *)user;
    uv_fs_t req;

    if (uv_fs_fsync(NULL, &req, pctx->fd, NULL) < 0)
    {
        pctx->error_code = taz_error_from_fs_req(&req);
        pctx->error_detail = taz_error_fs_detail(&req);
        uv_fs_req_cleanup(&req);
        return;
    }
    uv_fs_req_cleanup(&req);

    if (uv_fs_close(NULL, &req, pctx->fd, NULL) < 0)
    {
        pctx->error_code = taz_error_from_fs_req(&req);
        pctx->error_detail = taz_error_fs_detail(&req);
        uv_fs_req_cleanup(&req);
        pctx->fd_open = 0;
        return;
    }
    uv_fs_req_cleanup(&req);
    pctx->fd_open = 0;

    if (uv_fs_rename(NULL, &req, pctx->temp, pctx->req.dest, NULL) < 0)
    {
        pctx->error_code = taz_error_from_fs_req(&req);
        pctx->error_detail = taz_error_fs_detail(&req);
        uv_fs_req_cleanup(&req);
        return;
    }
    uv_fs_req_cleanup(&req);
    pctx->temp_created = 0; /* renamed away; nothing left at temp to unlink */

    pctx->ok = 1;
}

static void put_finish_done(void *user, int closing)
{
    put_ctx_t *pctx = (put_ctx_t *)user;
    const int ok = pctx->ok;

    pctx->work_in_flight = 0;
    pctx->ok = 0;

    if (closing || pctx->aborted)
    {
        if (ok)
        {
            /* The rename already happened; nothing to undo. */
            put_release(pctx);
        }
        else
        {
            pctx->state = PUT_STATE_ABORTING;
            put_submit_cleanup(pctx);
        }
        return;
    }

    if (!ok)
    {
        put_send_pool_error(pctx);
        pctx->state = PUT_STATE_ABORTING;
        put_submit_cleanup(pctx);
        return;
    }

    {
        taz_v1_FilePutResponse resp = taz_v1_FilePutResponse_init_zero;
        resp.which_phase = taz_v1_FilePutResponse_confirm_tag;
        resp.phase.confirm.bytes_written = pctx->written;
        resp.phase.confirm.checksum.size = 4U;
        taz_crc32c_to_le(pctx->crc, resp.phase.confirm.checksum.bytes);
        taz_response_send(pctx->write_fn, pctx->write_ctx, pctx->stream_id,
                          pctx->opcode, taz_v1_FilePutResponse_fields, &resp);
    }

    put_release(pctx);
}

/* -------------------------------------------------------------------------
 * Entry point
 * ------------------------------------------------------------------------- */

void handle_file_put(taz_dispatch_t *d, const taz_frame_header_t *header,
                     const uint8_t *payload, taz_dispatch_write_fn_t write_fn,
                     void *ctx)
{
    taz_v1_FilePutRequest req = taz_v1_FilePutRequest_init_zero;
    put_ctx_t *pctx;

    /* An empty payload is a valid encoding of a request with an empty
     * dest; that is caught by the dest check below, not here. */
    if (payload != NULL && header->length > 0U)
    {
        pb_istream_t istream =
            pb_istream_from_buffer(payload, (size_t)header->length);
        if (!pb_decode(&istream, taz_v1_FilePutRequest_fields, &req))
        {
            taz_error_send(write_fn, ctx, header->stream_id, header->opcode,
                           taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST,
                           "decode FilePutRequest failed", NULL);
            taz_dispatch_stream_done(d, header->stream_id);
            return;
        }
    }

    if (req.dest[0] == '\0')
    {
        taz_error_send(write_fn, ctx, header->stream_id, header->opcode,
                       taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST,
                       "dest is required", NULL);
        taz_dispatch_stream_done(d, header->stream_id);
        return;
    }

    pctx = (put_ctx_t *)calloc(1U, sizeof(*pctx));
    if (pctx == NULL)
    {
        taz_error_send(write_fn, ctx, header->stream_id, header->opcode,
                       taz_v1_ErrorCode_ERROR_CODE_INTERNAL, "out of memory",
                       NULL);
        taz_dispatch_stream_done(d, header->stream_id);
        return;
    }

    pctx->d = d;
    pctx->write_fn = write_fn;
    pctx->write_ctx = ctx;
    pctx->stream_id = header->stream_id;
    pctx->opcode = header->opcode;
    pctx->req = req;
    pctx->state = PUT_STATE_OPENING;

    pctx->ops.on_chunk = put_on_chunk;
    pctx->ops.cancel = put_cancel;
    pctx->ops.abort = put_abort;
    pctx->ops.on_writable = NULL;

    taz_dispatch_set_stream_ops(d, header->stream_id, &pctx->ops, pctx);

    if (taz_work_submit_step(d, put_open_work, put_open_done, pctx) != 0)
    {
        taz_dispatch_set_stream_ops(d, header->stream_id, NULL, NULL);
        free(pctx);
        taz_error_send(write_fn, ctx, header->stream_id, header->opcode,
                       taz_v1_ErrorCode_ERROR_CODE_INTERNAL,
                       "work submit failed", NULL);
        taz_dispatch_stream_done(d, header->stream_id);
        return;
    }
    pctx->work_in_flight = 1;
}
