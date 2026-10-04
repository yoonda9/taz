#include "taz/response.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <pb_encode.h>

#include "taz/frame.h"
#include "taz/v1/common.pb.h"

/* -------------------------------------------------------------------------
 * Varint helpers (protobuf wire format)
 * ------------------------------------------------------------------------- */

/* Low 7 bits of each varint byte. */
#define VARINT_DATA_MASK 0x7FU
/* Continuation bit in a varint byte. */
#define VARINT_CONT_MASK 0x80U
/* Bits per varint group. */
#define VARINT_BITS_PER_GROUP 7U
/* Wire-type bits in a protobuf tag. */
#define WIRE_TYPE_BITS 3U
/* Mask to extract wire type from a tag word. */
#define WIRE_TYPE_MASK 7U
/* Maximum varint shift before overflow (covers all uint64_t values). */
#define VARINT_MAX_SHIFT 64U
/* Bytes in a 64-bit fixed-width field. */
#define FIXED64_BYTES 8U
/* Bytes in a 32-bit fixed-width field. */
#define FIXED32_BYTES 4U
/* Conservative length-varint overhead per LEN sub-chunk (up to 2 MiB). */
#define LEN_VARINT_OVERHEAD 3U

/* Read one varint from buf[*pos..size].  Advances *pos.  Returns 0 on
 * success, -1 on truncated or overflow input. */
static int read_varint(const uint8_t *buf, size_t size, size_t *pos,
                       uint64_t *val)
{
    unsigned int shift = 0U;
    *val = 0U;
    while (*pos < size)
    {
        uint8_t b = buf[(*pos)++];
        *val |= (uint64_t)(b & VARINT_DATA_MASK) << shift;
        if ((b & VARINT_CONT_MASK) == 0U)
        {
            return 0;
        }
        shift += VARINT_BITS_PER_GROUP;
        if (shift >= VARINT_MAX_SHIFT)
        {
            return -1;
        }
    }
    return -1;
}

/* Number of bytes needed to encode v as a varint. */
static size_t varint_enc_size(uint64_t v)
{
    size_t n = 1U;
    while (v >= VARINT_CONT_MASK)
    {
        v >>= VARINT_BITS_PER_GROUP;
        n++;
    }
    return n;
}

/* Write v as a varint into buf.  Returns bytes written. */
static size_t write_varint(uint8_t *buf, uint64_t v)
{
    size_t n = 0U;
    while (v >= VARINT_CONT_MASK)
    {
        buf[n++] = (uint8_t)((v & VARINT_DATA_MASK) | VARINT_CONT_MASK);
        v >>= VARINT_BITS_PER_GROUP;
    }
    buf[n++] = (uint8_t)v;
    return n;
}

/* -------------------------------------------------------------------------
 * Frame flush helper
 * ------------------------------------------------------------------------- */

static void flush_frame(taz_dispatch_write_fn_t write_fn, void *ctx,
                        uint32_t stream_id, uint16_t opcode,
                        const uint8_t *payload, size_t pay_len, uint8_t flags)
{
    size_t total = TAZ_FRAME_HEADER_SIZE + pay_len;
    uint8_t *buf = (uint8_t *)malloc(total);
    if (buf == NULL)
    {
        return;
    }

    taz_frame_header_t h;
    h.type = (uint8_t)taz_v1_FrameType_FRAME_TYPE_RESPONSE;
    h.flags = flags;
    h.opcode = opcode;
    h.length = (uint32_t)pay_len;
    h.stream_id = stream_id;
    taz_frame_pack_header(&h, buf);
    if (pay_len > 0U)
    {
        (void)memcpy(buf + TAZ_FRAME_HEADER_SIZE, payload, pay_len);
    }

    write_fn(buf, total, ctx);
    free(buf);
}

/* -------------------------------------------------------------------------
 * Chunked (slow) path
 * ------------------------------------------------------------------------- */

/* Wire types. */
#define WT_VARINT 0U
#define WT_I64    1U
#define WT_LEN    2U
#define WT_I32    5U

