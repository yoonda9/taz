#ifndef TAZ_EXEC_H
#define TAZ_EXEC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /* Which stream a captured chunk came from. */
    typedef enum
    {
        TAZ_EXEC_STREAM_OUT = 0,
        TAZ_EXEC_STREAM_ERR = 1
    } taz_exec_stream_t;

    /* Growable pair of output buffers (OUT and ERR) sharing a single
     * combined byte cap. Zero-initialized memory is not a valid capture;
     * always call taz_exec_capture_init first. */
    typedef struct
    {
        uint8_t *out;
        size_t out_len;
        size_t out_cap;
        uint8_t *err;
        size_t err_len;
        size_t err_cap;
        size_t max_bytes;
        int truncated; /* 1 once the combined cap has been hit */
    } taz_exec_capture_t;

    /* Initialize *cap to empty OUT/ERR buffers with a combined byte cap of
     * max_bytes. */
    void taz_exec_capture_init(taz_exec_capture_t *cap, size_t max_bytes);

    /* Append len bytes from data to the OUT or ERR buffer of *cap, subject
     * to the combined max_bytes cap shared by both streams. If the append
     * would cross the cap it is applied only up to the cap, cap->truncated
     * is set, and all later appends (to either stream) become no-ops so
     * the caller can keep draining the source without growing the buffers
     * further. Allocation failure also sets truncated and drops the data
     * rather than crashing. A zero-length append is always a no-op. */
    void taz_exec_capture_append(taz_exec_capture_t *cap,
                                 taz_exec_stream_t which, const void *data,
                                 size_t len);

    /* Free the buffers owned by *cap. Safe to call on a capture that was
     * initialized but never appended to. */
    void taz_exec_capture_free(taz_exec_capture_t *cap);

#ifdef __cplusplus
}
#endif

#endif /* TAZ_EXEC_H */
