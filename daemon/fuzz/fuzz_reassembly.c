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

/* The first two input bytes (big-endian) choose the read size, 1..65536; the
 * rest is fed in reads of that size, so frames split across reads are fuzzed
 * as well as coalesced ones. `just fuzz` raises libFuzzer's input limit so
 * payloads can reach the end of the 64 KiB reassembly buffer. */
#define READ_SIZE_BYTES 2U
#define BITS_PER_BYTE   8U

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    taz_reassembly_state_t state;
    fuzz_ctx_t fctx;
    size_t read_size;
    size_t pos;

    if (size < READ_SIZE_BYTES)
    {
        return 0;
    }
    read_size = (((size_t)data[0] << BITS_PER_BYTE) | (size_t)data[1]) + 1U;
    data += READ_SIZE_BYTES;
    size -= READ_SIZE_BYTES;

    taz_reassembly_init(&state);
    taz_dispatch_init(&fctx.dispatch);

    for (pos = 0U; pos < size; pos += read_size)
    {
        size_t n = ((size - pos) < read_size) ? (size - pos) : read_size;
        taz_reassembly_feed(&state, &data[pos], n, on_frame, &fctx);
    }
    return 0;
}
