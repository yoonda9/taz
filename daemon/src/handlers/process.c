#include "process.h"

#include <stdlib.h>
#include <string.h>

#include <pb_decode.h>
#include <pb_encode.h>

#include "taz/error.h"
#include "taz/v1/common.pb.h"
#include "taz/v1/process.pb.h"
#include "taz/work.h"

/* taz_v1_ProcessListResponse.processes' own max_count (process.options). One
 * batch is always one RESPONSE frame (a full batch's worst-case encoded size,
 * 48768 B, is well under TAZ_FRAME_MAX_PAYLOAD_RESPONSE), so the batch
 * boundary - not the generic byte-size splitter - is what callers see as the
 * frame boundary. */
#define TAZ_PROCESS_LIST_BATCH_MAX 128U

/* Packs and writes one RESPONSE frame carrying payload[0..pay_len). Mirrors
 * response.c's own (private) flush_frame: taz_response_send_encoded's
 * generic byte-size splitter would not reproduce the per-batch framing
 * taz_process_list_send's/taz_process_info_send's batching is testable
 * against. Shared by both of those and by handle_process_kill's single
 * ProcessKillResponse frame: GCC's -fanalyzer (unlike clang-tidy/cppcheck/
 * MSVC /analyze) loses track of a malloc'd payload pointer's nullability
 * when the whole malloc-encode-pack-memcpy sequence is inlined across many
 * lines in one function, so crossing a function boundary here is load-
 * bearing, not just tidiness. */
static void process_flush_single_frame(taz_dispatch_write_fn_t write_fn,
                                       void *ctx, uint32_t stream_id,
                                       uint16_t opcode, const uint8_t *payload,
                                       size_t pay_len, uint8_t flags)
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

void taz_process_list_send(taz_dispatch_write_fn_t write_fn, void *ctx,
                           uint32_t stream_id, uint16_t opcode,
                           const taz_process_entry_t *entries, size_t count)
{
    size_t i = 0U;
    /* taz_v1_ProcessListResponse is ~48 KB (128 x ProcessInfo) - too large
     * for a stack frame on a 1 MiB Windows thread, so it is heap-allocated
     * once here and reused batch to batch; processes_count caps what
     * nanopb actually encodes, so stale bytes left over from the previous
     * batch past that count are never read. */
    taz_v1_ProcessListResponse *batch =
        (taz_v1_ProcessListResponse *)calloc(1U, sizeof(*batch));

    if (batch == NULL)
    {
        return;
    }

    do
    {
        size_t batch_count = 0U;
        size_t batch_size = 0U;
        uint8_t *payload;

        while (i < count && batch_count < TAZ_PROCESS_LIST_BATCH_MAX)
        {
            taz_v1_ProcessInfo *info = &batch->processes[batch_count];
            (void)strncpy(info->name, entries[i].name, sizeof(info->name) - 1U);
            info->name[sizeof(info->name) - 1U] = '\0';
            (void)strncpy(info->user, entries[i].user, sizeof(info->user) - 1U);
            info->user[sizeof(info->user) - 1U] = '\0';
            (void)strncpy(info->state, entries[i].state,
                          sizeof(info->state) - 1U);
            info->state[sizeof(info->state) - 1U] = '\0';
            info->pid = entries[i].pid;
            info->cpu_percent = entries[i].cpu_percent;
            info->memory_bytes = entries[i].memory_bytes;
            batch_count++;
            i++;
        }
        batch->processes_count = (pb_size_t)batch_count;

        if (!pb_get_encoded_size(&batch_size, taz_v1_ProcessListResponse_fields,
                                 batch))
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
            if (!pb_encode(&ostream, taz_v1_ProcessListResponse_fields, batch))
            {
                free(payload);
                free(batch);
                return;
            }
            batch_size = ostream.bytes_written;
        }

        process_flush_single_frame(
            write_fn, ctx, stream_id, opcode, payload, batch_size,
            (i < count) ? (uint8_t)taz_v1_FrameFlag_FRAME_FLAG_CONTINUATION
                        : (uint8_t)taz_v1_FrameFlag_FRAME_FLAG_NONE);
        free(payload);
    } while (i < count);

    free(batch);
}

/* Carries everything process_list_work/process_list_done need. filter is
 * snapshotted from the request on the loop thread; list is filled on the
 * pool thread and freed by process_list_done either way. */
typedef struct
{
    taz_dispatch_write_fn_t write_fn;
    void *write_ctx;
    uint32_t stream_id;
    uint16_t opcode;
    char filter[256];

    int ok;
    taz_v1_ErrorCode error_code;
    const char *detail;

    taz_process_list_t list;
} process_list_ctx_t;

