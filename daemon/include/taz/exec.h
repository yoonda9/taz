#ifndef TAZ_EXEC_H
#define TAZ_EXEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <uv.h>

#include "taz/v1/common.pb.h"

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

    /* Opaque, heap-allocated handle for one spawned process. Owned entirely
     * by exec.c; callers only ever see a pointer to it. */
    typedef struct taz_exec_s taz_exec_t;

    /* Describes a process to spawn. The engine copies everything it needs
     * out of *spec before taz_exec_start returns, so the pointers inside it
     * (including file/args/env/cwd strings) only need to stay valid for the
     * duration of that call. */
    typedef struct
    {
        const char *file;        /* command to run */
        const char *const *args; /* arguments, WITHOUT argv[0] */
        size_t args_count;
        const taz_v1_KeyValue *env; /* extra variables, merged over the
                                     * daemon's own environment */
        size_t env_count;
        const char *cwd;     /* NULL/"" = daemon's cwd */
        uint32_t timeout_ms; /* 0 = none */
        size_t max_output_bytes;
    } taz_exec_spec_t;

    /* Outcome of a finished exec. out/err point into buffers owned by the
     * taz_exec_t and are valid only for the duration of the taz_exec_done_fn
     * call they are passed to. */
    typedef struct
    {
        int64_t exit_status;
        int term_signal;
        const uint8_t *out;
        size_t out_len;
        const uint8_t *err;
        size_t err_len;
        bool timed_out;
        bool truncated;
        bool cancelled;
    } taz_exec_result_t;

    typedef void (*taz_exec_done_fn)(const taz_exec_result_t *result,
                                     void *arg);

    /* Start *spec on loop. Returns 0 on success, in which case *out receives
     * the new handle and on_done will fire exactly once, later, from the
     * loop. Returns a UV_E* code on spawn failure, in which case on_done
     * never fires, *out is untouched, and nothing is leaked (any partially
     * initialized handles are closed internally). */
    int taz_exec_start(uv_loop_t *loop, const taz_exec_spec_t *spec,
                       taz_exec_done_fn on_done, void *arg, taz_exec_t **out);

    /* Kill x's whole process tree right away and mark the eventual result
     * cancelled. on_done still fires exactly once, later, once the process
     * and its pipes have finished draining - same as a natural exit or a
     * timeout. A no-op if on_done has already fired. */
    void taz_exec_cancel(taz_exec_t *x);

#ifdef __cplusplus
}
#endif

#endif /* TAZ_EXEC_H */
