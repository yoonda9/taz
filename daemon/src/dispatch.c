#include "taz/dispatch.h"

#include <string.h>

#include "handlers/command.h"
#include "handlers/config.h"
#include "handlers/file.h"
#include "handlers/file_transfer.h"
#include "handlers/version.h"
#include "taz/error.h"
#include "taz/v1/common.pb.h"

/* Max payload in an outgoing frame from the dispatch layer.  The dispatcher
 * sends only PONG (0 bytes), ERROR (<=4096 bytes) and small RESPONSE frames
 * (VERSION payload is O(100 bytes)).  Error frames set the ceiling. */
#define DISPATCH_SEND_BUFSZ                                                    \
    (TAZ_FRAME_HEADER_SIZE + TAZ_FRAME_MAX_PAYLOAD_ERROR)

/* --------------------------------------------------------------------------
 * Active-stream set (unsorted array; max TAZ_DISPATCH_MAX_STREAMS entries)
 * -------------------------------------------------------------------------- */

/* Index of id in the active set, or d->active_count (one past the last
 * valid index) when it is not active. */
static size_t stream_find(const taz_dispatch_t *d, uint32_t id)
{
    size_t i;
    for (i = 0U; i < d->active_count; ++i)
    {
        if (d->active_streams[i] == id)
        {
            return i;
        }
    }
    return d->active_count;
}

static int stream_is_active(const taz_dispatch_t *d, uint32_t id)
{
    return stream_find(d, id) < d->active_count;
}

static int stream_add(taz_dispatch_t *d, uint32_t id)
{
    if (d->active_count >= TAZ_DISPATCH_MAX_STREAMS)
    {
        return 0;
    }
    d->active_streams[d->active_count] = id;
    d->stream_execs[d->active_count] = NULL;
    d->stream_ops[d->active_count] = NULL;
    d->stream_user[d->active_count] = NULL;
    d->active_count++;
    return 1;
}

void taz_dispatch_stream_done(taz_dispatch_t *d, uint32_t stream_id)
{
    size_t i;
    for (i = 0U; i < d->active_count; ++i)
    {
        if (d->active_streams[i] == stream_id)
        {
            d->active_count--;
            d->active_streams[i] = d->active_streams[d->active_count];
            d->stream_execs[i] = d->stream_execs[d->active_count];
            d->stream_ops[i] = d->stream_ops[d->active_count];
            d->stream_user[i] = d->stream_user[d->active_count];
            return;
        }
    }
}

void taz_dispatch_set_stream_exec(taz_dispatch_t *d, uint32_t stream_id,
                                  taz_exec_t *exec)
{
    size_t i;
    for (i = 0U; i < d->active_count; ++i)
    {
        if (d->active_streams[i] == stream_id)
        {
            d->stream_execs[i] = exec;
            return;
        }
    }
}

void taz_dispatch_cancel_all(taz_dispatch_t *d)
{
    size_t i;
    for (i = 0U; i < d->active_count; ++i)
    {
        if (d->stream_execs[i] != NULL)
        {
            taz_exec_cancel(d->stream_execs[i]);
        }
        if (d->stream_ops[i] != NULL)
        {
            d->stream_ops[i]->abort(d->stream_user[i]);
        }
    }
}

void taz_dispatch_set_stream_ops(taz_dispatch_t *d, uint32_t stream_id,
                                 const taz_stream_ops_t *ops, void *user)
{
    const size_t idx = stream_find(d, stream_id);
    if (idx >= d->active_count)
    {
        return;
    }
    d->stream_ops[idx] = ops;
    d->stream_user[idx] = (ops != NULL) ? user : NULL;
}

int taz_dispatch_cancel_stream(taz_dispatch_t *d, uint32_t target,
                               taz_stream_done_fn_t done, void *arg)
{
    const size_t idx = stream_find(d, target);
    if (idx >= d->active_count || d->stream_ops[idx] == NULL)
    {
        return 0;
    }
    return d->stream_ops[idx]->cancel(d->stream_user[idx], done, arg);
}

