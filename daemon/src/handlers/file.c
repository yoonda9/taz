#include "file.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pb_decode.h>
#include <pb_encode.h>
#include <uv.h>

#include "taz/error.h"
#include "taz/fsutil.h"
#include "taz/response.h"
#include "taz/v1/file.pb.h"
#include "taz/work.h"

#ifdef _WIN32
#include <aclapi.h>
#include <windows.h>
#endif

/* POSIX permission bits: owner/group/other rwx plus setuid/setgid/sticky. */
#define TAZ_FS_MODE_BITS 07777U

/* FILE_CREATE's default mode when the request's permissions field is 0
 * (proto3 cannot distinguish "unset" from 0; DEC-008). */
#define TAZ_FILE_DEFAULT_MODE 0644U

/* DIR_MAKE's default mode when the request's permissions field is 0. */
#define TAZ_DIR_DEFAULT_MODE 0755U

/* DIR_LIST's per-batch entry cap, matching taz_v1_DirListResponse's own
 * entries[] array size (DEC-003). */
#define TAZ_DIR_LIST_BATCH_MAX 64U

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

#ifdef _WIN32
/* Best-effort "DOMAIN\name" owner lookup (DEC-007): any failure along the
 * way (conversion, GetNamedSecurityInfoW, LookupAccountSidW) leaves owner
 * untouched (already "" from the caller's calloc) rather than failing the
 * stat. Runs on the pool thread; touches no shared state. */
static void file_stat_owner_win32(const char *path, char *owner,
                                  size_t owner_size)
{
    PSID owner_sid = NULL;
    PSECURITY_DESCRIPTOR sd = NULL;
    WCHAR *wpath;
    const ssize_t wlen = uv_wtf8_length_as_utf16(path);

    if (wlen <= 0)
    {
        return;
    }
    wpath = (WCHAR *)malloc((size_t)wlen * sizeof(WCHAR));
    if (wpath == NULL)
    {
        return;
    }
    uv_wtf8_to_utf16(path, wpath, (size_t)wlen);

    if (GetNamedSecurityInfoW(wpath, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION,
                              &owner_sid, NULL, NULL, NULL,
                              &sd) == ERROR_SUCCESS &&
        owner_sid != NULL)
    {
        WCHAR name[256];
        WCHAR domain[256];
        DWORD name_len = (DWORD)(sizeof(name) / sizeof(name[0]));
        DWORD domain_len = (DWORD)(sizeof(domain) / sizeof(domain[0]));
        SID_NAME_USE use;

        if (LookupAccountSidW(NULL, owner_sid, name, &name_len, domain,
                              &domain_len, &use))
        {
            char name_utf8[256];
            char domain_utf8[256];

            if (WideCharToMultiByte(CP_UTF8, 0, domain, -1, domain_utf8,
                                    (int)sizeof(domain_utf8), NULL, NULL) > 0 &&
                WideCharToMultiByte(CP_UTF8, 0, name, -1, name_utf8,
                                    (int)sizeof(name_utf8), NULL, NULL) > 0)
            {
                (void)snprintf(owner, owner_size, "%s\\%s", domain_utf8,
                               name_utf8);
            }
        }
    }

    if (sd != NULL)
    {
        LocalFree(sd);
    }
    free(wpath);
}
#endif

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
#else
    file_stat_owner_win32(fctx->req.path, fctx->resp.owner,
                          sizeof(fctx->resp.owner));
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

/* Carries everything file_chmod_work/file_chmod_done need. unrepresentable
 * is only meaningful (and only computed) on Windows, where uv_fs_chmod
 * honours just the owner-write bit. */
typedef struct
{
    taz_dispatch_write_fn_t write_fn;
    void *write_ctx;
    uint32_t stream_id;
    uint16_t opcode;
    taz_v1_FileChmodRequest req;

    int ok;
    taz_v1_ErrorCode error_code;
    const char *detail;
    taz_v1_FileChmodResponse resp;
#ifdef _WIN32
    uint32_t unrepresentable;
#endif
} file_chmod_ctx_t;

/* Pool thread: a single uv_fs_chmod, masked to the POSIX permission bits. */
static void file_chmod_work(void *user)
{
    file_chmod_ctx_t *fctx = (file_chmod_ctx_t *)user;
    uv_fs_t req;
    const uint32_t mode = fctx->req.permissions & TAZ_FS_MODE_BITS;

    if (uv_fs_chmod(NULL, &req, fctx->req.path, (int)mode, NULL) < 0)
    {
        fctx->ok = 0;
        fctx->error_code = taz_error_from_fs_req(&req);
        fctx->detail = taz_error_fs_detail(&req);
        uv_fs_req_cleanup(&req);
        return;
    }
    uv_fs_req_cleanup(&req);

#ifdef _WIN32
    fctx->unrepresentable = taz_fsutil_chmod_unrepresentable(mode);
#endif

    fctx->resp.success = true;
    fctx->ok = 1;
}

