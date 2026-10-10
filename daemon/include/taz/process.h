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

    /* Per-stream watch on one pid. Opened and closed on the loop thread;
     * sampled on the pool. Fields are plain so the handler can read
     * exit_fd without any #ifdef. */
    typedef struct taz_process_watch_s
    {
        uint32_t pid;
        int exit_fd;  /* Linux: a pidfd (pollable, readable once the
                         process exits); -1 on Windows, when pidfd is
                         unavailable (ENOSYS/EPERM), or when use_pidfd ==
                         0 */
        void *handle; /* Windows: the HANDLE held for the stream's life;
                         NULL on POSIX */
        uint64_t first_starttime; /* Linux: field 22 of the first
                                     successful sample (0 = none yet);
                                     later samples with a different value
                                     report exited (pid reuse) */
    } taz_process_watch_t;

    typedef enum
    {
        TAZ_PROCESS_SAMPLE_LIVE = 0,
        TAZ_PROCESS_SAMPLE_EXITED = 1 /* Z/X/x, ENOENT/ESRCH, starttime
                                         changed, WaitForSingleObject(0) ==
                                         WAIT_OBJECT_0 */
    } taz_process_sample_state_t;

    typedef struct taz_process_sample_s
    {
        taz_process_entry_t info; /* name/user/state/memory as
                                     PROCESS_INFO fills them; cpu_percent
                                     is left 0 - the handler computes the
                                     interval value */
        uint64_t cpu_time_ns;     /* cumulative user+system CPU time
                                     (Linux: (utime+stime) ticks scaled by
                                     1e9/CLK_TCK; Windows: 100-ns units x
                                     100) */
        uint64_t starttime;       /* Linux field 22 ticks; Windows
                                     creation FILETIME (reuse check) */
        taz_process_sample_state_t state;
        int32_t exit_code;   /* Windows: GetExitCodeProcess when EXITED;
                               else 0 */
        int exit_code_known; /* Windows: 1 when GetExitCodeProcess
                               succeeded; POSIX: always 0 */
    } taz_process_sample_t;

    /* Loop thread, inline (no blocking I/O). pid already validated
     * (1..INT32_MAX). Linux: use_pidfd != 0 -> syscall(SYS_pidfd_open);
     * ESRCH -> -1 NOT_FOUND; ENOSYS/EPERM (or any other failure) -> 0
     * with exit_fd == -1 (fallback); use_pidfd == 0 -> 0 with exit_fd -1
     * and no syscall. Windows (use_pidfd ignored):
     * OpenProcess(QUERY_LIMITED | SYNCHRONIZE): INVALID_PARAMETER ->
     * NOT_FOUND, ACCESS_DENIED -> PERMISSION_DENIED, other -> mapped;
     * GetProcessId(h) != pid -> NOT_FOUND; WaitForSingleObject(h, 0)
     * signalled -> NOT_FOUND "process has exited" (handle closed); else 0
     * with handle held. */
    int taz_process_watch_open(uint32_t pid, int use_pidfd,
                               taz_process_watch_t *w, taz_v1_ErrorCode *code,
                               const char **detail);

    /* Pool thread. Reads only *w (never written by the loop while a
     * sample is in flight) and fills *out. Returns 0 (state LIVE or
     * EXITED) or -1 with *code and *detail set: Linux ENOENT/ESRCH on
     * /proc/<pid>/stat -> NOT_FOUND (the handler decides: first sample ->
     * ERROR, later -> final exited), EACCES -> PERMISSION_DENIED,
     * malformed -> INTERNAL "unparseable /proc stat"; Windows never fails
     * once the handle is held (query failures leave fields blank).
     * Linux: state Z/X/x -> EXITED; w->first_starttime != 0 && !=
     * starttime -> EXITED. Windows: signalled -> EXITED +
     * GetExitCodeProcess. Partial info never fails. */
    int taz_process_watch_sample(const taz_process_watch_t *w,
                                 taz_process_sample_t *out,
                                 taz_v1_ErrorCode *code, const char **detail);

    /* Loop thread, after the poll handle (if any) is closed:
     * close(exit_fd) / CloseHandle; zeroes *w (exit_fd = -1). No-op on a
     * zeroed/already-closed watch. */
    void taz_process_watch_close(taz_process_watch_t *w);

    /* 100 * cpu_delta_ns / wall_delta_ns; 0.0f when wall_delta_ns == 0.
     * May exceed 100. */
    float taz_process_interval_cpu_percent(uint64_t cpu_delta_ns,
                                           uint64_t wall_delta_ns);

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

    /* 1 when st reports Z/X/x, or first_starttime != 0 &&
     * st->starttime != first_starttime. */
    int taz_proc_stat_is_exited(const taz_proc_stat_t *st,
                                uint64_t first_starttime);

    /* ticks * (1e9 / ticks_per_sec) without overflow for realistic
     * inputs; ticks_per_sec <= 0 -> 100. */
    uint64_t taz_proc_ticks_to_ns(uint64_t ticks, uint64_t ticks_per_sec);

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