void taz_dispatch_notify_writable(taz_dispatch_t *d)
{
    size_t i;
    for (i = 0U; i < d->active_count; ++i)
    {
        if (d->stream_ops[i] != NULL && d->stream_ops[i]->on_writable != NULL)
        {
            d->stream_ops[i]->on_writable(d->stream_user[i]);
        }
    }
}

size_t taz_dispatch_conn_write_queue_size(const taz_dispatch_t *d)
{
    if (d->conn_write_queue_size == NULL)
    {
        return 0U;
    }
    return d->conn_write_queue_size(d->conn_ctx);
}

void taz_dispatch_conn_pause_reads(taz_dispatch_t *d)
{
    if (d->conn_pause_reads != NULL)
    {
        d->conn_pause_reads(d->conn_ctx);
    }
}

void taz_dispatch_conn_resume_reads(taz_dispatch_t *d)
{
    if (d->conn_resume_reads != NULL)
    {
        d->conn_resume_reads(d->conn_ctx);
    }
}

void taz_dispatch_conn_ref(taz_dispatch_t *d)
{
    if (d->conn_ref != NULL)
    {
        d->conn_ref(d->conn_ctx);
    }
}

void taz_dispatch_conn_unref(taz_dispatch_t *d)
{
    if (d->conn_unref != NULL)
    {
        d->conn_unref(d->conn_ctx);
    }
}

int taz_dispatch_conn_closing(const taz_dispatch_t *d)
{
    if (d->conn_closing == NULL)
    {
        return 0;
    }
    return d->conn_closing(d->conn_ctx);
}

/* --------------------------------------------------------------------------
 * Frame-writing helpers
 * -------------------------------------------------------------------------- */

/* Pack header + payload into a single buffer and invoke write_fn. */
static void write_packed_frame(uint8_t type, uint8_t flags, uint16_t opcode,
                               uint32_t stream_id, const uint8_t *payload,
                               uint32_t pay_len,
                               taz_dispatch_write_fn_t write_fn, void *ctx)
{
    uint8_t buf[DISPATCH_SEND_BUFSZ];
    taz_frame_header_t h;

    if (pay_len > TAZ_FRAME_MAX_PAYLOAD_ERROR)
    {
        return;
    }

    h.type = type;
    h.flags = flags;
    h.opcode = opcode;
    h.length = pay_len;
    h.stream_id = stream_id;
    taz_frame_pack_header(&h, buf);

    if (pay_len > 0U && payload != NULL)
    {
        (void)memcpy(buf + TAZ_FRAME_HEADER_SIZE, payload, (size_t)pay_len);
    }

    write_fn(buf, (size_t)(TAZ_FRAME_HEADER_SIZE + pay_len), ctx);
}

static void send_error(uint32_t stream_id, uint16_t opcode,
                       taz_v1_ErrorCode code, const char *msg,
                       taz_dispatch_write_fn_t write_fn, void *ctx)
{
    uint8_t payload[TAZ_FRAME_MAX_PAYLOAD_ERROR];
    size_t len;

    len = taz_error_encode(payload, sizeof(payload), code, msg, NULL);
    if (len == 0U)
    {
        return;
    }

    write_packed_frame((uint8_t)taz_v1_FrameType_FRAME_TYPE_ERROR,
                       (uint8_t)taz_v1_FrameFlag_FRAME_FLAG_NONE, opcode,
                       stream_id, payload, (uint32_t)len, write_fn, ctx);
}

/* --------------------------------------------------------------------------
 * Per-opcode handlers
 * -------------------------------------------------------------------------- */

static void handle_ping(const taz_frame_header_t *header,
                        taz_dispatch_write_fn_t write_fn, void *ctx)
{
    write_packed_frame((uint8_t)taz_v1_FrameType_FRAME_TYPE_PONG,
                       (uint8_t)taz_v1_FrameFlag_FRAME_FLAG_NONE, 0U,
                       header->stream_id, NULL, 0U, write_fn, ctx);
}