/* Loop thread: on a successful, non-closing chmod on Windows, warn when the
 * requested mode could not be faithfully represented by the single
 * owner-write bit libuv actually toggles there. */
static void file_chmod_done(void *user, int closing)
{
    file_chmod_ctx_t *fctx = (file_chmod_ctx_t *)user;

    if (!closing)
    {
        if (fctx->ok)
        {
#ifdef _WIN32
            if (fctx->unrepresentable != 0U)
            {
                (void)fprintf(
                    stderr,
                    "tazd: warning: chmod %s: mode %04o is only partially "
                    "honoured on Windows (owner-write bit only)\n",
                    fctx->req.path, (unsigned int)fctx->req.permissions);
            }
#endif
            taz_response_send(fctx->write_fn, fctx->write_ctx, fctx->stream_id,
                              fctx->opcode, taz_v1_FileChmodResponse_fields,
                              &fctx->resp);
        }
        else
        {
            taz_error_send(fctx->write_fn, fctx->write_ctx, fctx->stream_id,
                           fctx->opcode, fctx->error_code, "chmod failed",
                           fctx->detail);
        }
    }
    free(fctx);
}

void handle_file_chmod(taz_dispatch_t *d, const taz_frame_header_t *header,
                       const uint8_t *payload, taz_dispatch_write_fn_t write_fn,
                       void *ctx)
{
    taz_v1_FileChmodRequest req = taz_v1_FileChmodRequest_init_zero;
    file_chmod_ctx_t *fctx;

    if (payload != NULL && header->length > 0U)
    {
        pb_istream_t istream =
            pb_istream_from_buffer(payload, (size_t)header->length);
        if (!pb_decode(&istream, taz_v1_FileChmodRequest_fields, &req))
        {
            taz_error_send(write_fn, ctx, header->stream_id, header->opcode,
                           taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST,
                           "decode FileChmodRequest failed", NULL);
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

    fctx = (file_chmod_ctx_t *)calloc(1U, sizeof(*fctx));
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

    if (taz_work_submit(d, header->stream_id, file_chmod_work, file_chmod_done,
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

/* Carries everything dir_make_work/dir_make_done need. */
typedef struct
{
    taz_dispatch_write_fn_t write_fn;
    void *write_ctx;
    uint32_t stream_id;
    uint16_t opcode;
    taz_v1_DirMakeRequest req;

    int ok;
    taz_v1_ErrorCode error_code;
    const char *detail;
    taz_v1_DirMakeResponse resp;
} dir_make_ctx_t;

/* mkdir -p over a heap copy of fctx->req.path: create every missing prefix,
 * then the final component. A prefix mkdir failure other than EEXIST fails
 * the whole call immediately. On EEXIST, stat (not lstat) the prefix to
 * confirm it is really a directory: CreateDirectoryW/mkdir through a
 * regular-file prefix reports a platform-specific error on the *next* level
 * down (ENOTDIR on POSIX, ERROR_PATH_NOT_FOUND -> NOT_FOUND on Windows), so
 * checking here keeps the error code platform-independent (ALREADY_EXISTS)
 * instead of leaking that difference to the caller. Following symlinks
 * (stat, not lstat) matches mkdir -p: a prefix or final component that is a
 * symlink to a directory counts as already existing, same as coreutils
 * mkdir -p. taz_fsutil_root_prefix_len/taz_fsutil_is_sep keep this portable
 * (POSIX "/", Windows drive/UNC/long-path prefixes); trailing separators are
 * trimmed first so they never produce an empty component. */
static void dir_make_parents(dir_make_ctx_t *fctx, uint32_t mode)
{
    const size_t path_len = strlen(fctx->req.path);
    char *copy = (char *)malloc(path_len + 1U);
    size_t root_len;
    size_t len;
    size_t i;
    uv_fs_t req;

    if (copy == NULL)
    {
        fctx->ok = 0;
        fctx->error_code = taz_v1_ErrorCode_ERROR_CODE_INTERNAL;
        fctx->detail = "out of memory";
        return;
    }
    (void)memcpy(copy, fctx->req.path, path_len + 1U);

    /* Trim trailing separators by scanning forward from root_len and
     * remembering the position just past the last non-separator byte
     * seen - equivalent to trimming from the end, but indexes only ever
     * increase (0 <= i < path_len), which keeps every array access
     * trivially in bounds for a static analyzer. */
    root_len = taz_fsutil_root_prefix_len(copy);
    len = root_len;
    for (i = root_len; i < path_len; i++)
    {
        if (!taz_fsutil_is_sep(copy[i]))
        {
            len = i + 1U;
        }
    }
    copy[len] = '\0';

    for (i = root_len; i < len; i++)
    {
        if (!taz_fsutil_is_sep(copy[i]))
        {
            continue;
        }

        {
            const char saved = copy[i];
            copy[i] = '\0';
            if (uv_fs_mkdir(NULL, &req, copy, (int)mode, NULL) < 0)
            {
                if (req.result != UV_EEXIST)
                {
                    fctx->ok = 0;
                    fctx->error_code = taz_error_from_fs_req(&req);
                    fctx->detail = taz_error_fs_detail(&req);
                    uv_fs_req_cleanup(&req);
                    free(copy);
                    return;
                }

                {
                    uv_fs_t stat_req;
                    int is_dir = 0;
                    if (uv_fs_stat(NULL, &stat_req, copy, NULL) == 0)
                    {
                        is_dir = (taz_fsutil_kind_from_mode(
                                      stat_req.statbuf.st_mode) ==
                                  taz_v1_Kind_KIND_DIR);
                    }
                    uv_fs_req_cleanup(&stat_req);
                    uv_fs_req_cleanup(&req);

                    if (!is_dir)
                    {
                        fctx->ok = 0;
                        fctx->error_code =
                            taz_v1_ErrorCode_ERROR_CODE_ALREADY_EXISTS;
                        fctx->detail = "path component is not a directory";
                        free(copy);
                        return;
                    }
                }
            }
            else
            {
                uv_fs_req_cleanup(&req);
            }
            copy[i] = saved;
        }
    }

    if (uv_fs_mkdir(NULL, &req, copy, (int)mode, NULL) < 0)
    {
        if (req.result == UV_EEXIST)
        {
            uv_fs_t stat_req;
            int is_dir = 0;
            if (uv_fs_stat(NULL, &stat_req, copy, NULL) == 0)
            {
                is_dir = (taz_fsutil_kind_from_mode(stat_req.statbuf.st_mode) ==
                          taz_v1_Kind_KIND_DIR);
            }
            uv_fs_req_cleanup(&stat_req);
            uv_fs_req_cleanup(&req);
            free(copy);

            if (is_dir)
            {
                fctx->resp.success = true;
                fctx->ok = 1;
                return;
            }
            fctx->ok = 0;
            fctx->error_code = taz_v1_ErrorCode_ERROR_CODE_ALREADY_EXISTS;
            fctx->detail = "path exists and is not a directory";
            return;
        }

        fctx->ok = 0;
        fctx->error_code = taz_error_from_fs_req(&req);
        fctx->detail = taz_error_fs_detail(&req);
        uv_fs_req_cleanup(&req);
        free(copy);
        return;
    }

    uv_fs_req_cleanup(&req);
    free(copy);
    fctx->resp.success = true;
    fctx->ok = 1;
}

/* Pool thread: without parents, a single uv_fs_mkdir; with parents, the
 * mkdir -p walk above. */
static void dir_make_work(void *user)
{
    dir_make_ctx_t *fctx = (dir_make_ctx_t *)user;
    const uint32_t mode = (fctx->req.permissions != 0U)
                              ? (fctx->req.permissions & TAZ_FS_MODE_BITS)
                              : TAZ_DIR_DEFAULT_MODE;

    if (fctx->req.parents)
    {
        dir_make_parents(fctx, mode);
        return;
    }

    {
        uv_fs_t req;
        if (uv_fs_mkdir(NULL, &req, fctx->req.path, (int)mode, NULL) < 0)
        {
            fctx->ok = 0;
            fctx->error_code = taz_error_from_fs_req(&req);
            fctx->detail = taz_error_fs_detail(&req);
            uv_fs_req_cleanup(&req);
            return;
        }
        uv_fs_req_cleanup(&req);
    }

    fctx->resp.success = true;
    fctx->ok = 1;
}

static void dir_make_done(void *user, int closing)
{
    dir_make_ctx_t *fctx = (dir_make_ctx_t *)user;

    if (!closing)
    {
        if (fctx->ok)
        {
            taz_response_send(fctx->write_fn, fctx->write_ctx, fctx->stream_id,
                              fctx->opcode, taz_v1_DirMakeResponse_fields,
                              &fctx->resp);
        }
        else
        {
            taz_error_send(fctx->write_fn, fctx->write_ctx, fctx->stream_id,
                           fctx->opcode, fctx->error_code, "mkdir failed",
                           fctx->detail);
        }
    }
    free(fctx);
}

void handle_dir_make(taz_dispatch_t *d, const taz_frame_header_t *header,
                     const uint8_t *payload, taz_dispatch_write_fn_t write_fn,
                     void *ctx)
{
    taz_v1_DirMakeRequest req = taz_v1_DirMakeRequest_init_zero;
    dir_make_ctx_t *fctx;

    if (payload != NULL && header->length > 0U)
    {
        pb_istream_t istream =
            pb_istream_from_buffer(payload, (size_t)header->length);
        if (!pb_decode(&istream, taz_v1_DirMakeRequest_fields, &req))
        {
            taz_error_send(write_fn, ctx, header->stream_id, header->opcode,
                           taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST,
                           "decode DirMakeRequest failed", NULL);
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

    fctx = (dir_make_ctx_t *)calloc(1U, sizeof(*fctx));
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

    if (taz_work_submit(d, header->stream_id, dir_make_work, dir_make_done,
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

/* One scanned directory entry, captured on the pool thread and later
 * re-encoded into DirEntry batches on the loop thread. name mirrors
 * taz_v1_DirEntry.name's size. */
typedef struct
{
    char name[256];
    taz_v1_Kind kind;
    uint64_t size;
} dir_list_entry_t;

/* Carries everything dir_list_work/dir_list_done need. entries/count/
 * capacity form a growable array built on the pool thread; dir_list_done
 * frees it after encoding. */
typedef struct
{
    taz_dispatch_write_fn_t write_fn;
    void *write_ctx;
    uint32_t stream_id;
    uint16_t opcode;
    taz_v1_DirListRequest req;

    int ok;
    taz_v1_ErrorCode error_code;
    const char *detail;

    dir_list_entry_t *entries;
    size_t count;
    size_t capacity;
} dir_list_ctx_t;

/* Appends one entry, growing the array geometrically. Returns 0 on OOM
 * (including an overflowing capacity*sizeof(entry)) without touching
 * fctx's existing array. */
static int dir_list_append(dir_list_ctx_t *fctx, const char *name,
                           taz_v1_Kind kind, uint64_t size)
{
    if (fctx->count == fctx->capacity)
    {
        const size_t new_capacity = (fctx->capacity == 0U)
                                        ? TAZ_DIR_LIST_BATCH_MAX
                                        : fctx->capacity * 2U;
        dir_list_entry_t *grown;

        if (new_capacity > (SIZE_MAX / sizeof(*grown)))
        {
            return 0;
        }
        grown = (dir_list_entry_t *)realloc(fctx->entries,
                                            new_capacity * sizeof(*grown));
        if (grown == NULL)
        {
            return 0;
        }
        fctx->entries = grown;
        fctx->capacity = new_capacity;
    }

    {
        dir_list_entry_t *e = &fctx->entries[fctx->count];
        taz_fsutil_truncate_utf8(name, e->name, sizeof(e->name));
        e->kind = kind;
        e->size = size;
    }
    fctx->count++;
    return 1;
}

/* Pool thread: stat (following symlinks) the top-level path first - never
 * scandir straight away - so a path that is a regular file reports a
 * stable error other than NOT_FOUND instead of whatever scandir's own
 * ENOTDIR maps to on this platform (POSIX ENOTDIR maps to NOT_FOUND via
 * taz_error_from_errno, which would otherwise violate the "path is a file
 * -> error != NOT_FOUND" row).
 * Following symlinks here (not lstat) means a symlink to a directory lists
 * the target's entries like opendir/ls would, instead of failing with
 * "path is not a directory"; a dangling symlink still fails this stat with
 * ENOENT -> NOT_FOUND, same as a missing path. Then scandir, filtering
 * hidden names and lstat-ing each survivor (entries themselves are never
 * followed). Only an ENOENT from that lstat (the entry vanished between
 * scandir and lstat) is skipped rather than failing the whole listing; any
 * other lstat failure (e.g. EACCES from a directory that can be read but
 * not searched) fails the listing instead of silently dropping the
 * entry. */
static void dir_list_work(void *user)
{
    dir_list_ctx_t *fctx = (dir_list_ctx_t *)user;
    uv_fs_t stat_req;
    uv_fs_t scan_req;
    uv_dirent_t ent;

    if (uv_fs_stat(NULL, &stat_req, fctx->req.path, NULL) < 0)
    {
        fctx->ok = 0;
        fctx->error_code = taz_error_from_fs_req(&stat_req);
        fctx->detail = taz_error_fs_detail(&stat_req);
        uv_fs_req_cleanup(&stat_req);
        return;
    }
    if (taz_fsutil_kind_from_mode(stat_req.statbuf.st_mode) !=
        taz_v1_Kind_KIND_DIR)
    {
        uv_fs_req_cleanup(&stat_req);
        fctx->ok = 0;
        fctx->error_code = taz_v1_ErrorCode_ERROR_CODE_INTERNAL;
        fctx->detail = "path is not a directory";
        return;
    }
    uv_fs_req_cleanup(&stat_req);

    if (uv_fs_scandir(NULL, &scan_req, fctx->req.path, 0, NULL) < 0)
    {
        fctx->ok = 0;
        fctx->error_code = taz_error_from_fs_req(&scan_req);
        fctx->detail = taz_error_fs_detail(&scan_req);
        uv_fs_req_cleanup(&scan_req);
        return;
    }

    while (uv_fs_scandir_next(&scan_req, &ent) != UV_EOF)
    {
        char *child;
        uv_fs_t lstat_req;

        if (!fctx->req.include_hidden && taz_fsutil_is_hidden(ent.name))
        {
            continue;
        }

        child = taz_fsutil_join(fctx->req.path, ent.name);
        if (child == NULL)
        {
            fctx->ok = 0;
            fctx->error_code = taz_v1_ErrorCode_ERROR_CODE_INTERNAL;
            fctx->detail = "out of memory";
            uv_fs_req_cleanup(&scan_req);
            return;
        }

        {
            const int lstat_rc = uv_fs_lstat(NULL, &lstat_req, child, NULL);
            free(child);
            if (lstat_rc < 0)
            {
                if (lstat_rc == UV_ENOENT)
                {
                    /* Vanished between scandir and lstat: skip, not an
                     * error. */
                    uv_fs_req_cleanup(&lstat_req);
                    continue;
                }
                fctx->ok = 0;
                fctx->error_code = taz_error_from_fs_req(&lstat_req);
                fctx->detail = taz_error_fs_detail(&lstat_req);
                uv_fs_req_cleanup(&lstat_req);
                uv_fs_req_cleanup(&scan_req);
                return;
            }
        }

        {
            const taz_v1_Kind kind =
                taz_fsutil_kind_from_mode(lstat_req.statbuf.st_mode);
            const uint64_t size = (kind == taz_v1_Kind_KIND_FILE)
                                      ? lstat_req.statbuf.st_size
                                      : 0U;
            uv_fs_req_cleanup(&lstat_req);

            if (!dir_list_append(fctx, ent.name, kind, size))
            {
                fctx->ok = 0;
                fctx->error_code = taz_v1_ErrorCode_ERROR_CODE_INTERNAL;
                fctx->detail = "out of memory";
                uv_fs_req_cleanup(&scan_req);
                return;
            }
        }
    }
    uv_fs_req_cleanup(&scan_req);

    fctx->ok = 1;
}

/* Loop thread (pure computation, no I/O): encodes fctx->entries into
 * batches of at most TAZ_DIR_LIST_BATCH_MAX DirEntry each (the struct's
 * own array limit), concatenating each batch's encoded bytes into one
 * growable buffer. The do/while runs its body at least once, so an empty
 * listing still produces one (zero-length) iteration rather than no
 * buffer at all - taz_response_send_encoded turns that into a single
 * RESPONSE frame with an empty payload. taz_v1_DirListResponse is ~17.5 KiB
 * (64 x DirEntry{name[256]}) - too large for a stack frame on a 1 MiB
 * Windows thread (MSVC /analyze C6262 is fatal under /WX; GCC's
 * -Wframe-larger-than=16384 agrees), so it is heap-allocated once here and
 * reused batch to batch; entries_count caps what nanopb actually encodes,
 * so stale bytes left over from the previous batch past that count are
 * never read. Returns 0 on an encoding/allocation failure; out_buf and
 * out_len are valid (and must still be freed) either way. */
static int dir_list_encode(const dir_list_ctx_t *fctx, uint8_t **out_buf,
                           size_t *out_len)
{
    uint8_t *buf = NULL;
    size_t buf_len = 0U;
    size_t i = 0U;
    taz_v1_DirListResponse *batch =
        (taz_v1_DirListResponse *)calloc(1U, sizeof(*batch));

    if (batch == NULL)
    {
        *out_buf = NULL;
        *out_len = 0U;
        return 0;
    }

    do
    {
        size_t batch_count = 0U;
        size_t batch_size = 0U;

        while (i < fctx->count && batch_count < TAZ_DIR_LIST_BATCH_MAX)
        {
            taz_v1_DirEntry *e = &batch->entries[batch_count];
            (void)strncpy(e->name, fctx->entries[i].name, sizeof(e->name) - 1U);
            e->name[sizeof(e->name) - 1U] = '\0';
            e->kind = fctx->entries[i].kind;
            e->size = fctx->entries[i].size;
            batch_count++;
            i++;
        }
        batch->entries_count = (pb_size_t)batch_count;

        if (!pb_get_encoded_size(&batch_size, taz_v1_DirListResponse_fields,
                                 batch))
        {
            free(batch);
            *out_buf = buf;
            *out_len = buf_len;
            return 0;
        }

        if (batch_size > 0U)
        {
            uint8_t *grown = (uint8_t *)realloc(buf, buf_len + batch_size);
            if (grown == NULL)
            {
                free(batch);
                *out_buf = buf;
                *out_len = buf_len;
                return 0;
            }
            buf = grown;

            {
                pb_ostream_t ostream =
                    pb_ostream_from_buffer(buf + buf_len, batch_size);
                if (!pb_encode(&ostream, taz_v1_DirListResponse_fields, batch))
                {
                    free(batch);
                    *out_buf = buf;
                    *out_len = buf_len;
                    return 0;
                }
                buf_len += ostream.bytes_written;
            }
        }
    } while (i < fctx->count);

    free(batch);
    *out_buf = buf;
    *out_len = buf_len;
    return 1;
}

static void dir_list_done(void *user, int closing)
{
    dir_list_ctx_t *fctx = (dir_list_ctx_t *)user;

    if (!closing)
    {
        if (fctx->ok)
        {
            uint8_t *buf = NULL;
            size_t buf_len = 0U;

            if (dir_list_encode(fctx, &buf, &buf_len))
            {
                taz_response_send_encoded(fctx->write_fn, fctx->write_ctx,
                                          fctx->stream_id, fctx->opcode, buf,
                                          buf_len);
            }
            else
            {
                taz_error_send(fctx->write_fn, fctx->write_ctx, fctx->stream_id,
                               fctx->opcode,
                               taz_v1_ErrorCode_ERROR_CODE_INTERNAL,
                               "out of memory", NULL);
            }
            free(buf);
        }
        else
        {
            taz_error_send(fctx->write_fn, fctx->write_ctx, fctx->stream_id,
                           fctx->opcode, fctx->error_code, "list failed",
                           fctx->detail);
        }
    }
    free(fctx->entries);
    free(fctx);
}

void handle_dir_list(taz_dispatch_t *d, const taz_frame_header_t *header,
                     const uint8_t *payload, taz_dispatch_write_fn_t write_fn,
                     void *ctx)
{
    taz_v1_DirListRequest req = taz_v1_DirListRequest_init_zero;
    dir_list_ctx_t *fctx;

    if (payload != NULL && header->length > 0U)
    {
        pb_istream_t istream =
            pb_istream_from_buffer(payload, (size_t)header->length);
        if (!pb_decode(&istream, taz_v1_DirListRequest_fields, &req))
        {
            taz_error_send(write_fn, ctx, header->stream_id, header->opcode,
                           taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST,
                           "decode DirListRequest failed", NULL);
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

    fctx = (dir_list_ctx_t *)calloc(1U, sizeof(*fctx));
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

    if (taz_work_submit(d, header->stream_id, dir_list_work, dir_list_done,
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

/* Carries everything dir_remove_work/dir_remove_done need. */
typedef struct
{
    taz_dispatch_write_fn_t write_fn;
    void *write_ctx;
    uint32_t stream_id;
    uint16_t opcode;
    taz_v1_DirRemoveRequest req;

    int ok;
    taz_v1_ErrorCode error_code;
    const char *detail;
    taz_v1_DirRemoveResponse resp;
} dir_remove_ctx_t;

/* One level of dir_remove_tree's walk: an open scandir result for one
 * directory still being drained, plus the heap path it was opened on.
 * scan_opened distinguishes "not yet opened" from "open, needs cleanup"
 * (both on the happy path, once its last entry is consumed, and during
 * error unwind). */
typedef struct
{
    char *path;
    uv_fs_t scan_req;
    int scan_opened;
} dir_remove_frame_t;

/* Iteratively removes path and everything beneath it, post-order, with no
 * C-level recursion (clang-tidy's misc-no-recursion forbids it here): an
 * explicit heap stack holds one frame per directory level still being
 * drained, mirroring what real recursion would put on the call stack.
 * Each frame holds exactly one open uv_fs_t scandir result at a time,
 * cleaned up the moment that level's last entry is consumed and before
 * its own rmdir - so at most one scandir per level is ever open, matching
 * the design's recursion-depth-equals-tree-depth risk note. Within a
 * directory, a child that is itself a directory (lstat-ed, never
 * followed) pushes a new frame to be drained next; anything else
 * (regular file or symlink - read-only or not, since libuv clears
 * Windows' READONLY attribute before unlinking and POSIX unlink does not
 * care) is unlinked immediately, so a symlink is removed itself and
 * never resolved into another tree. The first failure anywhere unwinds
 * every still-open frame (cleaning up its scandir, freeing its path) and
 * returns 0 with error_code/detail set; everything removed up to that
 * point stays removed, and the rest of the tree - including whatever the
 * failing entry pointed at, if it was a symlink - is left untouched. */
static int dir_remove_tree(const char *path, taz_v1_ErrorCode *error_code,
                           const char **detail)
{
    dir_remove_frame_t *stack;
    size_t count = 1U;
    size_t capacity = 1U;
    int ok = 1;
    const size_t path_len = strlen(path);

    stack = (dir_remove_frame_t *)calloc(1U, sizeof(*stack));
    if (stack == NULL)
    {
        *error_code = taz_v1_ErrorCode_ERROR_CODE_INTERNAL;
        *detail = "out of memory";
        return 0;
    }
    stack[0].path = (char *)malloc(path_len + 1U);
    if (stack[0].path == NULL)
    {
        free(stack);
        *error_code = taz_v1_ErrorCode_ERROR_CODE_INTERNAL;
        *detail = "out of memory";
        return 0;
    }
    (void)memcpy(stack[0].path, path, path_len + 1U);

    while (count > 0U)
    {
        dir_remove_frame_t *top = &stack[count - 1U];
        uv_dirent_t ent;

        if (!top->scan_opened)
        {
            if (uv_fs_scandir(NULL, &top->scan_req, top->path, 0, NULL) < 0)
            {
                *error_code = taz_error_from_fs_req(&top->scan_req);
                *detail = taz_error_fs_detail(&top->scan_req);
                uv_fs_req_cleanup(&top->scan_req);
                ok = 0;
                break;
            }
            top->scan_opened = 1;
        }

        if (uv_fs_scandir_next(&top->scan_req, &ent) == UV_EOF)
        {
            uv_fs_t rmdir_req;

            uv_fs_req_cleanup(&top->scan_req);
            top->scan_opened = 0;

            if (uv_fs_rmdir(NULL, &rmdir_req, top->path, NULL) < 0)
            {
                *error_code = taz_error_from_fs_req(&rmdir_req);
                *detail = taz_error_fs_detail(&rmdir_req);
                uv_fs_req_cleanup(&rmdir_req);
                ok = 0;
                break;
            }
            uv_fs_req_cleanup(&rmdir_req);

            free(top->path);
            count--;
            continue;
        }

        {
            char *child = taz_fsutil_join(top->path, ent.name);
            uv_fs_t lstat_req;
            int is_dir;

            if (child == NULL)
            {
                *error_code = taz_v1_ErrorCode_ERROR_CODE_INTERNAL;
                *detail = "out of memory";
                ok = 0;
                break;
            }

            if (uv_fs_lstat(NULL, &lstat_req, child, NULL) < 0)
            {
                *error_code = taz_error_from_fs_req(&lstat_req);
                *detail = taz_error_fs_detail(&lstat_req);
                uv_fs_req_cleanup(&lstat_req);
                free(child);
                ok = 0;
                break;
            }
            is_dir = (taz_fsutil_kind_from_mode(lstat_req.statbuf.st_mode) ==
                      taz_v1_Kind_KIND_DIR);
            uv_fs_req_cleanup(&lstat_req);

            if (is_dir)
            {
                if (count == capacity)
                {
                    const size_t new_capacity = capacity * 2U;
                    dir_remove_frame_t *grown;

                    if (new_capacity > (SIZE_MAX / sizeof(*grown)))
                    {
                        free(child);
                        *error_code = taz_v1_ErrorCode_ERROR_CODE_INTERNAL;
                        *detail = "out of memory";
                        ok = 0;
                        break;
                    }
                    grown = (dir_remove_frame_t *)realloc(
                        stack, new_capacity * sizeof(*grown));
                    if (grown == NULL)
                    {
                        free(child);
                        *error_code = taz_v1_ErrorCode_ERROR_CODE_INTERNAL;
                        *detail = "out of memory";
                        ok = 0;
                        break;
                    }
                    stack = grown;
                    capacity = new_capacity;
                }

                stack[count].path = child;
                stack[count].scan_opened = 0;
                count++;
            }
            else
            {
                uv_fs_t unlink_req;
                if (uv_fs_unlink(NULL, &unlink_req, child, NULL) < 0)
                {
                    *error_code = taz_error_from_fs_req(&unlink_req);
                    *detail = taz_error_fs_detail(&unlink_req);
                    uv_fs_req_cleanup(&unlink_req);
                    free(child);
                    ok = 0;
                    break;
                }
                uv_fs_req_cleanup(&unlink_req);
                free(child);
            }
        }
    }

    /* Unwind whatever frames remain (all of them on success, since the
     * loop above only exits early via break on failure): each still owns
     * an open scandir (cleaned up here, if scan_opened) and a path
     * (freed here). */
    while (count > 0U)
    {
        dir_remove_frame_t *top = &stack[count - 1U];
        if (top->scan_opened)
        {
            uv_fs_req_cleanup(&top->scan_req);
        }
        free(top->path);
        count--;
    }
    free(stack);

    return ok;
}

/* Pool thread: lstat (never follow) the top-level path first - never jump
 * straight to uv_fs_rmdir - so a path that is a regular file reports a
 * stable error other than NOT_FOUND instead of whatever uv_fs_rmdir's own
 * ENOTDIR maps to on this platform (POSIX ENOTDIR maps to NOT_FOUND via
 * taz_error_from_errno, which would otherwise violate the "path is a file
 * -> error != NOT_FOUND" row); a missing path's ENOENT -> NOT_FOUND is
 * exactly what callers expect either way. uv_fs_rmdir never follows
 * symlinks either, so lstat (not stat) here matches what the final rmdir
 * would actually do: a symlink at the top level, even one pointing at a
 * directory, is reported as "not a directory" rather than silently
 * resolved. Without recursive, a single uv_fs_rmdir handles everything
 * (non-empty -> INTERNAL with detail); with recursive, dir_remove_tree
 * removes path and everything beneath it in one post-order walk. */
static void dir_remove_work(void *user)
{
    dir_remove_ctx_t *fctx = (dir_remove_ctx_t *)user;
    uv_fs_t stat_req;

    if (uv_fs_lstat(NULL, &stat_req, fctx->req.path, NULL) < 0)
    {
        fctx->ok = 0;
        fctx->error_code = taz_error_from_fs_req(&stat_req);
        fctx->detail = taz_error_fs_detail(&stat_req);
        uv_fs_req_cleanup(&stat_req);
        return;
    }
    if (taz_fsutil_kind_from_mode(stat_req.statbuf.st_mode) !=
        taz_v1_Kind_KIND_DIR)
    {
        uv_fs_req_cleanup(&stat_req);
        fctx->ok = 0;
        fctx->error_code = taz_v1_ErrorCode_ERROR_CODE_INTERNAL;
        fctx->detail = "path is not a directory";
        return;
    }
    uv_fs_req_cleanup(&stat_req);

    if (fctx->req.recursive)
    {
        if (!dir_remove_tree(fctx->req.path, &fctx->error_code, &fctx->detail))
        {
            fctx->ok = 0;
            return;
        }
    }
    else
    {
        uv_fs_t rmdir_req;
        if (uv_fs_rmdir(NULL, &rmdir_req, fctx->req.path, NULL) < 0)
        {
            fctx->ok = 0;
            fctx->error_code = taz_error_from_fs_req(&rmdir_req);
            fctx->detail = taz_error_fs_detail(&rmdir_req);
            uv_fs_req_cleanup(&rmdir_req);
            return;
        }
        uv_fs_req_cleanup(&rmdir_req);
    }

    fctx->resp.success = true;
    fctx->ok = 1;
}

static void dir_remove_done(void *user, int closing)
{
    dir_remove_ctx_t *fctx = (dir_remove_ctx_t *)user;

    if (!closing)
    {
        if (fctx->ok)
        {
            taz_response_send(fctx->write_fn, fctx->write_ctx, fctx->stream_id,
                              fctx->opcode, taz_v1_DirRemoveResponse_fields,
                              &fctx->resp);
        }
        else
        {
            taz_error_send(fctx->write_fn, fctx->write_ctx, fctx->stream_id,
                           fctx->opcode, fctx->error_code, "remove failed",
                           fctx->detail);
        }
    }
    free(fctx);
}

void handle_dir_remove(taz_dispatch_t *d, const taz_frame_header_t *header,
                       const uint8_t *payload, taz_dispatch_write_fn_t write_fn,
                       void *ctx)
{
    taz_v1_DirRemoveRequest req = taz_v1_DirRemoveRequest_init_zero;
    dir_remove_ctx_t *fctx;

    if (payload != NULL && header->length > 0U)
    {
        pb_istream_t istream =
            pb_istream_from_buffer(payload, (size_t)header->length);
        if (!pb_decode(&istream, taz_v1_DirRemoveRequest_fields, &req))
        {
            taz_error_send(write_fn, ctx, header->stream_id, header->opcode,
                           taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST,
                           "decode DirRemoveRequest failed", NULL);
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

    fctx = (dir_remove_ctx_t *)calloc(1U, sizeof(*fctx));
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

    if (taz_work_submit(d, header->stream_id, dir_remove_work, dir_remove_done,
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
