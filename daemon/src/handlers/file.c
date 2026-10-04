#include "file.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pb_decode.h>
#include <uv.h>

#include "taz/error.h"
#include "taz/fsutil.h"
#include "taz/response.h"
#include "taz/v1/file.pb.h"
#include "taz/work.h"

/* POSIX permission bits: owner/group/other rwx plus setuid/setgid/sticky. */
#define TAZ_FS_MODE_BITS 07777U

#ifndef _WIN32
#define TAZ_PASSWD_PATH "/etc/passwd"
#endif

/* Carries everything file_stat_work/file_stat_done need: the request
 * snapshot for the pool thread, and the write sink/stream/opcode plus the
 * result for the loop thread. Heap-allocated per in-flight stat, freed once
 * file_stat_done runs. */
typedef struct
{
    taz_dispatch_write_fn_t write_fn;
    void *write_ctx;
    uint32_t stream_id;
    uint16_t opcode;
    taz_v1_FileStatRequest req;

    /* Filled in by file_stat_work; read only by file_stat_done. */
    int ok;
    taz_v1_ErrorCode error_code;
    const char *detail;
    taz_v1_FileStatResponse resp;
} file_stat_ctx_t;

/* Pool thread: touches only fctx->req (input) and fctx->{ok,error_code,
 * detail,resp} (output) - never d, a connection, or a uv_* handle outside
 * its own synchronous uv_fs_* requests. */
static void file_stat_work(void *user)
{
    file_stat_ctx_t *fctx = (file_stat_ctx_t *)user;
    uv_fs_t req;
    uint64_t st_mode;
    uint64_t st_uid;

    if (uv_fs_lstat(NULL, &req, fctx->req.path, NULL) < 0)
    {
        fctx->ok = 0;
        fctx->error_code = taz_error_from_fs_req(&req);
        fctx->detail = taz_error_fs_detail(&req);
        uv_fs_req_cleanup(&req);
        return;
    }

    st_mode = req.statbuf.st_mode;
    st_uid = req.statbuf.st_uid;
    fctx->resp.size = req.statbuf.st_size;
    fctx->resp.permissions = (uint32_t)(st_mode & TAZ_FS_MODE_BITS);
    fctx->resp.modified = (uint64_t)req.statbuf.st_mtim.tv_sec;
    fctx->resp.created = (uint64_t)req.statbuf.st_birthtim.tv_sec;
    fctx->resp.kind = taz_fsutil_kind_from_mode(st_mode);
    uv_fs_req_cleanup(&req);

    if (fctx->resp.kind == taz_v1_Kind_KIND_SYMLINK)
    {
        uv_fs_t link_req;
        if (uv_fs_readlink(NULL, &link_req, fctx->req.path, NULL) == 0)
        {
            (void)strncpy(fctx->resp.link_target, (const char *)link_req.ptr,
                          sizeof(fctx->resp.link_target) - 1U);
        }
        uv_fs_req_cleanup(&link_req);
    }

#ifndef _WIN32
    /* Owner lookup never fails the stat: fall back to the decimal uid. */
    if (!taz_passwd_name_from_uid(TAZ_PASSWD_PATH, (unsigned long)st_uid,
                                  fctx->resp.owner, sizeof(fctx->resp.owner)))
    {
        (void)snprintf(fctx->resp.owner, sizeof(fctx->resp.owner), "%lu",
                       (unsigned long)st_uid);
    }
#endif

    fctx->ok = 1;
}

/* Loop thread: closing means the connection began closing while the stat
 * was in flight, so nothing may be written. Either way frees fctx exactly
 * once. */
static void file_stat_done(void *user, int closing)
{
    file_stat_ctx_t *fctx = (file_stat_ctx_t *)user;

    if (!closing)
    {
        if (fctx->ok)
        {
            taz_response_send(fctx->write_fn, fctx->write_ctx, fctx->stream_id,
                              fctx->opcode, taz_v1_FileStatResponse_fields,
                              &fctx->resp);
        }
        else
        {
            taz_error_send(fctx->write_fn, fctx->write_ctx, fctx->stream_id,
                           fctx->opcode, fctx->error_code, "stat failed",
                           fctx->detail);
        }
    }
    free(fctx);
}

void handle_file_stat(taz_dispatch_t *d, const taz_frame_header_t *header,
                      const uint8_t *payload, taz_dispatch_write_fn_t write_fn,
                      void *ctx)
{
    taz_v1_FileStatRequest req = taz_v1_FileStatRequest_init_zero;
    file_stat_ctx_t *fctx;

    /* An empty payload is a valid encoding of a request with an empty
     * path; that is caught by the path check below, not here. */
    if (payload != NULL && header->length > 0U)
    {
        pb_istream_t istream =
            pb_istream_from_buffer(payload, (size_t)header->length);
        if (!pb_decode(&istream, taz_v1_FileStatRequest_fields, &req))
        {
            taz_error_send(write_fn, ctx, header->stream_id, header->opcode,
                           taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST,
                           "decode FileStatRequest failed", NULL);
            taz_dispatch_stream_done(d, header->stream_id);
            return;
        }
    }

    if (req.path[0] == '\0')
    {
        taz_error_send(write_fn, ctx, header->stream_id, header->opcode,
                       taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST,
                       "path is required", NULL);
        taz_dispatch_stream_done(d, header->stream_id);
        return;
    }

    fctx = (file_stat_ctx_t *)calloc(1U, sizeof(*fctx));
    if (fctx == NULL)
    {
        taz_error_send(write_fn, ctx, header->stream_id, header->opcode,
                       taz_v1_ErrorCode_ERROR_CODE_INTERNAL, "out of memory",
                       NULL);
        taz_dispatch_stream_done(d, header->stream_id);
        return;
    }
    fctx->write_fn = write_fn;
    fctx->write_ctx = ctx;
    fctx->stream_id = header->stream_id;
    fctx->opcode = header->opcode;
    fctx->req = req;

    if (taz_work_submit(d, header->stream_id, file_stat_work, file_stat_done,
                        fctx) != 0)
    {
        free(fctx);
        taz_error_send(write_fn, ctx, header->stream_id, header->opcode,
                       taz_v1_ErrorCode_ERROR_CODE_INTERNAL,
                       "work submit failed", NULL);
        taz_dispatch_stream_done(d, header->stream_id);
        return;
    }
}
