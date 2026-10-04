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

/* FILE_CREATE's default mode when the request's permissions field is 0
 * (proto3 cannot distinguish "unset" from 0; DEC-008). */
#define TAZ_FILE_DEFAULT_MODE 0644U

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
#ifndef _WIN32
    uint64_t st_uid;
#endif

    if (uv_fs_lstat(NULL, &req, fctx->req.path, NULL) < 0)
    {
        fctx->ok = 0;
        fctx->error_code = taz_error_from_fs_req(&req);
        fctx->detail = taz_error_fs_detail(&req);
        uv_fs_req_cleanup(&req);
        return;
    }

    st_mode = req.statbuf.st_mode;
#ifndef _WIN32
    st_uid = req.statbuf.st_uid;
#endif
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

/* Collects FileCreateRequest.content during pb_decode: the field is
 * FT_CALLBACK, so nanopb hands the decode callback a substream limited to
 * the field's own bytes. data/len are heap-owned by whoever's holding this
 * struct afterwards - NULL/0 for an absent or empty content field. */
typedef struct
{
    uint8_t *data;
    size_t len;
} file_content_t;

static bool decode_file_content(pb_istream_t *stream, const pb_field_t *field,
                                void **arg)
{
    file_content_t *content = (file_content_t *)*arg;
    const size_t len = stream->bytes_left;
    uint8_t *buf = NULL;

    (void)field;

    if (len > 0U)
    {
        buf = (uint8_t *)malloc(len);
        if (buf == NULL)
        {
            return false;
        }
        if (!pb_read(stream, buf, len))
        {
            free(buf);
            return false;
        }
    }
    /* Proto3 "last wins": a repeated occurrence of this field replaces, not
     * appends to, the previous decode. Free it before overwriting. */
    free(content->data);
    content->data = buf;
    content->len = len;
    return true;
}

/* Carries everything file_create_work/file_create_done need. content/
 * content_len own the bytes decode_file_content copied out of the request
 * payload; req.content itself is never read again after pb_decode returns. */
typedef struct
{
    taz_dispatch_write_fn_t write_fn;
    void *write_ctx;
    uint32_t stream_id;
    uint16_t opcode;
    taz_v1_FileCreateRequest req;
    uint8_t *content;
    size_t content_len;

    int ok;
    taz_v1_ErrorCode error_code;
    const char *detail;
    taz_v1_FileCreateResponse resp;
} file_create_ctx_t;

/* Pool thread: exclusive-create, write all of content (looping over short
 * writes), close either way. A write failure closes the file and leaves
 * whatever was written so far on disk. */
static void file_create_work(void *user)
{
    file_create_ctx_t *fctx = (file_create_ctx_t *)user;
    uv_fs_t req;
    uv_file fd;
    uv_fs_t close_req;
    const uint32_t mode = (fctx->req.permissions != 0U)
                              ? (fctx->req.permissions & TAZ_FS_MODE_BITS)
                              : TAZ_FILE_DEFAULT_MODE;
    size_t written = 0U;

    fd = uv_fs_open(NULL, &req, fctx->req.path,
                    UV_FS_O_WRONLY | UV_FS_O_CREAT | UV_FS_O_EXCL, (int)mode,
                    NULL);
    if (fd < 0)
    {
        fctx->ok = 0;
        fctx->error_code = taz_error_from_fs_req(&req);
        fctx->detail = taz_error_fs_detail(&req);
        uv_fs_req_cleanup(&req);
        return;
    }
    uv_fs_req_cleanup(&req);

    while (written < fctx->content_len)
    {
        uv_buf_t buf = uv_buf_init((char *)fctx->content + written,
                                   (unsigned int)(fctx->content_len - written));
        uv_fs_t write_req;
        const int n =
            uv_fs_write(NULL, &write_req, fd, &buf, 1, (int64_t)written, NULL);
        if (n <= 0)
        {
            fctx->ok = 0;
            fctx->error_code = taz_error_from_fs_req(&write_req);
            fctx->detail = taz_error_fs_detail(&write_req);
            uv_fs_req_cleanup(&write_req);
            (void)uv_fs_close(NULL, &close_req, fd, NULL);
            uv_fs_req_cleanup(&close_req);
            return;
        }
        uv_fs_req_cleanup(&write_req);
        written += (size_t)n;
    }

    (void)uv_fs_close(NULL, &close_req, fd, NULL);
    uv_fs_req_cleanup(&close_req);

    fctx->resp.success = true;
    fctx->ok = 1;
}

static void file_create_done(void *user, int closing)
{
    file_create_ctx_t *fctx = (file_create_ctx_t *)user;

    if (!closing)
    {
        if (fctx->ok)
        {
            taz_response_send(fctx->write_fn, fctx->write_ctx, fctx->stream_id,
                              fctx->opcode, taz_v1_FileCreateResponse_fields,
                              &fctx->resp);
        }
        else
        {
            taz_error_send(fctx->write_fn, fctx->write_ctx, fctx->stream_id,
                           fctx->opcode, fctx->error_code, "create failed",
                           fctx->detail);
        }
    }
    free(fctx->content);
    free(fctx);
}