static void send_chunked(taz_dispatch_write_fn_t write_fn, void *ctx,
                         uint32_t stream_id, uint16_t opcode,
                         const uint8_t *encoded, size_t encoded_size)
{
    /* Scratch buffers for frame accumulation. */
    const size_t limit = TAZ_FRAME_MAX_PAYLOAD_RESPONSE;
    uint8_t *frame_buf = (uint8_t *)malloc(limit);
    uint8_t *scalar_buf = (uint8_t *)malloc(limit);
    if (frame_buf == NULL || scalar_buf == NULL)
    {
        free(frame_buf);
        free(scalar_buf);
        return;
    }

    size_t frame_pos = 0U;
    size_t scalar_pos = 0U;
    size_t pos = 0U;

    while (pos < encoded_size)
    {
        size_t entry_start = pos;

        /* Read the tag+wire-type varint. */
        uint64_t tag_word;
        if (read_varint(encoded, encoded_size, &pos, &tag_word) != 0)
        {
            break;
        }
        uint32_t wire_type = (uint32_t)(tag_word & WIRE_TYPE_MASK);

        /* Measure the value bytes that follow the tag. */
        if (wire_type == WT_VARINT)
        {
            uint64_t dummy;
            if (read_varint(encoded, encoded_size, &pos, &dummy) != 0)
            {
                break;
            }
            /* Scalar: defer to last frame. */
            size_t entry_size = pos - entry_start;
            if (scalar_pos + entry_size <= limit)
            {
                (void)memcpy(scalar_buf + scalar_pos, encoded + entry_start,
                             entry_size);
                scalar_pos += entry_size;
            }
        }
        else if (wire_type == WT_I64)
        {
            if (pos + FIXED64_BYTES > encoded_size)
            {
                break;
            }
            pos += FIXED64_BYTES;
            /* Scalar. */
            size_t entry_size = pos - entry_start;
            if (scalar_pos + entry_size <= limit)
            {
                (void)memcpy(scalar_buf + scalar_pos, encoded + entry_start,
                             entry_size);
                scalar_pos += entry_size;
            }
        }
        else if (wire_type == WT_I32)
        {
            if (pos + FIXED32_BYTES > encoded_size)
            {
                break;
            }
            pos += FIXED32_BYTES;
            /* Scalar. */
            size_t entry_size = pos - entry_start;
            if (scalar_pos + entry_size <= limit)
            {
                (void)memcpy(scalar_buf + scalar_pos, encoded + entry_start,
                             entry_size);
                scalar_pos += entry_size;
            }
        }
        else if (wire_type == WT_LEN)
        {
            uint64_t data_len;
            if (read_varint(encoded, encoded_size, &pos, &data_len) != 0)
            {
                break;
            }
            size_t data_start = pos;
            if (data_start + (size_t)data_len > encoded_size)
            {
                break;
            }
            pos += (size_t)data_len;
            size_t entry_size = pos - entry_start;

            /* Field number (for re-encoding sub-chunks). */
            uint64_t field_num = tag_word >> WIRE_TYPE_BITS;
            uint64_t new_tag = (field_num << WIRE_TYPE_BITS) | (uint64_t)WT_LEN;
            size_t tag_enc_sz = varint_enc_size(new_tag);

            if (entry_size <= limit)
            {
                /* Atomic: fits in one frame. */
                if (frame_pos + entry_size > limit)
                {
                    /* Flush current frame with CONTINUATION. */
                    flush_frame(
                        write_fn, ctx, stream_id, opcode, frame_buf, frame_pos,
                        (uint8_t)taz_v1_FrameFlag_FRAME_FLAG_CONTINUATION);
                    frame_pos = 0U;
                }
                (void)memcpy(frame_buf + frame_pos, encoded + entry_start,
                             entry_size);
                frame_pos += entry_size;
            }
            else
            {
                /* Too large for one frame: split data into sub-chunks.
                 * Each sub-chunk is re-encoded as (tag, sub_len, sub_data).
                 * This is valid only for bytes/string fields; the receiver
                 * concatenates them per §6.1. */
                const uint8_t *data_ptr = encoded + data_start;
                size_t remaining = (size_t)data_len;

                while (remaining > 0U)
                {
                    /* How much data can we fit in the frame with the
                     * overhead of tag + length varint? Reserve 3 bytes
                     * for the length varint (handles payloads up to
                     * 2 MiB). */
                    size_t overhead = tag_enc_sz + LEN_VARINT_OVERHEAD;
                    size_t space = (frame_pos + overhead < limit)
                                       ? (limit - frame_pos - overhead)
                                       : 0U;

                    if (space == 0U)
                    {
                        /* Flush and start fresh. */
                        if (frame_pos > 0U)
                        {
                            flush_frame(
                                write_fn, ctx, stream_id, opcode, frame_buf,
                                frame_pos,
                                (uint8_t)
                                    taz_v1_FrameFlag_FRAME_FLAG_CONTINUATION);
                            frame_pos = 0U;
                        }
                        space = limit - tag_enc_sz - LEN_VARINT_OVERHEAD;
                    }

                    size_t chunk = (remaining < space) ? remaining : space;

                    /* Write (tag, len, data) into frame_buf. */
                    frame_pos += write_varint(frame_buf + frame_pos, new_tag);
                    frame_pos +=
                        write_varint(frame_buf + frame_pos, (uint64_t)chunk);
                    (void)memcpy(frame_buf + frame_pos, data_ptr, chunk);
                    frame_pos += chunk;
                    data_ptr += chunk;
                    remaining -= chunk;
                }
            }
        }
        else
        {
            /* Unknown wire type: skip and continue best-effort. */
            break;
        }
    }

    /* Flush the final frame: append scalars and send without CONTINUATION. */
    if (frame_pos + scalar_pos <= limit)
    {
        (void)memcpy(frame_buf + frame_pos, scalar_buf, scalar_pos);
        frame_pos += scalar_pos;
        flush_frame(write_fn, ctx, stream_id, opcode, frame_buf, frame_pos,
                    (uint8_t)taz_v1_FrameFlag_FRAME_FLAG_NONE);
    }
    else
    {
        /* Scalars don't fit with remaining non-scalars: emit two frames. */
        flush_frame(write_fn, ctx, stream_id, opcode, frame_buf, frame_pos,
                    (uint8_t)taz_v1_FrameFlag_FRAME_FLAG_CONTINUATION);
        flush_frame(write_fn, ctx, stream_id, opcode, scalar_buf, scalar_pos,
                    (uint8_t)taz_v1_FrameFlag_FRAME_FLAG_NONE);
    }

    free(frame_buf);
    free(scalar_buf);
}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

