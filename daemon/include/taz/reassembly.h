#ifndef TAZ_REASSEMBLY_H
#define TAZ_REASSEMBLY_H

#include <stddef.h>
#include <stdint.h>

#include "taz/frame.h"

#ifdef __cplusplus
extern "C"
{
#endif

    typedef enum
    {
        TAZ_REASSEMBLY_PHASE_HEADER,  /* accumulating the 12-byte header */
        TAZ_REASSEMBLY_PHASE_PAYLOAD, /* accumulating payload for a valid frame
                                       */
        TAZ_REASSEMBLY_PHASE_SKIP,    /* discarding payload for an unknown-type
                                         frame */
        TAZ_REASSEMBLY_PHASE_DONE,    /* terminal: oversized frame; no further
                                         processing */
    } taz_reassembly_phase_t;

    typedef struct
    {
        taz_reassembly_phase_t phase;
        uint32_t cursor;           /* bytes accumulated in the current phase */
        taz_frame_header_t header; /* valid once HEADER phase completes */
        uint8_t header_buf[TAZ_FRAME_HEADER_SIZE];
        uint8_t payload[TAZ_FRAME_MAX_PAYLOAD]; /* 64 KiB static buffer */
    } taz_reassembly_state_t;

    /*
     * Called by taz_reassembly_feed for each completed event:
     *
     *   TAZ_FRAME_OK           — complete frame assembled; payload/length valid
     *   TAZ_FRAME_UNKNOWN_TYPE — payload discarded; payload is NULL; caller
     *                            sends NOT_SUPPORTED and continues
     *   TAZ_FRAME_OVERSIZED    — oversized header detected; payload is NULL;
     *                            caller sends PROTOCOL_ERROR and closes
     */
    typedef void (*taz_reassembly_on_frame_t)(const taz_frame_header_t *header,
                                              const uint8_t *payload,
                                              taz_frame_verdict_t verdict,
                                              void *ctx);

    /* Initialise a reassembly state to the empty HEADER phase. */
    void taz_reassembly_init(taz_reassembly_state_t *state);

    /*
     * Feed raw bytes into the state machine.  Calls on_frame once per
     * completed event.  A single call may deliver multiple frames when reads
     * are coalesced.  After an OVERSIZED event the state enters DONE and
     * further calls are no-ops.
     */
    void taz_reassembly_feed(taz_reassembly_state_t *state, const uint8_t *data,
                             size_t len, taz_reassembly_on_frame_t on_frame,
                             void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* TAZ_REASSEMBLY_H */