void handle_file_create(taz_dispatch_t *d, const taz_frame_header_t *header,
                        const uint8_t *payload,
                        taz_dispatch_write_fn_t write_fn, void *ctx)
{
    taz_v1_FileCreateRequest req = taz_v1_FileCreateRequest_init_zero;
    file_content_t content = {NULL, 0U};
    file_create_ctx_t *fctx;

    req.content.funcs.decode = decode_file_content;
    req.content.arg = &content;

    /* An empty payload is a valid encoding of a request with an empty
     * path; that is caught by the path check below, not here. */
    if (payload != NULL && header->length > 0U)
    {
        pb_istream_t istream =
            pb_istream_from_buffer(payload, (size_t)header->length);
        if (!pb_decode(&istream, taz_v1_FileCreateRequest_fields, &req))
        {
            free(content.data);
            taz_error_send(write_fn, ctx, header->stream_id, header->opcode,
                           taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST,
                           "decode FileCreateRequest failed", NULL);
            taz_dispatch_stream_done(d, header->stream_id);
            return;
        }
    }

    if (req.path[0] == '\0')
    {
        free(content.data);
        taz_error_send(write_fn, ctx, header->stream_id, header->opcode,
                       taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST,
                       "path is required", NULL);
        taz_dispatch_stream_done(d, header->stream_id);
        return;
    }

    fctx = (file_create_ctx_t *)calloc(1U, sizeof(*fctx));
    if (fctx == NULL)
    {
        free(content.data);
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
    fctx->content = content.data;
    fctx->content_len = content.len;

    if (taz_work_submit(d, header->stream_id, file_create_work,
                        file_create_done, fctx) != 0)
    {
        free(fctx->content);
        free(fctx);
        taz_error_send(write_fn, ctx, header->stream_id, header->opcode,
                       taz_v1_ErrorCode_ERROR_CODE_INTERNAL,
                       "work submit failed", NULL);
        taz_dispatch_stream_done(d, header->stream_id);
        return;
    }
}

/* Carries everything file_delete_work/file_delete_done need. */
typedef struct
{
    taz_dispatch_write_fn_t write_fn;
    void *write_ctx;
    uint32_t stream_id;
    uint16_t opcode;
    taz_v1_FileDeleteRequest req;

    int ok;
    taz_v1_ErrorCode error_code;
    const char *detail;
    taz_v1_FileDeleteResponse resp;
} file_delete_ctx_t;

/* Pool thread: unlink only - never follows or removes a directory's
 * contents; a symlink is removed itself, never its target. */
static void file_delete_work(void *user)
{
    file_delete_ctx_t *fctx = (file_delete_ctx_t *)user;
    uv_fs_t req;

    if (uv_fs_unlink(NULL, &req, fctx->req.path, NULL) < 0)
    {
        fctx->ok = 0;
        fctx->error_code = taz_error_from_fs_req(&req);
        fctx->detail = taz_error_fs_detail(&req);
        uv_fs_req_cleanup(&req);
        return;
    }
    uv_fs_req_cleanup(&req);
    fctx->resp.success = true;
    fctx->ok = 1;
}

static void file_delete_done(void *user, int closing)
{
    file_delete_ctx_t *fctx = (file_delete_ctx_t *)user;

    if (!closing)
    {
        if (fctx->ok)
        {
            taz_response_send(fctx->write_fn, fctx->write_ctx, fctx->stream_id,
                              fctx->opcode, taz_v1_FileDeleteResponse_fields,
                              &fctx->resp);
        }
        else
        {
            taz_error_send(fctx->write_fn, fctx->write_ctx, fctx->stream_id,
                           fctx->opcode, fctx->error_code, "delete failed",
                           fctx->detail);
        }
    }
    free(fctx);
}

void handle_file_delete(taz_dispatch_t *d, const taz_frame_header_t *header,
                        const uint8_t *payload,
                        taz_dispatch_write_fn_t write_fn, void *ctx)
{
    taz_v1_FileDeleteRequest req = taz_v1_FileDeleteRequest_init_zero;
    file_delete_ctx_t *fctx;

    if (payload != NULL && header->length > 0U)
    {
        pb_istream_t istream =
            pb_istream_from_buffer(payload, (size_t)header->length);
        if (!pb_decode(&istream, taz_v1_FileDeleteRequest_fields, &req))
        {
            taz_error_send(write_fn, ctx, header->stream_id, header->opcode,
                           taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST,
                           "decode FileDeleteRequest failed", NULL);
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

    fctx = (file_delete_ctx_t *)calloc(1U, sizeof(*fctx));
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

    if (taz_work_submit(d, header->stream_id, file_delete_work,
                        file_delete_done, fctx) != 0)
    {
        free(fctx);
        taz_error_send(write_fn, ctx, header->stream_id, header->opcode,
                       taz_v1_ErrorCode_ERROR_CODE_INTERNAL,
                       "work submit failed", NULL);
        taz_dispatch_stream_done(d, header->stream_id);
        return;
    }
}
