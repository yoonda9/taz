#ifndef TAZ_FRAME_H
#define TAZ_FRAME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define TAZ_FRAME_HEADER_SIZE 12

/* Largest per-type limit; used as the bound for unknown types (§10.1). */
#define TAZ_FRAME_MAX_PAYLOAD 65536U

/* Per-type default payload limits (protocol §6). frame.c asserts that none
 * exceeds TAZ_FRAME_MAX_PAYLOAD. */
#define TAZ_FRAME_MAX_PAYLOAD_PING       0U
#define TAZ_FRAME_MAX_PAYLOAD_PONG       0U
#define TAZ_FRAME_MAX_PAYLOAD_CAPABILITY 1024U
#define TAZ_FRAME_MAX_PAYLOAD_ERROR      4096U
#define TAZ_FRAME_MAX_PAYLOAD_REQUEST    TAZ_FRAME_MAX_PAYLOAD
#define TAZ_FRAME_MAX_PAYLOAD_RESPONSE   TAZ_FRAME_MAX_PAYLOAD
#define TAZ_FRAME_MAX_PAYLOAD_FILE_CHUNK TAZ_FRAME_MAX_PAYLOAD

    typedef struct
    {
        uint8_t type;
        uint8_t flags;
        uint16_t opcode;
        uint32_t length;
        uint32_t stream_id;
    } taz_frame_header_t;

    typedef enum
    {
        TAZ_FRAME_OK,
        TAZ_FRAME_UNKNOWN_TYPE,
        TAZ_FRAME_OVERSIZED
    } taz_frame_verdict_t;

    void taz_frame_pack_header(const taz_frame_header_t *header,
                               uint8_t buf[TAZ_FRAME_HEADER_SIZE]);

    void taz_frame_unpack_header(const uint8_t buf[TAZ_FRAME_HEADER_SIZE],
                                 taz_frame_header_t *header);

    uint32_t taz_frame_max_payload(uint8_t type);

    taz_frame_verdict_t
    taz_frame_validate_header(const taz_frame_header_t *header);

#ifdef __cplusplus
}
#endif

#endif /* TAZ_FRAME_H */
