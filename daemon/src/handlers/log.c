#include "log.h"

#include <stdlib.h>
#include <string.h>

#include <pb_decode.h>
#include <pb_encode.h>

#include "taz/error.h"
#include "taz/log.h"
#include "taz/v1/advanced.pb.h"
#include "taz/v1/common.pb.h"

/* taz_v1_LogResponse.entries' own max_count (advanced.options). One batch
 * is always one RESPONSE frame (a full batch's worst-case encoded size,
 * 34368 B, is well under TAZ_FRAME_MAX_PAYLOAD_RESPONSE), so the batch
 * boundary - not a byte-size splitter - is what callers see as the frame
 * boundary. Mirrors handlers/process.c's taz_process_list_send. */
#define TAZ_LOG_RESPONSE_BATCH_MAX 64U

/* Packs and writes one RESPONSE frame carrying payload[0..pay_len). Mirrors
 * response.c's own (private) flush_frame and process.c's
 * process_flush_single_frame: GCC's -fanalyzer loses track of a malloc'd
 * payload pointer's nullability when the whole malloc-encode-pack-memcpy
 * sequence is inlined across many lines in one function, so crossing a
 * function boundary here is load-bearing, not just tidiness. */
static void log_flush_frame(taz_dispatch_write_fn_t write_fn, void *ctx,
                            uint32_t stream_id, uint16_t opcode,
                            const uint8_t *payload, size_t pay_len,
                            uint8_t flags)
{
    const size_t total = (size_t)TAZ_FRAME_HEADER_SIZE + pay_len;
    uint8_t *buf = (uint8_t *)malloc(total);

    if (buf == NULL)
    {
        return;
    }

    {
        taz_frame_header_t h;
        h.type = (uint8_t)taz_v1_FrameType_FRAME_TYPE_RESPONSE;
        h.flags = flags;
        h.opcode = opcode;
        h.length = (uint32_t)pay_len;
        h.stream_id = stream_id;
        taz_frame_pack_header(&h, buf);
    }
    if (pay_len > 0U)
    {
        (void)memcpy(buf + TAZ_FRAME_HEADER_SIZE, payload, pay_len);
    }

    write_fn(buf, total, ctx);
    free(buf);
}

/* Sends entries[0..count) as one or more LogResponse RESPONSE frames,
 * batched at TAZ_LOG_RESPONSE_BATCH_MAX entries per frame. count == 0
 * still sends one RESPONSE frame with zero entries. */
static void log_send_entries(taz_dispatch_write_fn_t write_fn, void *ctx,
                             uint32_t stream_id, uint16_t opcode,
                             const taz_v1_LogEntry *entries, size_t count)
{
    size_t i = 0U;
    /* taz_v1_LogResponse is ~34 KB (64 x LogEntry) - too large for a stack
     * frame on a 1 MiB Windows thread, so it is heap-allocated once here
     * and reused batch to batch; entries_count caps what nanopb actually
     * encodes, so stale bytes left over from the previous batch past that
     * count are never read. */
    taz_v1_LogResponse *batch =
        (taz_v1_LogResponse *)calloc(1U, sizeof(*batch));

    if (batch == NULL)
    {
        return;
    }

    do
    {
        size_t batch_count = 0U;
        size_t batch_size = 0U;
        uint8_t *payload;

        while (i < count && batch_count < TAZ_LOG_RESPONSE_BATCH_MAX)
        {
            batch->entries[batch_count] = entries[i];
            batch_count++;
            i++;
        }
        batch->entries_count = (pb_size_t)batch_count;

        if (!pb_get_encoded_size(&batch_size, taz_v1_LogResponse_fields, batch))
        {
            free(batch);
            return;
        }

        payload = (batch_size > 0U) ? (uint8_t *)malloc(batch_size) : NULL;
        if (batch_size > 0U && payload == NULL)
        {
            free(batch);
            return;
        }

        if (batch_size > 0U)
        {
            pb_ostream_t ostream = pb_ostream_from_buffer(payload, batch_size);
            if (!pb_encode(&ostream, taz_v1_LogResponse_fields, batch))
            {
                free(payload);
                free(batch);
                return;
            }
            batch_size = ostream.bytes_written;
        }

        log_flush_frame(write_fn, ctx, stream_id, opcode, payload, batch_size,
                        (i < count)
                            ? (uint8_t)taz_v1_FrameFlag_FRAME_FLAG_CONTINUATION
                            : (uint8_t)taz_v1_FrameFlag_FRAME_FLAG_NONE);
        free(payload);
    } while (i < count);

    free(batch);
}

void handle_log(const taz_frame_header_t *header, const uint8_t *payload,
                taz_dispatch_write_fn_t write_fn, void *ctx)
{
    taz_v1_LogRequest req = taz_v1_LogRequest_init_zero;
    taz_log_level_t min_level = TAZ_LOG_DEBUG;
    taz_v1_LogEntry *entries;
    size_t count;

    if (payload != NULL && header->length > 0U)
    {
        pb_istream_t istream =
            pb_istream_from_buffer(payload, (size_t)header->length);
        if (!pb_decode(&istream, taz_v1_LogRequest_fields, &req))
        {
            taz_error_send(write_fn, ctx, header->stream_id, header->opcode,
                           taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST,
                           "decode LogRequest failed", NULL);
            return;
        }
    }

    if (taz_log_level_parse(req.level, &min_level) != 0)
    {
        taz_error_send(write_fn, ctx, header->stream_id, header->opcode,
                       taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST,
                       "unknown log level", NULL);
        return;
    }

    /* Heap-allocate: TAZ_LOG_RING_CAPACITY (4096) x taz_v1_LogEntry is
     * ~2 MB, far too large for any stack frame. taz_log_collect never
     * writes more than the ring's own capacity, so this bound is always
     * enough regardless of req.lines. */
    entries =
        (taz_v1_LogEntry *)malloc(TAZ_LOG_RING_CAPACITY * sizeof(*entries));
    if (entries == NULL)
    {
        taz_error_send(write_fn, ctx, header->stream_id, header->opcode,
                       taz_v1_ErrorCode_ERROR_CODE_INTERNAL, "out of memory",
                       NULL);
        return;
    }

    count = taz_log_collect(req.lines, req.since, min_level, entries,
                            TAZ_LOG_RING_CAPACITY);
    log_send_entries(write_fn, ctx, header->stream_id, header->opcode, entries,
                     count);
    free(entries);
}
