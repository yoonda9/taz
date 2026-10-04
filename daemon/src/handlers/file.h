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
     * resolved from /etc/passwd on POSIX and left empty on Windows (a later
     * task adds the Windows lookup). Closes the stream itself
     * (taz_dispatch_stream_done), either synchronously on a decode/
     * validation failure or later from the work-done callback. */
    void handle_file_stat(taz_dispatch_t *d, const taz_frame_header_t *header,
                          const uint8_t *payload,
                          taz_dispatch_write_fn_t write_fn, void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* TAZ_HANDLERS_FILE_H */