static void process_list_work(void *user)
{
    process_list_ctx_t *pctx = (process_list_ctx_t *)user;

    pctx->ok = (taz_process_enumerate(pctx->filter, &pctx->list,
                                      &pctx->error_code, &pctx->detail) == 0);
}

static void process_list_done(void *user, int closing)
{
    process_list_ctx_t *pctx = (process_list_ctx_t *)user;

    if (!closing)
    {
        if (pctx->ok)
        {
            taz_process_list_send(pctx->write_fn, pctx->write_ctx,
                                  pctx->stream_id, pctx->opcode,
                                  pctx->list.entries, pctx->list.count);
        }
        else
        {
            taz_error_send(pctx->write_fn, pctx->write_ctx, pctx->stream_id,
                           pctx->opcode, pctx->error_code, "list failed",
                           pctx->detail);
        }
    }
    taz_process_list_free(&pctx->list);
    free(pctx);
}

void handle_process_list(taz_dispatch_t *d, const taz_frame_header_t *header,
                         const uint8_t *payload,
                         taz_dispatch_write_fn_t write_fn, void *ctx)
{
    taz_v1_ProcessListRequest req = taz_v1_ProcessListRequest_init_zero;
    process_list_ctx_t *pctx;

    if (payload != NULL && header->length > 0U)
    {
        pb_istream_t istream =
            pb_istream_from_buffer(payload, (size_t)header->length);
        if (!pb_decode(&istream, taz_v1_ProcessListRequest_fields, &req))
        {
            taz_error_send(write_fn, ctx, header->stream_id, header->opcode,
                           taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST,
                           "decode ProcessListRequest failed", NULL);
            taz_dispatch_stream_done(d, header->stream_id);
            return;
        }
    }

    pctx = (process_list_ctx_t *)calloc(1U, sizeof(*pctx));
    if (pctx == NULL)
    {
        taz_error_send(write_fn, ctx, header->stream_id, header->opcode,
                       taz_v1_ErrorCode_ERROR_CODE_INTERNAL, "out of memory",
                       NULL);
        taz_dispatch_stream_done(d, header->stream_id);
        return;
    }
    pctx->write_fn = write_fn;
    pctx->write_ctx = ctx;
    pctx->stream_id = header->stream_id;
    pctx->opcode = header->opcode;
    (void)strncpy(pctx->filter, req.filter, sizeof(pctx->filter) - 1U);
    pctx->filter[sizeof(pctx->filter) - 1U] = '\0';

    if (taz_work_submit(d, header->stream_id, process_list_work,
                        process_list_done, pctx) != 0)
    {
        free(pctx);
        taz_error_send(write_fn, ctx, header->stream_id, header->opcode,
                       taz_v1_ErrorCode_ERROR_CODE_INTERNAL,
                       "work submit failed", NULL);
        taz_dispatch_stream_done(d, header->stream_id);
        return;
    }
}

/* taz_v1_ProcessInfoResponse.open_files' own max_count (process.options).
 * One batch holds at most 32 file paths. */
#define TAZ_PROCESS_INFO_BATCH_MAX 32U

