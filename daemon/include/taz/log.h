#ifndef TAZ_LOG_H
#define TAZ_LOG_H

#include <stddef.h>
#include <stdint.h>

#include "taz/v1/advanced.pb.h"

/* Capacity of the in-memory ring buffer backing taz_log_collect. */
#define TAZ_LOG_RING_CAPACITY 4096U

/* Let the compilers check taz_log's format string against its arguments:
 * GCC/Clang through the format attribute, MSVC /analyze through SAL. */
#if defined(__GNUC__) || defined(__clang__)
#define TAZ_LOG_PRINTF(fmt_index, first_arg_index)                             \
    __attribute__((format(printf, fmt_index, first_arg_index)))
#else
#define TAZ_LOG_PRINTF(fmt_index, first_arg_index)
#endif
#ifdef _MSC_VER
#include <sal.h>
#define TAZ_LOG_FORMAT_STRING _Printf_format_string_
#else
#define TAZ_LOG_FORMAT_STRING
#endif

#ifdef __cplusplus
extern "C"
{
#endif

    typedef enum
    {
        TAZ_LOG_DEBUG = 0,
        TAZ_LOG_INFO = 1,
        TAZ_LOG_WARN = 2,
        TAZ_LOG_ERROR = 3
    } taz_log_level_t;

    /* Initialize the ring buffer and its mutex. Idempotent (a second call
     * before taz_log_shutdown is a no-op). Call once from main before
     * taz_server_listen; taz_log before this is a no-op. */
    void taz_log_init(void);

    /* Destroy the mutex. Call once at shutdown. */
    void taz_log_shutdown(void);

    /* Format fmt/args (bounded; never an unbounded %s into the ring),
     * sanitize the result to valid UTF-8 and cut it to LogEntry.message's
     * 512-byte limit at a codepoint boundary, stamp it with the current
     * time, and - gated by level >= the level set by taz_log_set_level -
     * record it into the ring and write it to stderr. The level is read
     * under the mutex before formatting, and the ring append and stderr
     * write then happen together under it, so concurrent callers from pool
     * threads and the loop thread never interleave or race. A call that
     * passed the gate just before a concurrent taz_log_set_level is still
     * recorded. */
    void taz_log(taz_log_level_t level, TAZ_LOG_FORMAT_STRING const char *fmt,
                 ...) TAZ_LOG_PRINTF(2, 3);

    /* Update the minimum level taz_log gates against. Takes the same mutex
     * as taz_log/taz_log_collect, so config.c can call this right after
     * validating and storing a new "log.level" value without racing a
     * concurrent taz_log call on another thread. A no-op before
     * taz_log_init. */
    void taz_log_set_level(taz_log_level_t level);

    /* Copy up to out_cap entries from the ring into out, oldest first.
     * Selects entries with timestamp > since and level >= min_level, then
     * keeps only the newest `lines` of those matches (0 means all).
     * Returns the number of entries written to out. */
    size_t taz_log_collect(uint32_t lines, uint64_t since,
                           taz_log_level_t min_level, taz_v1_LogEntry *out,
                           size_t out_cap);

    /* Parse a level token: "" -> DEBUG (returns 0); "DEBUG"/"INFO"/"WARN"/
     * "ERROR" (exact, case-sensitive) -> that level (returns 0); anything
     * else leaves *out unchanged and returns non-zero. */
    int taz_log_level_parse(const char *s, taz_log_level_t *out);

    /* Test-only: clear the ring back to empty without changing capacity. */
    void taz_log_reset_for_tests(void);

    /* Test-only: shrink the ring's effective capacity to n (clamped to
     * [1, TAZ_LOG_RING_CAPACITY]) so a wrap test is cheap, and clear it. */
    void taz_log_set_capacity_for_tests(size_t n);

#ifdef __cplusplus
}
#endif

#endif /* TAZ_LOG_H */
