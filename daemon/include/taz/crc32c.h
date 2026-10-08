#ifndef TAZ_CRC32C_H
#define TAZ_CRC32C_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /* Continues a CRC32C (Castagnoli) computation. crc is the finished CRC
     * of the bytes processed so far (0 for none):
     * taz_crc32c_update(taz_crc32c_update(0, a, na), b, nb) ==
     * taz_crc32c(a followed by b, na + nb). Dispatches to the hardware path
     * when available, else the software table. */
    uint32_t taz_crc32c_update(uint32_t crc, const void *data, size_t len);

    /* taz_crc32c(data, len) == taz_crc32c_update(0, data, len). */
    uint32_t taz_crc32c(const void *data, size_t len);

    /* Writes the finished CRC to out[4], little-endian. */
    void taz_crc32c_to_le(uint32_t crc, uint8_t out[4]);

    /* 256-entry table-driven path; always available on every platform. */
    uint32_t taz_crc32c_software(uint32_t crc, const void *data, size_t len);

    /* Hardware-accelerated path (x86-64 SSE4.2, AArch64 +crc). Only valid
     * to call when taz_crc32c_hardware_available() is non-zero. */
    uint32_t taz_crc32c_hardware(uint32_t crc, const void *data, size_t len);

    /* Cached runtime detection of a hardware CRC32C instruction. 0 on any
     * platform/arch without one. */
    int taz_crc32c_hardware_available(void);

#ifdef __cplusplus
}
#endif

#endif /* TAZ_CRC32C_H */
