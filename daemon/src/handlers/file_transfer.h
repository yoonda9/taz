#ifndef TAZ_HANDLERS_FILE_TRANSFER_H
#define TAZ_HANDLERS_FILE_TRANSFER_H

#include <stdint.h>

#include "taz/dispatch.h"
#include "taz/frame.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /* Async FILE_PUT handler: decode FilePutRequest, reject an empty dest
     * with INVALID_REQUEST, then register stream_ops (on_chunk/cancel/abort)
     * before opening the destination so FILE_CHUNK frames that arrive ahead
     * of the Ack are queued rather than dropped. Open phase (pool): the
     * parent directory of dest must exist and be a directory (else
     * NOT_FOUND); dest already a directory is INVALID_REQUEST regardless of
     * overwrite; dest already a file without overwrite is ALREADY_EXISTS;
     * otherwise a temp file "<dest>.taz-<stream_id>.tmp" is created
     * exclusively (an existing temp of that name is BUSY, left untouched).
     * On success replies with FilePutResponse{ack{ready=true}} and starts
     * receiving chunks (each copied off the reassembly buffer, written on
     * the pool thread at the running offset while a CRC32C accumulates,
     * ingress paused past a 256 KiB backlog and resumed once it drains).
     * The final chunk (CONTINUATION clear) triggers fsync + close + an
     * atomic rename into dest and FilePutResponse{confirm{bytes_written,
     * checksum}}; any failure sends ERROR, unlinks the temp, and leaves
     * dest untouched. Every blocking call runs via taz_work_submit_step, so
     * taz_work_request_shutdown waits for an in-flight upload. Closes the
     * stream itself. */
    void handle_file_put(taz_dispatch_t *d, const taz_frame_header_t *header,
                         const uint8_t *payload,
                         taz_dispatch_write_fn_t write_fn, void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* TAZ_HANDLERS_FILE_TRANSFER_H */