void taz_response_send_encoded(taz_dispatch_write_fn_t write_fn, void *ctx,
                               uint32_t stream_id, uint16_t opcode,
                               const uint8_t *buf, size_t len)
{
    if (len <= TAZ_FRAME_MAX_PAYLOAD_RESPONSE)
    {
        flush_frame(write_fn, ctx, stream_id, opcode, buf, len,
                    (uint8_t)taz_v1_FrameFlag_FRAME_FLAG_NONE);
    }
    else
    {
        /* Too large for one frame: split field-by-field. */
        send_chunked(write_fn, ctx, stream_id, opcode, buf, len);
    }
}

void taz_response_send(taz_dispatch_write_fn_t write_fn, void *ctx,
                       uint32_t stream_id, uint16_t opcode,
                       const pb_msgdesc_t *fields, const void *msg)
{
    /* Size first and encode into the heap: a frame-sized stack buffer would
     * be 64 KiB.  The spare byte keeps malloc from seeing 0 for a message
     * whose fields are all default. */
    size_t encoded_size = 0U;
    if (!pb_get_encoded_size(&encoded_size, fields, msg))
    {
        return;
    }

    uint8_t *encoded = (uint8_t *)malloc(encoded_size + 1U);
    if (encoded == NULL)
    {
        return;
    }

    pb_ostream_t os = pb_ostream_from_buffer(encoded, encoded_size);
    if (!pb_encode(&os, fields, msg))
    {
        free(encoded);
        return;
    }

    taz_response_send_encoded(write_fn, ctx, stream_id, opcode, encoded,
                              os.bytes_written);
    free(encoded);
}