void taz_process_info_send(taz_dispatch_write_fn_t write_fn, void *ctx,
                           uint32_t stream_id, uint16_t opcode,
                           const taz_process_detail_t *detail)
{
    size_t i = 0U;
    int first_frame = 1;
    /* taz_v1_ProcessInfoResponse is ~33 KB (1 ProcessInfo + 32 x 1KB open
     * files) - too large for a stack frame, so it is heap-allocated and
     * reused batch to batch. */
    taz_v1_ProcessInfoResponse *batch =
        (taz_v1_ProcessInfoResponse *)calloc(1U, sizeof(*batch));

    if (batch == NULL)
    {
        return;
    }

    do
    {
        size_t batch_count = 0U;
        size_t batch_size = 0U;
        uint8_t *payload;
        int is_last = 0;

        /* command_line goes on the first frame only: a chunked response's
         * strings are concatenated in frame order (protocol §6.1). */
        if (first_frame)
        {
            if (detail->command_line != NULL)
            {
                (void)strncpy(batch->command_line, detail->command_line,
                              sizeof(batch->command_line) - 1U);
            }
            batch->command_line[sizeof(batch->command_line) - 1U] = '\0';
            first_frame = 0;
        }

        /* Pack open_files entries up to batch max, starting from index i. */
        while (i < detail->open_files_count &&
               batch_count < TAZ_PROCESS_INFO_BATCH_MAX)
        {
            (void)strncpy(batch->open_files[batch_count], detail->open_files[i],
                          sizeof(batch->open_files[batch_count]) - 1U);
            batch->open_files[batch_count]
                             [sizeof(batch->open_files[batch_count]) - 1U] =
                '\0';
            batch_count++;
            i++;
        }
        batch->open_files_count = (pb_size_t)batch_count;

        /* info and start_time go on the last frame only, where a chunked
         * response's scalars are taken from (protocol §6.1). */
        is_last = (i >= detail->open_files_count);
        if (is_last)
        {
            taz_v1_ProcessInfo *info = &batch->info;
            /* taz_v1_ProcessInfoResponse.info is a proto3 singular message
             * field: nanopb only encodes it when has_info is set, so a
             * forgotten flag here silently drops the whole submessage
             * (pid/name/user/state/cpu_percent/memory_bytes) from the wire
             * with no encode-time error. */
            batch->has_info = true;
            (void)strncpy(info->name, detail->info.name,
                          sizeof(info->name) - 1U);
            info->name[sizeof(info->name) - 1U] = '\0';
            (void)strncpy(info->user, detail->info.user,
                          sizeof(info->user) - 1U);
            info->user[sizeof(info->user) - 1U] = '\0';
            (void)strncpy(info->state, detail->info.state,
                          sizeof(info->state) - 1U);
            info->state[sizeof(info->state) - 1U] = '\0';
            info->pid = detail->info.pid;
            info->cpu_percent = detail->info.cpu_percent;
            info->memory_bytes = detail->info.memory_bytes;
            batch->start_time = detail->start_time;
        }

        if (!pb_get_encoded_size(&batch_size, taz_v1_ProcessInfoResponse_fields,
                                 batch))
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
            if (!pb_encode(&ostream, taz_v1_ProcessInfoResponse_fields, batch))
            {
                free(payload);
                free(batch);
                return;
            }
            batch_size = ostream.bytes_written;
        }

        process_flush_single_frame(
            write_fn, ctx, stream_id, opcode, payload, batch_size,
            is_last ? (uint8_t)taz_v1_FrameFlag_FRAME_FLAG_NONE
                    : (uint8_t)taz_v1_FrameFlag_FRAME_FLAG_CONTINUATION);
        free(payload);

        /* Clear batch for next iteration. */
        (void)memset(batch, 0, sizeof(*batch));
    } while (i < detail->open_files_count);

    free(batch);
}

void handle_process_kill(const taz_frame_header_t *header,
                         const uint8_t *payload,
                         taz_dispatch_write_fn_t write_fn, void *ctx)
{
    taz_v1_ProcessKillRequest req = taz_v1_ProcessKillRequest_init_zero;

    if (payload != NULL && header->length > 0U)
    {
        pb_istream_t istream =
            pb_istream_from_buffer(payload, (size_t)header->length);
        if (!pb_decode(&istream, taz_v1_ProcessKillRequest_fields, &req))
        {
            taz_error_send(write_fn, ctx, header->stream_id, header->opcode,
                           taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST,
                           "decode ProcessKillRequest failed", NULL);
            return;
        }
    }

    /* Validation: pid must be positive (1..INT32_MAX). */
    if (req.pid == 0U || req.pid > (uint32_t)INT32_MAX)
    {
        taz_error_send(write_fn, ctx, header->stream_id, header->opcode,
                       taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST,
                       "pid must be positive and <= 2147483647", NULL);
        return;
    }

    /* Validation: signal must be non-negative. On POSIX, 0 means SIGTERM
     * (default); on Windows, 0 means default terminate. */
    if (req.signal < 0)
    {
        taz_error_send(write_fn, ctx, header->stream_id, header->opcode,
                       taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST,
                       "signal must be non-negative", NULL);
        return;
    }

    /* Call taz_process_kill inline (loop thread, no pool work). */
    {
        taz_v1_ErrorCode error_code;
        const char *detail = NULL;

        if (taz_process_kill(req.pid, req.signal, &error_code, &detail) == 0)
        {
            /* Success: encode and send ProcessKillResponse. */
            taz_v1_ProcessKillResponse resp =
                taz_v1_ProcessKillResponse_init_zero;
            resp.success = true;

            size_t resp_size = 0U;
            if (!pb_get_encoded_size(&resp_size,
                                     taz_v1_ProcessKillResponse_fields, &resp))
            {
                taz_error_send(write_fn, ctx, header->stream_id, header->opcode,
                               taz_v1_ErrorCode_ERROR_CODE_INTERNAL,
                               "encode size failed", NULL);
                return;
            }

            uint8_t *payload_buf =
                (resp_size > 0U) ? (uint8_t *)malloc(resp_size) : NULL;
            if (resp_size > 0U && payload_buf == NULL)
            {
                taz_error_send(write_fn, ctx, header->stream_id, header->opcode,
                               taz_v1_ErrorCode_ERROR_CODE_INTERNAL,
                               "out of memory", NULL);
                return;
            }

            if (resp_size > 0U)
            {
                pb_ostream_t ostream =
                    pb_ostream_from_buffer(payload_buf, resp_size);
                if (!pb_encode(&ostream, taz_v1_ProcessKillResponse_fields,
                               &resp))
                {
                    free(payload_buf);
                    taz_error_send(write_fn, ctx, header->stream_id,
                                   header->opcode,
                                   taz_v1_ErrorCode_ERROR_CODE_INTERNAL,
                                   "encode failed", NULL);
                    return;
                }
                resp_size = ostream.bytes_written;
            }

            process_flush_single_frame(
                write_fn, ctx, header->stream_id, header->opcode, payload_buf,
                resp_size, (uint8_t)taz_v1_FrameFlag_FRAME_FLAG_NONE);
            free(payload_buf);
        }
        else
        {
            /* Failure: taz_process_kill returned error. */
            taz_error_send(write_fn, ctx, header->stream_id, header->opcode,
                           error_code, "kill failed", detail);
        }
    }
}

