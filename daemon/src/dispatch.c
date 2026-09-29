#include "taz/dispatch.h"

#include <string.h>

#include "taz/error.h"
#include "taz/payload.h"
#include "taz/v1/common.pb.h"

/* Max payload in an outgoing frame from the dispatch layer.  The dispatcher
 * sends only PONG (0 bytes), ERROR (<=4096 bytes) and small RESPONSE frames
 * (VERSION payload is O(100 bytes)).  Error frames set the ceiling. */
#define DISPATCH_SEND_BUFSZ                                                    \
    (TAZ_FRAME_HEADER_SIZE + TAZ_FRAME_MAX_PAYLOAD_ERROR)

/* VERSION response payload buffer.  512 bytes is far more than the encoded
 * version/build/platform strings will ever need; encode will fail and we
 * send INTERNAL if it somehow overflows. */
#define VERSION_PAYLOAD_BUFSZ 512U

/* --------------------------------------------------------------------------
 * Active-stream set (unsorted array; max TAZ_DISPATCH_MAX_STREAMS entries)
 * -------------------------------------------------------------------------- */

static int stream_is_active(const taz_dispatch_t *d, uint32_t id)
{
    size_t i;
    for (i = 0U; i < d->active_count; ++i)
    {
        if (d->active_streams[i] == id)
        {
            return 1;
        }
    }
    return 0;
}

static int stream_add(taz_dispatch_t *d, uint32_t id)
{
    if (d->active_count >= TAZ_DISPATCH_MAX_STREAMS)
    {
        return 0;
    }
    d->active_streams[d->active_count] = id;
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
            return;
        }
    }
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

static void handle_version(const taz_frame_header_t *header,
                           const uint8_t *payload,
                           taz_dispatch_write_fn_t write_fn, void *ctx)
{
    uint8_t pay[VERSION_PAYLOAD_BUFSZ];
    size_t len;

    (void)payload;

    len = taz_payload_version_response(pay, sizeof(pay));
    if (len == 0U)
    {
        send_error(header->stream_id, header->opcode,
                   taz_v1_ErrorCode_ERROR_CODE_INTERNAL,
                   "version encode failed", write_fn, ctx);
        return;
    }

    write_packed_frame((uint8_t)taz_v1_FrameType_FRAME_TYPE_RESPONSE,
                       (uint8_t)taz_v1_FrameFlag_FRAME_FLAG_NONE,
                       header->opcode, header->stream_id, pay, (uint32_t)len,
                       write_fn, ctx);
}

/* --------------------------------------------------------------------------
 * Opcode dispatch table
 * -------------------------------------------------------------------------- */

typedef void (*handler_fn_t)(const taz_frame_header_t *header,
                             const uint8_t *payload,
                             taz_dispatch_write_fn_t write_fn, void *ctx);

typedef struct
{
    uint16_t opcode;
    handler_fn_t fn;
} opcode_entry_t;

static const opcode_entry_t OPCODE_TABLE[] = {
    {(uint16_t)taz_v1_Opcode_OPCODE_VERSION, handle_version},
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

    /* Open the stream, dispatch to the handler, then close it.  All current
     * handlers are synchronous; async handlers in future steps will call
     * taz_dispatch_stream_done themselves and not reach this point. */
    if (!stream_add(d, header->stream_id))
    {
        send_error(header->stream_id, header->opcode,
                   taz_v1_ErrorCode_ERROR_CODE_BUSY, "stream limit reached",
                   write_fn, ctx);
        return;
    }
    entry->fn(header, payload, write_fn, ctx);
    taz_dispatch_stream_done(d, header->stream_id);
}