/* --------------------------------------------------------------------------
 * Opcode dispatch table
 * -------------------------------------------------------------------------- */

/* Synchronous handlers return with their stream already finished;
 * taz_dispatch_frame closes it for them. */
typedef void (*sync_handler_fn_t)(const taz_frame_header_t *header,
                                  const uint8_t *payload,
                                  taz_dispatch_write_fn_t write_fn, void *ctx);

/* Async handlers receive d so they can read d->loop, register a taz_exec_t *
 * via taz_dispatch_set_stream_exec, and close their own stream later via
 * taz_dispatch_stream_done once their response is written. */
typedef void (*async_handler_fn_t)(taz_dispatch_t *d,
                                   const taz_frame_header_t *header,
                                   const uint8_t *payload,
                                   taz_dispatch_write_fn_t write_fn, void *ctx);

typedef struct
{
    sync_handler_fn_t sync_fn;   /* non-NULL iff !is_async */
    async_handler_fn_t async_fn; /* non-NULL iff is_async */
    uint16_t opcode;
    /* When true, taz_dispatch_frame leaves the stream open after async_fn
     * returns: async_fn owns calling taz_dispatch_stream_done later, once
     * its response has actually been written. It also requires d->loop to
     * be set; taz_dispatch_frame reports ERROR INTERNAL without calling
     * async_fn at all when it is NULL (e.g. a pure unit-test dispatch). */
    bool is_async;
} opcode_entry_t;

static const opcode_entry_t OPCODE_TABLE[] = {
    {handle_version, NULL, (uint16_t)taz_v1_Opcode_OPCODE_VERSION, false},
    {handle_configuration_get, NULL,
     (uint16_t)taz_v1_Opcode_OPCODE_CONFIGURATION_GET, false},
    {handle_configuration_update, NULL,
     (uint16_t)taz_v1_Opcode_OPCODE_CONFIGURATION_UPDATE, false},
    {NULL, handle_command_exec, (uint16_t)taz_v1_Opcode_OPCODE_COMMAND_EXEC,
     true},
    {NULL, handle_file_stat, (uint16_t)taz_v1_Opcode_OPCODE_FILE_STAT, true},
    {NULL, handle_file_create, (uint16_t)taz_v1_Opcode_OPCODE_FILE_CREATE,
     true},
    {NULL, handle_file_delete, (uint16_t)taz_v1_Opcode_OPCODE_FILE_DELETE,
     true},
    {NULL, handle_file_chmod, (uint16_t)taz_v1_Opcode_OPCODE_FILE_CHMOD, true},
    {NULL, handle_dir_make, (uint16_t)taz_v1_Opcode_OPCODE_DIR_MAKE, true},
    {NULL, handle_dir_list, (uint16_t)taz_v1_Opcode_OPCODE_DIR_LIST, true},
    {NULL, handle_dir_remove, (uint16_t)taz_v1_Opcode_OPCODE_DIR_REMOVE, true},
    {NULL, handle_file_put, (uint16_t)taz_v1_Opcode_OPCODE_FILE_PUT, true},
};

#define OPCODE_TABLE_SIZE (sizeof(OPCODE_TABLE) / sizeof(OPCODE_TABLE[0]))

