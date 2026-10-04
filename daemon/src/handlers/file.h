#ifndef TAZ_HANDLERS_FILE_H
#define TAZ_HANDLERS_FILE_H

#include <stdint.h>

#include "taz/dispatch.h"
#include "taz/frame.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /* Async FILE_STAT handler: decode the request, reject an empty path with
     * INVALID_REQUEST, then uv_fs_lstat (and, for a symlink, uv_fs_readlink)
     * the path on the thread pool and reply with a FileStatResponse (kind,
     * size, permissions, modified, created, owner, link_target). Owner is
     * resolved from /etc/passwd on POSIX; on Windows via
     * GetNamedSecurityInfoW + LookupAccountSidW as "DOMAIN\name", falling
     * back to "" on any failure. Closes the stream itself
     * (taz_dispatch_stream_done), either synchronously on a decode/
     * validation failure or later from the work-done callback. */
    void handle_file_stat(taz_dispatch_t *d, const taz_frame_header_t *header,
                          const uint8_t *payload,
                          taz_dispatch_write_fn_t write_fn, void *ctx);

    /* Async FILE_CREATE handler: decode the request (the content field is a
     * decode callback copied into a heap buffer while still on the loop
     * thread, since payload is only valid for the duration of this call),
     * reject an empty path with INVALID_REQUEST, then on the thread pool
     * exclusively create the file (permissions == 0 defaults to 0644) and
     * write all of content, closing it either way. Replies with a
     * FileCreateResponse(success=true) or an error (e.g. ALREADY_EXISTS when
     * the path exists, NOT_FOUND when a parent directory is missing). A
     * write failure closes the file and leaves whatever was written so far
     * on disk. Closes the stream itself. */
    void handle_file_create(taz_dispatch_t *d, const taz_frame_header_t *header,
                            const uint8_t *payload,
                            taz_dispatch_write_fn_t write_fn, void *ctx);

    /* Async FILE_DELETE handler: decode the request, reject an empty path
     * with INVALID_REQUEST, then uv_fs_unlink the path on the thread pool
     * (removes a symlink itself, never its target; never removes a
     * directory). Replies with a FileDeleteResponse(success=true) or an
     * error. Closes the stream itself. */
    void handle_file_delete(taz_dispatch_t *d, const taz_frame_header_t *header,
                            const uint8_t *payload,
                            taz_dispatch_write_fn_t write_fn, void *ctx);

    /* Async FILE_CHMOD handler: decode the request, reject an empty path
     * with INVALID_REQUEST, then uv_fs_chmod(path, permissions & 07777) on
     * the thread pool. On Windows, uv_fs_chmod only honours the owner-write
     * bit (READONLY attribute); on success there, after-work prints a
     * stderr warning when taz_fsutil_chmod_unrepresentable says the
     * requested mode cannot be faithfully represented. Replies with a
     * FileChmodResponse(success=true) or an error. Closes the stream
     * itself. */
    void handle_file_chmod(taz_dispatch_t *d, const taz_frame_header_t *header,
                           const uint8_t *payload,
                           taz_dispatch_write_fn_t write_fn, void *ctx);

    /* Async DIR_MAKE handler: decode the request, reject an empty path with
     * INVALID_REQUEST, then on the thread pool create the directory
     * (permissions == 0 defaults to 0755). With parents == false this is a
     * single uv_fs_mkdir (EEXIST -> ALREADY_EXISTS, a missing parent ->
     * NOT_FOUND). With parents == true (mkdir -p) it walks the path via
     * taz_fsutil_root_prefix_len / taz_fsutil_is_sep, creating each missing
     * prefix (ignoring EEXIST on a prefix); when the final component
     * already exists, success if it is a directory, else ALREADY_EXISTS.
     * Replies with a DirMakeResponse(success=true) or an error. Closes the
     * stream itself. */
    void handle_dir_make(taz_dispatch_t *d, const taz_frame_header_t *header,
                         const uint8_t *payload,
                         taz_dispatch_write_fn_t write_fn, void *ctx);

    /* Async DIR_LIST handler: decode the request, reject an empty path
     * with INVALID_REQUEST, then on the thread pool lstat the top-level
     * path first (so a regular-file path reports a stable error other
     * than NOT_FOUND instead of whatever scandir's own ENOTDIR happens to
     * map to on this platform) and uv_fs_scandir it, filtering hidden
     * names unless include_hidden and uv_fs_lstat-ing each survivor
     * (joined on the heap; an entry that vanished between scandir and
     * lstat is skipped). On the loop thread, encodes the collected
     * entries into batches of at most 64 DirEntry each, concatenates the
     * encoded bytes, and sends them via taz_response_send_encoded (so a
     * listing larger than one frame is split without ever cutting an
     * entry in half; an empty directory is one RESPONSE with zero
     * entries). Closes the stream itself. */
    void handle_dir_list(taz_dispatch_t *d, const taz_frame_header_t *header,
                         const uint8_t *payload,
                         taz_dispatch_write_fn_t write_fn, void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* TAZ_HANDLERS_FILE_H */
