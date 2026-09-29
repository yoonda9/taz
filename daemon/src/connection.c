#include "taz/connection.h"

#include <stdlib.h>
#include <string.h>

#include "taz/dispatch.h"
#include "taz/error.h"
#include "taz/frame.h"
#include "taz/payload.h"
#include "taz/reassembly.h"
#include "taz/v1/common.pb.h"

/* Heap-allocated write request; owns the frame data.  The uv_write_t must be
 * the first member so the write-done callback can cast back. */
typedef struct
{
    uv_write_t req; /* must be first */
    taz_conn_t *conn;
    /* Frame bytes follow immediately after the struct. */
} write_req_t;

static void on_write_done(uv_write_t *req, int status);
static void on_close_cb(uv_handle_t *handle);

/* -------------------------------------------------------------------------
 * Reference counting
 * ------------------------------------------------------------------------- */

static void conn_unref(taz_conn_t *conn)
{
    conn->refcount--;
    if (conn->refcount == 0U && conn->closing)
    {
        free(conn);
    }
}

static void on_write_done(uv_write_t *req, int status)
{
    write_req_t *wr = (write_req_t *)req;
    taz_conn_t *conn = wr->conn;
    (void)status; /* UV_ECANCELED is expected on a closing connection */
    free(wr);
    conn_unref(conn);
}

static void on_close_cb(uv_handle_t *handle)
{
    taz_conn_t *conn = (taz_conn_t *)handle->data;
    conn_unref(conn);
}

/* Initiate teardown: stop reads and close the TCP handle.  Must be called
 * from the libuv loop thread and is idempotent. */
static void conn_close(taz_conn_t *conn)
{
    if (conn->closing)
    {
        return;
    }
    conn->closing = 1;
    uv_read_stop((uv_stream_t *)&conn->handle);
    uv_close((uv_handle_t *)&conn->handle, on_close_cb);
}

/* -------------------------------------------------------------------------
 * Write primitive
 * ------------------------------------------------------------------------- */

static void conn_write_fn(const uint8_t *data, size_t len, void *ctx)
{
    taz_conn_t *conn = (taz_conn_t *)ctx;
    write_req_t *wr;
    uint8_t *frame_data;
    uv_buf_t buf;
    int rc;

    if (conn->closing || len == 0U)
    {
        return;
    }

    /* Allocate the request header plus the frame bytes in one block. */
    wr = (write_req_t *)malloc(sizeof(write_req_t) + len);
    if (wr == NULL)
    {
        return;
    }
    wr->conn = conn;
    frame_data = (uint8_t *)(wr + 1);
    (void)memcpy(frame_data, data, len);

    buf = uv_buf_init((char *)frame_data, (unsigned int)len);
    conn->refcount++;
    rc = uv_write(&wr->req, (uv_stream_t *)&conn->handle, &buf, 1U,
                  on_write_done);
    if (rc != 0)
    {
        conn->refcount--;
        free(wr);
    }
}

/* -------------------------------------------------------------------------
 * taz_conn_handle_frame — testable frame routing
 * ------------------------------------------------------------------------- */

void taz_conn_handle_frame(taz_dispatch_t *d, const taz_frame_header_t *header,
                           const uint8_t *payload, taz_frame_verdict_t verdict,
                           taz_dispatch_write_fn_t write_fn, void *ctx,
                           int *close_out)
{
    *close_out = 0;

    if (verdict == TAZ_FRAME_OVERSIZED)
    {
        /* §10.1a: send ERROR PROTOCOL_ERROR then close. */
        uint8_t err_payload[TAZ_FRAME_MAX_PAYLOAD_ERROR];
        uint8_t frame_buf[TAZ_FRAME_HEADER_SIZE + TAZ_FRAME_MAX_PAYLOAD_ERROR];
        taz_frame_header_t h;
        size_t err_len =
            taz_error_encode(err_payload, sizeof(err_payload),
                             taz_v1_ErrorCode_ERROR_CODE_PROTOCOL_ERROR,
                             "oversized frame", NULL);
        if (err_len > 0U)
        {
            h.type = (uint8_t)taz_v1_FrameType_FRAME_TYPE_ERROR;
            h.flags = 0U;
            h.opcode = header->opcode;
            h.length = (uint32_t)err_len;
            h.stream_id = header->stream_id;
            taz_frame_pack_header(&h, frame_buf);
            (void)memcpy(frame_buf + TAZ_FRAME_HEADER_SIZE, err_payload,
                         err_len);
            write_fn(frame_buf, (size_t)TAZ_FRAME_HEADER_SIZE + err_len, ctx);
        }
        *close_out = 1;
        return;
    }

    /* TAZ_FRAME_OK or TAZ_FRAME_UNKNOWN_TYPE: dispatch handles both. */
    taz_dispatch_frame(d, header, payload, verdict, write_fn, ctx);
}