static const opcode_entry_t *lookup_opcode(uint16_t opcode)
{
    size_t i;
    for (i = 0U; i < OPCODE_TABLE_SIZE; ++i)
    {
        if (OPCODE_TABLE[i].opcode == opcode)
        {
            return &OPCODE_TABLE[i];
        }
    }
    return NULL;
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

void taz_dispatch_init(taz_dispatch_t *d)
{
    (void)memset(d, 0, sizeof(*d));
}

void taz_dispatch_frame(taz_dispatch_t *d, const taz_frame_header_t *header,
                        const uint8_t *payload, taz_frame_verdict_t verdict,
                        taz_dispatch_write_fn_t write_fn, void *ctx)
{
    const opcode_entry_t *entry;

    /* OVERSIZED: connection.c sends PROTOCOL_ERROR and closes; the DONE
     * reassembly phase prevents any further calls here. */
    if (verdict == TAZ_FRAME_OVERSIZED)
    {
        return;
    }

    /* Unknown-type frame: report NOT_SUPPORTED and continue. */
    if (verdict == TAZ_FRAME_UNKNOWN_TYPE)
    {
        send_error(header->stream_id, header->opcode,
                   taz_v1_ErrorCode_ERROR_CODE_NOT_SUPPORTED,
                   "unknown frame type", write_fn, ctx);
        return;
    }

    /* Route by frame type. */
    if (header->type == (uint8_t)taz_v1_FrameType_FRAME_TYPE_PING)
    {
        handle_ping(header, write_fn, ctx);
        return;
    }

    /* FILE_CHUNK is routed by stream_id alone (opcode ignored) to the
     * sink registered on that stream; with no sink (unknown stream,
     * finished, or a plain REQUEST stream such as COMMAND_EXEC) it is
     * dropped silently: no ERROR, no close. */
    if (header->type == (uint8_t)taz_v1_FrameType_FRAME_TYPE_FILE_CHUNK)
    {
        const size_t idx = stream_find(d, header->stream_id);
        if (idx < d->active_count && d->stream_ops[idx] != NULL)
        {
            const uint8_t *chunk_payload =
                (header->length > 0U) ? payload : NULL;
            d->stream_ops[idx]->on_chunk(d->stream_user[idx], header,
                                         chunk_payload);
        }
        return;
    }

    if (header->type != (uint8_t)taz_v1_FrameType_FRAME_TYPE_REQUEST)
    {
        return;
    }

    /* REQUEST: duplicate stream_id means a conflicting in-flight request. */
    if (stream_is_active(d, header->stream_id))
    {
        send_error(header->stream_id, header->opcode,
                   taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST,
                   "duplicate stream_id", write_fn, ctx);
        return;
    }

    /* Unknown opcode: send NOT_SUPPORTED without opening the stream. */
    entry = lookup_opcode(header->opcode);
    if (entry == NULL)
    {
        send_error(header->stream_id, header->opcode,
                   taz_v1_ErrorCode_ERROR_CODE_NOT_SUPPORTED, "unknown opcode",
                   write_fn, ctx);
        return;
    }

    /* Async opcodes need a loop to spawn their work on; a dispatch without
     * one (e.g. a pure unit test) can never complete them, so reject before
     * opening a stream rather than hanging it forever. */
    if (entry->is_async && d->loop == NULL)
    {
        send_error(header->stream_id, header->opcode,
                   taz_v1_ErrorCode_ERROR_CODE_INTERNAL,
                   "async handler requires a loop", write_fn, ctx);
        return;
    }

    if (!stream_add(d, header->stream_id))
    {
        send_error(header->stream_id, header->opcode,
                   taz_v1_ErrorCode_ERROR_CODE_BUSY, "stream limit reached",
                   write_fn, ctx);
        return;
    }

    /* Sync handlers return with their work already done, so the stream
     * closes here. Async handlers keep it open and close it themselves,
     * later, once their response has actually been written. */
    if (entry->is_async)
    {
        entry->async_fn(d, header, payload, write_fn, ctx);
    }
    else
    {
        entry->sync_fn(header, payload, write_fn, ctx);
        taz_dispatch_stream_done(d, header->stream_id);
    }
}

bool taz_dispatch_opcode_is_async(uint16_t opcode)
{
    const opcode_entry_t *entry = lookup_opcode(opcode);
    if (entry == NULL)
    {
        return false;
    }
    return entry->is_async;
}
