#ifndef TAZ_PROCESS_H
#define TAZ_PROCESS_H

#include <stddef.h>
#include <stdint.h>

#include "taz/v1/common.pb.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /* One process, as reported by PROCESS_LIST / embedded in PROCESS_INFO.
     * Field widths match taz_v1_ProcessInfo exactly. */
    typedef struct taz_process_entry_s
    {
        uint32_t pid;
        char name[256];    /* sanitized UTF-8 */
        char user[64];     /* "" when unknown */
        float cpu_percent; /* lifetime average, one CPU == 100 */
        uint64_t memory_bytes;
        char state[32];
    } taz_process_entry_t;

    /* A growable, heap-owned list of entries, ascending by pid. */
    typedef struct taz_process_list_s
    {
        taz_process_entry_t *entries;
        size_t count;
        size_t capacity;
    } taz_process_list_t;

    /* A single process's full detail, as reported by PROCESS_INFO. */
    typedef struct taz_process_detail_s
    {
        taz_process_entry_t info;
        char *command_line;  /* heap, sanitized UTF-8, <= 4095 bytes, "" when
                                unknown */
        uint64_t start_time; /* unix seconds, floored */
        char (*open_files)[1024]; /* heap array of sanitized, truncated link
                                     targets */
        size_t open_files_count;
    } taz_process_detail_t;

    /* Pool thread. filter == "" lists everything; otherwise a plain
     * case-sensitive byte substring match on name. Fills out (count may be
     * zero) and returns 0, or returns -1 with code/detail set. Never fails
     * because a single process vanished or could not be read: such entries
     * are dropped from the list rather than failing the whole call. */
    int taz_process_enumerate(const char *filter, taz_process_list_t *out,
                              taz_v1_ErrorCode *code, const char **detail);

    /* Frees list->entries and zeroes *list. No-op on an already-zeroed
     * struct. */
    void taz_process_list_free(taz_process_list_t *list);

    /* Pool thread. Fills *out and returns 0, or returns -1 with *code set to
     * NOT_FOUND (no such process / already exited), PERMISSION_DENIED or
     * INTERNAL (with *detail). Partial information never fails the call:
     * an unreadable command line becomes "", an unreadable fd directory
     * becomes no open_files, an unknown user becomes "". */
    int taz_process_inspect(uint32_t pid, taz_process_detail_t *out,
                            taz_v1_ErrorCode *code, const char **detail);

    /* Frees detail->command_line and detail->open_files and zeroes *detail.
     * No-op on an already-zeroed struct. */
    void taz_process_detail_free(taz_process_detail_t *detail);

    /* Loop thread, inline (no pool work item). pid is already validated
     * (1..INT32_MAX); signal >= 0 (0 means SIGTERM / platform terminate).
     * Returns 0 on success, or -1 with code and detail set. */
    int taz_process_kill(uint32_t pid, int32_t signal, taz_v1_ErrorCode *code,
                         const char **detail);

#ifndef _WIN32

    /* The fields of /proc/<pid>/stat this code needs, parsed from a single
     * crafted or real stat line. comm holds the raw bytes between the
     * first '(' and the LAST ')' (not sanitized: comm can itself contain
     * ')' and spaces). */
    typedef struct taz_proc_stat_s
    {
        char comm[256];
        char state;
        uint64_t utime;     /* field 14, clock ticks */
        uint64_t stime;     /* field 15, clock ticks */
        uint64_t starttime; /* field 22, clock ticks since boot */
        uint64_t rss_pages; /* field 24 */
    } taz_proc_stat_t;

    /* Parses a /proc/<pid>/stat line (text, exactly len bytes, no NUL
     * reliance). Returns 1 and fills *out on success, 0 when text is
     * malformed (no '(' / no matching ')', fewer than 24 whitespace-
     * separated fields after the ')', or a non-numeric numeric field). */
    int taz_proc_parse_stat(const char *text, size_t len, taz_proc_stat_t *out);

    /* Parses a /proc/<pid>/status "Uid:\t<real>\t..." line out of the whole
     * status file text. Returns 1 and fills *real_uid on success, 0 when
     * there is no Uid: line or it has no digits. */
    int taz_proc_parse_status_uid(const char *text, size_t len,
                                  unsigned long *real_uid);

    /* Parses a /proc/stat "btime N" line out of the whole file text.
     * Returns 1 and fills *btime on success, 0 when there is no btime
     * line. */
    int taz_proc_parse_btime(const char *text, size_t len, uint64_t *btime);

    /* Maps a /proc/<pid>/stat state letter to its lowercase word (R
     * running, S sleeping, D disk-sleep, T stopped, t tracing-stop, Z
     * zombie, X/x dead, K wake-kill, W waking, P parked, I idle); any other
     * letter maps to "unknown". The returned pointer is a string literal,
     * valid for the life of the program. */
    const char *taz_proc_state_word(char letter);

    /* 100 * (cpu_ticks / ticks_per_sec) / elapsed_seconds: the lifetime CPU
     * percentage (one CPU == 100, may exceed 100 on a multi-threaded
     * process). Returns 0.0f when elapsed_seconds <= 0. */
    float taz_proc_cpu_percent(uint64_t cpu_ticks, uint64_t ticks_per_sec,
                               double elapsed_seconds);

    /* Joins a /proc/<pid>/cmdline byte buffer (NUL-separated arguments,
     * len bytes total) into a single human-readable string: each NUL
     * becomes one space, a trailing NUL is dropped (no trailing space),
     * then the result is sanitized to valid UTF-8 and truncated to fit
     * outsize (taz_fsutil_sanitize_utf8). No-op when outsize == 0. */
    void taz_proc_cmdline_to_string(const uint8_t *buf, size_t len, char *out,
                                    size_t outsize);

#endif /* !_WIN32 */

#ifdef __cplusplus
}
#endif

#endif /* TAZ_PROCESS_H */