/* -------------------------------------------------------------------------
 * on_frame_cb — reassembly callback, called by on_read
 * ------------------------------------------------------------------------- */

static void on_frame_cb(const taz_frame_header_t *header,
                        const uint8_t *payload, taz_frame_verdict_t verdict,
                        void *ctx)
{
    taz_conn_t *conn = (taz_conn_t *)ctx;
    int should_close = 0;

    taz_conn_handle_frame(&conn->dispatch, header, payload, verdict,
                          conn_write_fn, conn, &should_close);
    if (should_close)
    {
        conn_close(conn);
    }
}

/* -------------------------------------------------------------------------
 * libuv read callbacks
 * ------------------------------------------------------------------------- */

static void alloc_cb(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf)
{
    taz_conn_t *conn = (taz_conn_t *)handle->data;
    (void)suggested_size;
    *buf = uv_buf_init((char *)conn->read_buf,
                       (unsigned int)sizeof(conn->read_buf));
}

static void on_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf)
{
    taz_conn_t *conn = (taz_conn_t *)stream->data;
    (void)buf;

    if (nread < 0)
    {
        conn_close(conn);
        return;
    }
    if (nread == 0)
    {
        return;
    }

    taz_reassembly_feed(&conn->reassembly, (const uint8_t *)buf->base,
                        (size_t)nread, on_frame_cb, conn);
}

/* -------------------------------------------------------------------------
 * CAPABILITY frame helper
 * ------------------------------------------------------------------------- */

static void send_capability(taz_conn_t *conn)
{
    uint8_t payload[TAZ_FRAME_MAX_PAYLOAD_CAPABILITY];
    uint8_t frame[TAZ_FRAME_HEADER_SIZE + TAZ_FRAME_MAX_PAYLOAD_CAPABILITY];
    taz_frame_header_t h;
    size_t pay_len;

    pay_len = taz_payload_capability(payload, sizeof(payload));
    if (pay_len == 0U)
    {
        return;
    }

    h.type = (uint8_t)taz_v1_FrameType_FRAME_TYPE_CAPABILITY;
    h.flags = 0U;
    h.opcode = 0U;
    h.length = (uint32_t)pay_len;
    h.stream_id = 0U;
    taz_frame_pack_header(&h, frame);
    (void)memcpy(frame + TAZ_FRAME_HEADER_SIZE, payload, pay_len);

    conn_write_fn(frame, (size_t)TAZ_FRAME_HEADER_SIZE + pay_len, conn);
}

/* -------------------------------------------------------------------------
 * taz_conn_on_new_connection
 * ------------------------------------------------------------------------- */

void taz_conn_on_new_connection(uv_stream_t *server, int status)
{
    taz_conn_t *conn;
    int rc;

    if (status < 0)
    {
        return;
    }

    conn = (taz_conn_t *)malloc(sizeof(taz_conn_t));
    if (conn == NULL)
    {
        return;
    }

    (void)memset(conn, 0, sizeof(*conn));
    conn->refcount = 1U;
    taz_reassembly_init(&conn->reassembly);
    taz_dispatch_init(&conn->dispatch);

    rc = uv_tcp_init(uv_handle_get_loop((uv_handle_t *)server), &conn->handle);
    if (rc != 0)
    {
        free(conn);
        return;
    }
    conn->handle.data = conn;

    rc = uv_accept(server, (uv_stream_t *)&conn->handle);
    if (rc != 0)
    {
        conn_close(conn);
        return;
    }

    send_capability(conn);

    rc = uv_read_start((uv_stream_t *)&conn->handle, alloc_cb, on_read);
    if (rc != 0)
    {
        conn_close(conn);
        return;
    }
}
