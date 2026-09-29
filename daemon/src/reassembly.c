#include "taz/reassembly.h"

#include <string.h>

void taz_reassembly_init(taz_reassembly_state_t *state)
{
    state->phase = TAZ_REASSEMBLY_PHASE_HEADER;
    state->cursor = 0U;
}

void taz_reassembly_feed(taz_reassembly_state_t *state, const uint8_t *data,
                         size_t len, taz_reassembly_on_frame_t on_frame,
                         void *ctx)
{
    size_t pos = 0U;

    while (pos < len)
    {
        switch (state->phase)
        {
            case TAZ_REASSEMBLY_PHASE_HEADER:
            {
                size_t need =
                    (size_t)TAZ_FRAME_HEADER_SIZE - (size_t)state->cursor;
                size_t take = ((len - pos) < need) ? (len - pos) : need;
                (void)memcpy(&state->header_buf[state->cursor], &data[pos],
                             take);
                state->cursor += (uint32_t)take;
                pos += take;

                if (state->cursor == (uint32_t)TAZ_FRAME_HEADER_SIZE)
                {
                    taz_frame_unpack_header(state->header_buf, &state->header);
                    state->cursor = 0U;

                    taz_frame_verdict_t verdict =
                        taz_frame_validate_header(&state->header);
                    if (verdict == TAZ_FRAME_OVERSIZED)
                    {
                        state->phase = TAZ_REASSEMBLY_PHASE_DONE;
                        on_frame(&state->header, NULL, TAZ_FRAME_OVERSIZED,
                                 ctx);
                        return;
                    }
                    if (verdict == TAZ_FRAME_UNKNOWN_TYPE)
                    {
                        if (state->header.length == 0U)
                        {
                            on_frame(&state->header, NULL,
                                     TAZ_FRAME_UNKNOWN_TYPE, ctx);
                        }
                        else
                        {
                            state->phase = TAZ_REASSEMBLY_PHASE_SKIP;
                        }
                    }
                    else /* TAZ_FRAME_OK */
                    {
                        if (state->header.length == 0U)
                        {
                            on_frame(&state->header, state->payload,
                                     TAZ_FRAME_OK, ctx);
                        }
                        else
                        {
                            state->phase = TAZ_REASSEMBLY_PHASE_PAYLOAD;
                        }
                    }
                }
                break;
            }

            case TAZ_REASSEMBLY_PHASE_PAYLOAD:
            {
                size_t need =
                    (size_t)state->header.length - (size_t)state->cursor;
                size_t take = ((len - pos) < need) ? (len - pos) : need;
                (void)memcpy(&state->payload[state->cursor], &data[pos], take);
                state->cursor += (uint32_t)take;
                pos += take;

                if (state->cursor == state->header.length)
                {
                    state->phase = TAZ_REASSEMBLY_PHASE_HEADER;
                    state->cursor = 0U;
                    on_frame(&state->header, state->payload, TAZ_FRAME_OK, ctx);
                }
                break;
            }

            case TAZ_REASSEMBLY_PHASE_SKIP:
            {
                size_t need =
                    (size_t)state->header.length - (size_t)state->cursor;
                size_t take = ((len - pos) < need) ? (len - pos) : need;
                state->cursor += (uint32_t)take;
                pos += take;

                if (state->cursor == state->header.length)
                {
                    state->phase = TAZ_REASSEMBLY_PHASE_HEADER;
                    state->cursor = 0U;
                    on_frame(&state->header, NULL, TAZ_FRAME_UNKNOWN_TYPE, ctx);
                }
                break;
            }

            case TAZ_REASSEMBLY_PHASE_DONE:
                return;
        }
    }
}