/* Carries everything process_info_work/process_info_done need. */
typedef struct
{
    taz_dispatch_write_fn_t write_fn;
    void *write_ctx;
    uint32_t stream_id;
    uint16_t opcode;

    int ok;
    taz_v1_ErrorCode error_code;
    const char *detail;

    taz_process_detail_t detail_data;
} process_info_ctx_t;

static void process_info_work(void *user)
{
    process_info_ctx_t *pctx = (process_info_ctx_t *)user;

    pctx->ok =
        (taz_process_inspect(pctx->detail_data.info.pid, &pctx->detail_data,
                             &pctx->error_code, &pctx->detail) == 0);
}

static void process_info_done(void *user, int closing)
{
    process_info_ctx_t *pctx = (process_info_ctx_t *)user;

    if (!closing)
    {
        if (pctx->ok)
        {
            taz_process_info_send(pctx->write_fn, pctx->write_ctx,
                                  pctx->stream_id, pctx->opcode,
                                  &pctx->detail_data);
        }
        else
        {
            taz_error_send(pctx->write_fn, pctx->write_ctx, pctx->stream_id,
                           pctx->opcode, pctx->error_code, "info failed",
                           pctx->detail);
        }
    }
    taz_process_detail_free(&pctx->detail_data);
    free(pctx);
}

void handle_process_info(taz_dispatch_t *d, const taz_frame_header_t *header,
                         const uint8_t *payload,
                         taz_dispatch_write_fn_t write_fn, void *ctx)
{
    taz_v1_ProcessInfoRequest req = taz_v1_ProcessInfoRequest_init_zero;
    process_info_ctx_t *pctx;

    if (payload != NULL && header->length > 0U)
    {
        pb_istream_t istream =
            pb_istream_from_buffer(payload, (size_t)header->length);
        if (!pb_decode(&istream, taz_v1_ProcessInfoRequest_fields, &req))
        {
            taz_error_send(write_fn, ctx, header->stream_id, header->opcode,
                           taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST,
                           "decode ProcessInfoRequest failed", NULL);
            taz_dispatch_stream_done(d, header->stream_id);
            return;
        }
    }

    /* Validation: pid must be positive (1..INT32_MAX). */
    if (req.pid == 0U || req.pid > (uint32_t)INT32_MAX)
    {
        taz_error_send(write_fn, ctx, header->stream_id, header->opcode,
                       taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST,
                       "pid must be positive and <= 2147483647", NULL);
        taz_dispatch_stream_done(d, header->stream_id);
        return;
    }

    pctx = (process_info_ctx_t *)calloc(1U, sizeof(*pctx));
    if (pctx == NULL)
    {
        taz_error_send(write_fn, ctx, header->stream_id, header->opcode,
                       taz_v1_ErrorCode_ERROR_CODE_INTERNAL, "out of memory",
                       NULL);
        taz_dispatch_stream_done(d, header->stream_id);
        return;
    }
    pctx->write_fn = write_fn;
    pctx->write_ctx = ctx;
    pctx->stream_id = header->stream_id;
    pctx->opcode = header->opcode;
    pctx->detail_data.info.pid = req.pid;

    if (taz_work_submit(d, header->stream_id, process_info_work,
                        process_info_done, pctx) != 0)
    {
        free(pctx);
        taz_error_send(write_fn, ctx, header->stream_id, header->opcode,
                       taz_v1_ErrorCode_ERROR_CODE_INTERNAL,
                       "work submit failed", NULL);
        taz_dispatch_stream_done(d, header->stream_id);
        return;
    }
}
