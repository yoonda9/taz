/* libFuzzer harness for the frame reassembly state machine + dispatch layer.
 * Feeds arbitrary bytes to taz_reassembly_feed and verifies no crash or
 * sanitizer error occurs. */

#include <stddef.h>
#include <stdint.h>

#include "taz/connection.h"
#include "taz/dispatch.h"
#include "taz/frame.h"
#include "taz/reassembly.h"

static void noop_write(const uint8_t *data, size_t len, void *ctx)
{
    (void)data;
    (void)len;
    (void)ctx;
}

typedef struct
{
    taz_dispatch_t dispatch;
} fuzz_ctx_t;

static void on_frame(const taz_frame_header_t *header, const uint8_t *payload,
                     taz_frame_verdict_t verdict, void *ctx)
{
    fuzz_ctx_t *fctx = (fuzz_ctx_t *)ctx;
    int close_out = 0;
    taz_conn_handle_frame(&fctx->dispatch, header, payload, verdict, noop_write,
                          NULL, &close_out);
    (void)close_out;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    taz_reassembly_state_t state;
    fuzz_ctx_t fctx;

    taz_reassembly_init(&state);
    taz_dispatch_init(&fctx.dispatch);

    taz_reassembly_feed(&state, data, size, on_frame, &fctx);
    return 0;
}
