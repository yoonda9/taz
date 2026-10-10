#include "taz/process.h"

#ifndef _WIN32

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>
#include <uv.h>

#include "taz/error.h"
#include "taz/fsutil.h"

/* The glibc pidfd_open() wrapper needs glibc >= 2.36, and musl (used by
 * the static/portable build target) may have none at all, so this calls
 * the syscall directly. The number is the same across every architecture. */
#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434
#endif

/* sysconf(_SC_CLK_TCK)/_SC_PAGESIZE fallbacks for the (never observed on a
 * real Linux host, but contractually possible) case where sysconf reports
 * <= 0. Declared here (rather than next to their one other use site below)
 * so the pure helpers just below can use them too. */
#define TAZ_PROC_DEFAULT_CLK_TCK   100U
#define TAZ_PROC_DEFAULT_PAGE_SIZE 4096U

/* /proc/<pid>/stat field numbers (1-based, counting the leading pid and the
 * parenthesized comm as fields 1 and 2) this code reads. Everything between
 * state and rss that is not one of these is skipped unparsed. */
#define TAZ_PROC_STAT_STATE_FIELD     3U
#define TAZ_PROC_STAT_UTIME_FIELD     14U
#define TAZ_PROC_STAT_STIME_FIELD     15U
#define TAZ_PROC_STAT_STARTTIME_FIELD 22U
#define TAZ_PROC_STAT_RSS_FIELD       24U

/* Long enough for any uint64_t in decimal (20 digits) plus the NUL. */
#define TAZ_PROC_NUMBER_BUF_LEN 32U
#define TAZ_PROC_DECIMAL_BASE   10U

#define TAZ_PROC_UID_PREFIX   "Uid:"
#define TAZ_PROC_BTIME_PREFIX "btime"

/* Large enough for the command_line field (4096 bytes) plus a little slack;
 * taz_fsutil_sanitize_utf8 never needs more source bytes than the output
 * capacity it is filling (a replacement character always encodes to at
 * least as many output bytes as the input bytes it consumes), so this
 * covers any realistic outsize. */
#define TAZ_PROC_CMDLINE_SCRATCH_LEN 4104U

typedef struct
{
    const char *ptr;
    size_t len;
} taz_proc_token_t;

/* First occurrence of c in text[0, len); (size_t)-1 when absent. */
static size_t proc_find_char(const char *text, size_t len, char c)
{
    size_t i;

    for (i = 0U; i < len; i++)
    {
        if (text[i] == c)
        {
            return i;
        }
    }
    return (size_t)-1;
}

/* Last occurrence of c in text[0, len); (size_t)-1 when absent. */
static size_t proc_find_last_char(const char *text, size_t len, char c)
{
    size_t i;

    for (i = len; i > 0U; i--)
    {
        if (text[i - 1U] == c)
        {
            return i - 1U;
        }
    }
    return (size_t)-1;
}

/* Offset of the first line in text[0, len) that starts with
 * prefix[0, prefix_len); (size_t)-1 when none does. Only line starts count:
 * /proc/<pid>/status puts the process-controlled comm on its Name: line, so
 * a "Uid:" inside it must not match. */
static size_t proc_find_line_prefix(const char *text, size_t len,
                                    const char *prefix, size_t prefix_len)
{
    size_t i;

    if ((prefix_len == 0U) || (prefix_len > len))
    {
        return (size_t)-1;
    }
    for (i = 0U; (i + prefix_len) <= len; i++)
    {
        if (((i == 0U) || (text[i - 1U] == '\n')) &&
            (memcmp(text + i, prefix, prefix_len) == 0))
        {
            return i;
        }
    }
    return (size_t)-1;
}

/* Advances *pos past the next run of non-space bytes in text[0, len),
 * skipping leading spaces first. Returns 0 (and leaves *pos unchanged)
 * when no token remains. */
static int proc_next_token(const char *text, size_t len, size_t *pos,
                           taz_proc_token_t *tok)
{
    size_t i = *pos;
    size_t start;

    while ((i < len) && (text[i] == ' '))
    {
        i++;
    }
    if (i >= len)
    {
        return 0;
    }
    start = i;
    while ((i < len) && (text[i] != ' '))
    {
        i++;
    }
    tok->ptr = text + start;
    tok->len = i - start;
    *pos = i;
    return 1;
}

/* Parses tok as an unsigned decimal uint64_t with no leading sign, no
 * leading/trailing junk, and no overflow (strtoull with endptr/errno,
 * cert-err34-c). Returns 0 when malformed. */
static int proc_parse_u64_token(const char *tok, size_t tok_len,
                                uint64_t *out_val)
{
    char buf[TAZ_PROC_NUMBER_BUF_LEN];
    char *endptr;
    unsigned long long value;

    if ((tok_len == 0U) || (tok_len >= sizeof(buf)))
    {
        return 0;
    }
    if ((tok[0] < '0') || (tok[0] > '9'))
    {
        /* Rejects '-'/'+' and anything else strtoull would otherwise
         * accept (a negative number would wrap around to a huge
         * unsigned value instead of failing). */
        return 0;
    }
    (void)memcpy(buf, tok, tok_len);
    buf[tok_len] = '\0';

    errno = 0;
    value = strtoull(buf, &endptr, TAZ_PROC_DECIMAL_BASE);
    if ((endptr != (buf + tok_len)) || (errno == ERANGE))
    {
        return 0;
    }
    *out_val = (uint64_t)value;
    return 1;
}

/* Parses a decimal run starting right after a prefix already located at
 * text + prefix_end, skipping leading spaces/tabs first. Returns 0 when
 * there is no digit. */
static int proc_parse_decimal_after(const char *text, size_t len,
                                    size_t prefix_end,
                                    unsigned long long *out_val)
{
    size_t i = prefix_end;
    size_t digit_start;
    size_t digit_len;
    char buf[TAZ_PROC_NUMBER_BUF_LEN];
    char *endptr;

    while ((i < len) && ((text[i] == ' ') || (text[i] == '\t')))
    {
        i++;
    }
    digit_start = i;
    while ((i < len) && (text[i] >= '0') && (text[i] <= '9'))
    {
        i++;
    }
    digit_len = i - digit_start;
    if ((digit_len == 0U) || (digit_len >= sizeof(buf)))
    {
        return 0;
    }
    (void)memcpy(buf, text + digit_start, digit_len);
    buf[digit_len] = '\0';

    errno = 0;
    *out_val = strtoull(buf, &endptr, TAZ_PROC_DECIMAL_BASE);
    if ((endptr != (buf + digit_len)) || (errno == ERANGE))
    {
        return 0;
    }
    return 1;
}

int taz_proc_parse_stat(const char *text, size_t len, taz_proc_stat_t *out)
{
    size_t open_idx;
    size_t close_idx;
    size_t comm_len;
    size_t pos;
    unsigned field;

    memset(out, 0, sizeof(*out));

    open_idx = proc_find_char(text, len, '(');
    close_idx = proc_find_last_char(text, len, ')');
    if ((open_idx == (size_t)-1) || (close_idx == (size_t)-1) ||
        (close_idx <= open_idx))
    {
        return 0;
    }

    comm_len = close_idx - open_idx - 1U;
    if (comm_len >= sizeof(out->comm))
    {
        comm_len = sizeof(out->comm) - 1U;
    }
    (void)memcpy(out->comm, text + open_idx + 1U, comm_len);
    out->comm[comm_len] = '\0';

    pos = close_idx + 1U;
    for (field = TAZ_PROC_STAT_STATE_FIELD; field <= TAZ_PROC_STAT_RSS_FIELD;
         field++)
    {
        taz_proc_token_t tok;

        if (!proc_next_token(text, len, &pos, &tok))
        {
            return 0;
        }
        switch (field)
        {
            case TAZ_PROC_STAT_STATE_FIELD:
                if (tok.len == 0U)
                {
                    return 0;
                }
                out->state = tok.ptr[0];
                break;
            case TAZ_PROC_STAT_UTIME_FIELD:
                if (!proc_parse_u64_token(tok.ptr, tok.len, &out->utime))
                {
                    return 0;
                }
                break;
            case TAZ_PROC_STAT_STIME_FIELD:
                if (!proc_parse_u64_token(tok.ptr, tok.len, &out->stime))
                {
                    return 0;
                }
                break;
            case TAZ_PROC_STAT_STARTTIME_FIELD:
                if (!proc_parse_u64_token(tok.ptr, tok.len, &out->starttime))
                {
                    return 0;
                }
                break;
            case TAZ_PROC_STAT_RSS_FIELD:
                if (!proc_parse_u64_token(tok.ptr, tok.len, &out->rss_pages))
                {
                    return 0;
                }
                break;
            default:
                /* Field not needed by this struct; just consumed above. */
                break;
        }
    }
    return 1;
}

int taz_proc_parse_status_uid(const char *text, size_t len,
                              unsigned long *real_uid)
{
    size_t pos = proc_find_line_prefix(text, len, TAZ_PROC_UID_PREFIX,
                                       strlen(TAZ_PROC_UID_PREFIX));
    unsigned long long value;

    if (pos == (size_t)-1)
    {
        return 0;
    }
    if (!proc_parse_decimal_after(text, len, pos + strlen(TAZ_PROC_UID_PREFIX),
                                  &value))
    {
        return 0;
    }
    *real_uid = (unsigned long)value;
    return 1;
}

int taz_proc_parse_btime(const char *text, size_t len, uint64_t *btime)
{
    size_t pos = proc_find_line_prefix(text, len, TAZ_PROC_BTIME_PREFIX,
                                       strlen(TAZ_PROC_BTIME_PREFIX));
    unsigned long long value;

    if (pos == (size_t)-1)
    {
        return 0;
    }
    if (!proc_parse_decimal_after(text, len,
                                  pos + strlen(TAZ_PROC_BTIME_PREFIX), &value))
    {
        return 0;
    }
    *btime = (uint64_t)value;
    return 1;
}

const char *taz_proc_state_word(char letter)
{
    switch (letter)
    {
        case 'R':
            return "running";
        case 'S':
            return "sleeping";
        case 'D':
            return "disk-sleep";
        case 'T':
            return "stopped";
        case 't':
            return "tracing-stop";
        case 'Z':
            return "zombie";
        case 'X':
        case 'x':
            return "dead";
        case 'K':
            return "wake-kill";
        case 'W':
            return "waking";
        case 'P':
            return "parked";
        case 'I':
            return "idle";
        default:
            return "unknown";
    }
}

/* Percentage scale (100%) for the lifetime-average CPU calculation. */
#define TAZ_PROC_CPU_PERCENT_SCALE 100.0

float taz_proc_cpu_percent(uint64_t cpu_ticks, uint64_t ticks_per_sec,
                           double elapsed_seconds)
{
    double ratio;

    if (elapsed_seconds <= 0.0)
    {
        return 0.0F;
    }
    ratio = TAZ_PROC_CPU_PERCENT_SCALE *
            ((double)cpu_ticks / (double)ticks_per_sec) / elapsed_seconds;
    return (float)ratio;
}

int taz_proc_stat_is_exited(const taz_proc_stat_t *st, uint64_t first_starttime)
{
    if ((st->state == 'Z') || (st->state == 'X') || (st->state == 'x'))
    {
        return 1;
    }
    if ((first_starttime != 0U) && (st->starttime != first_starttime))
    {
        return 1;
    }
    return 0;
}

/* Nanoseconds per second: the scale taz_proc_ticks_to_ns converts clock
 * ticks into. */
#define TAZ_PROC_NS_PER_SEC 1000000000ULL

uint64_t taz_proc_ticks_to_ns(uint64_t ticks, uint64_t ticks_per_sec)
{
    const uint64_t tps =
        (ticks_per_sec > 0U) ? ticks_per_sec : TAZ_PROC_DEFAULT_CLK_TCK;

    /* Whole seconds and the remainder separately: ticks * 1e9 would wrap
     * past ~1.8e10 ticks, which a busy many-threaded process reaches in
     * days. */
    return ((ticks / tps) * TAZ_PROC_NS_PER_SEC) +
           (((ticks % tps) * TAZ_PROC_NS_PER_SEC) / tps);
}

void taz_proc_cmdline_to_string(const uint8_t *buf, size_t len, char *out,
                                size_t outsize)
{
    char scratch[TAZ_PROC_CMDLINE_SCRATCH_LEN];
    size_t copy_len = len;
    size_t i;

    if (outsize == 0U)
    {
        return;
    }

    /* A single trailing NUL (the argv array's final terminator) is
     * dropped so the joined string never ends in a stray space. */
    if ((copy_len > 0U) && (buf[copy_len - 1U] == 0U))
    {
        copy_len -= 1U;
    }
    /* taz_fsutil_sanitize_utf8 never needs more source bytes than
     * outsize - 1 to fill its output, so anything past that (or past the
     * scratch buffer) is dropped unread. */
    if (copy_len > outsize)
    {
        copy_len = outsize;
    }
    if (copy_len > sizeof(scratch))
    {
        copy_len = sizeof(scratch);
    }
    for (i = 0U; i < copy_len; i++)
    {
        scratch[i] = (buf[i] == 0U) ? ' ' : (char)buf[i];
    }
    taz_fsutil_sanitize_utf8(scratch, copy_len, out, outsize);
}

/* ---------------------------------------------------------------------------
 * Platform ops: taz_process_enumerate/_inspect/_kill and their frees. Pool
 * thread for enumerate/inspect (/proc I/O via synchronous uv_fs_*); kill is
 * inline on the loop thread (kill(2) is not a filesystem call).
 * ------------------------------------------------------------------------- */

#define TAZ_PROC_DIR       "/proc"
#define TAZ_PROC_STAT_FILE "/proc/stat"

/* "/proc/<pid><suffix>" and "/proc/<pid>/fd/<fd>": pid and fd are each at
 * most 10 decimal digits, so this is generous for both. */
#define TAZ_PROC_PATH_BUF_LEN 64U

/* Growth chunk / caps for proc_read_file below. Per-pid stat and status
 * files are a few hundred bytes in practice; /proc/stat (every CPU's
 * counters plus btime) and cmdline can be larger. A cap stops growth
 * rather than failing: a read truncated well past every field this code
 * needs still parses fine, and cmdline is truncated again anyway by
 * taz_proc_cmdline_to_string's own outsize. */
#define TAZ_PROC_READ_CHUNK       4096U
#define TAZ_PROC_STAT_READ_CAP    4096U
#define TAZ_PROC_STATUS_READ_CAP  8192U
#define TAZ_PROC_CMDLINE_READ_CAP 8192U
#define TAZ_PROC_STAT_FILE_CAP    65536U

/* Initial geometric-growth capacities for the growable arrays below (pids,
 * open-file fds, process entries). */
#define TAZ_PROC_U32_LIST_INITIAL_CAP   64U
#define TAZ_PROC_LIST_INITIAL_CAP       64U
#define TAZ_PROC_OPEN_FILES_INITIAL_CAP 32U

/* taz_v1_ProcessInfoResponse.command_line: <= 4095 bytes + NUL. */
#define TAZ_PROCESS_CMDLINE_CAP 4096U

/* Builds "/proc/<pid><suffix>" into buf. Returns 0 (buf left unspecified)
 * when it would not fit. */
static int proc_build_path(char *buf, size_t bufsize, uint32_t pid,
                           const char *suffix)
{
    const int n =
        snprintf(buf, bufsize, "/proc/%lu%s", (unsigned long)pid, suffix);
    return (n >= 0) && ((size_t)n < bufsize);
}

/* Builds "/proc/<pid>/fd/<fd>" into buf. Returns 0 when it would not fit. */
static int proc_build_fd_path(char *buf, size_t bufsize, uint32_t pid,
                              uint32_t fd)
{
    const int n = snprintf(buf, bufsize, "/proc/%lu/fd/%lu", (unsigned long)pid,
                           (unsigned long)fd);
    return (n >= 0) && ((size_t)n < bufsize);
}

/* Parses a /proc scandir entry name (a pid or an fd number) as a decimal
 * uint32_t with no sign, no junk, no overflow (strtoull with endptr/errno,
 * cert-err34-c). Returns 0 when malformed, which also rejects non-numeric
 * entries such as "self" or "net". strtoull (not strtoul) keeps the
 * UINT32_MAX bound meaningful: unsigned long is only 32 bits on LLP64
 * platforms, which would make that comparison always false there. */
static int proc_parse_u32_name(const char *name, uint32_t *out_value)
{
    char *endptr;
    unsigned long long value;

    if ((name[0] < '0') || (name[0] > '9'))
    {
        return 0;
    }
    errno = 0;
    value = strtoull(name, &endptr, TAZ_PROC_DECIMAL_BASE);
    if ((*endptr != '\0') || (errno == ERANGE) ||
        (value > (unsigned long long)UINT32_MAX))
    {
        return 0;
    }
    *out_value = (uint32_t)value;
    return 1;
}

/* A growable array of uint32_t: /proc's numeric pid entries, or one pid's
 * numeric fd entries. */
typedef struct
{
    uint32_t *items;
    size_t count;
    size_t capacity;
} proc_u32_list_t;

/* Appends value, growing geometrically. Returns 0 on OOM (including an
 * overflowing capacity*sizeof(*items)) without touching the existing
 * array. */
static int proc_u32_list_append(proc_u32_list_t *list, uint32_t value)
{
    if (list->count == list->capacity)
    {
        const size_t new_capacity = (list->capacity == 0U)
                                        ? TAZ_PROC_U32_LIST_INITIAL_CAP
                                        : list->capacity * 2U;
        uint32_t *grown;

        if (new_capacity > (SIZE_MAX / sizeof(*grown)))
        {
            return 0;
        }
        grown = (uint32_t *)realloc(list->items, new_capacity * sizeof(*grown));
        if (grown == NULL)
        {
            return 0;
        }
        list->items = grown;
        list->capacity = new_capacity;
    }
    list->items[list->count] = value;
    list->count++;
    return 1;
}

static int proc_u32_compare(const void *a, const void *b)
{
    const uint32_t va = *(const uint32_t *)a;
    const uint32_t vb = *(const uint32_t *)b;

    if (va < vb)
    {
        return -1;
    }
    if (va > vb)
    {
        return 1;
    }
    return 0;
}

/* Closes fd, discarding the result: used only on paths where the read or
 * open that preceded it already reports whatever failure matters. */
static void proc_close_fd(uv_file fd)
{
    uv_fs_t req;

    (void)uv_fs_close(NULL, &req, fd, NULL);
    uv_fs_req_cleanup(&req);
}

/* Synchronously reads path in full into a heap buffer, via
 * uv_fs_open/_read/_close, growing geometrically up to cap (where it stops
 * rather than failing - see the cap macros above). On success, *out_buf is
 * a malloc'd buffer the caller frees and *out_len its length (never NUL-
 * terminated: callers pass text+len to the pure parsers, never strlen). On
 * failure, returns -1; when code/detail are non-NULL they are set from the
 * failing uv_fs_t exactly as taz_error_from_fs_req/taz_error_fs_detail
 * already do elsewhere in this codebase. Callers that only want a best-
 * effort read (status/cmdline: an unreadable one just means an unknown
 * user / empty command line, not a failure of the whole call) pass
 * NULL/NULL. */
static int proc_read_file(const char *path, size_t cap, char **out_buf,
                          size_t *out_len, taz_v1_ErrorCode *code,
                          const char **detail)
{
    uv_fs_t req;
    uv_file fd;
    char *buf = NULL;
    size_t capacity = 0U;
    size_t len = 0U;

    fd = uv_fs_open(NULL, &req, path, UV_FS_O_RDONLY, 0, NULL);
    if (fd < 0)
    {
        if (code != NULL)
        {
            *code = taz_error_from_fs_req(&req);
        }
        if (detail != NULL)
        {
            *detail = taz_error_fs_detail(&req);
        }
        uv_fs_req_cleanup(&req);
        return -1;
    }
    uv_fs_req_cleanup(&req);

    for (;;)
    {
        uv_buf_t iov;
        int n;

        if (len == capacity)
        {
            size_t new_capacity;
            char *grown;

            if (capacity >= cap)
            {
                break;
            }
            new_capacity =
                (capacity == 0U) ? TAZ_PROC_READ_CHUNK : capacity * 2U;
            if (new_capacity > cap)
            {
                new_capacity = cap;
            }
            grown = (char *)realloc(buf, new_capacity);
            if (grown == NULL)
            {
                free(buf);
                proc_close_fd(fd);
                if (code != NULL)
                {
                    *code = taz_v1_ErrorCode_ERROR_CODE_INTERNAL;
                }
                if (detail != NULL)
                {
                    *detail = "out of memory";
                }
                return -1;
            }
            buf = grown;
            capacity = new_capacity;
        }

        iov = uv_buf_init(buf + len, (unsigned int)(capacity - len));
        n = uv_fs_read(NULL, &req, fd, &iov, 1, (int64_t)len, NULL);
        if (n < 0)
        {
            if (code != NULL)
            {
                *code = taz_error_from_fs_req(&req);
            }
            if (detail != NULL)
            {
                *detail = taz_error_fs_detail(&req);
            }
            uv_fs_req_cleanup(&req);
            free(buf);
            proc_close_fd(fd);
            return -1;
        }
        uv_fs_req_cleanup(&req);
        if (n == 0)
        {
            break;
        }
        len += (size_t)n;
    }

    proc_close_fd(fd);
    *out_buf = buf;
    *out_len = len;
    return 0;
}

/* Best-effort /proc/stat "btime" read: *out_btime is left unchanged on any
 * failure to open, read or parse it. Used as an input to a CPU-percentage
 * calculation that itself degrades gracefully (a zero btime just
 * understates cpu_percent), never as a hard failure. */
static void proc_read_btime(uint64_t *out_btime)
{
    char *text = NULL;
    size_t len = 0U;

    if (proc_read_file(TAZ_PROC_STAT_FILE, TAZ_PROC_STAT_FILE_CAP, &text, &len,
                       NULL, NULL) != 0)
    {
        return;
    }
    (void)taz_proc_parse_btime(text, len, out_btime);
    free(text);
}

/* Appends a copy of *entry, growing list->entries geometrically. Returns 0
 * on OOM (including an overflowing capacity*sizeof(*entry)) without
 * touching the existing array. */
static int proc_entry_list_append(taz_process_list_t *list,
                                  const taz_process_entry_t *entry)
{
    if (list->count == list->capacity)
    {
        const size_t new_capacity = (list->capacity == 0U)
                                        ? TAZ_PROC_LIST_INITIAL_CAP
                                        : list->capacity * 2U;
        taz_process_entry_t *grown;

        if (new_capacity > (SIZE_MAX / sizeof(*grown)))
        {
            return 0;
        }
        grown = (taz_process_entry_t *)realloc(list->entries,
                                               new_capacity * sizeof(*grown));
        if (grown == NULL)
        {
            return 0;
        }
        list->entries = grown;
        list->capacity = new_capacity;
    }
    list->entries[list->count] = *entry;
    list->count++;
    return 1;
}

/* Fills entry->name/state/memory_bytes from an already-parsed stat_info (no
 * I/O). Shared by proc_fill_entry, taz_process_inspect and
 * taz_process_watch_sample so the three cannot drift apart. */
static void proc_fill_entry_name_state_memory(const taz_proc_stat_t *stat_info,
                                              uint64_t page_size,
                                              taz_process_entry_t *entry)
{
    taz_fsutil_sanitize_utf8(stat_info->comm, strlen(stat_info->comm),
                             entry->name, sizeof(entry->name));
    {
        const char *word = taz_proc_state_word(stat_info->state);
        taz_fsutil_sanitize_utf8(word, strlen(word), entry->state,
                                 sizeof(entry->state));
    }
    entry->memory_bytes = stat_info->rss_pages * page_size;
}

/* Fills entry->user from /proc/<pid>/status; leaves it "" (the memset
 * default) when the file is unreadable or has no parsable Uid: line. Not
 * a failure of the caller - shared by proc_fill_entry, taz_process_inspect
 * and taz_process_watch_sample. */
static void proc_fill_entry_user(uint32_t pid, taz_process_entry_t *entry)
{
    char path[TAZ_PROC_PATH_BUF_LEN];

    if (proc_build_path(path, sizeof(path), pid, "/status"))
    {
        char *status_text = NULL;
        size_t status_len = 0U;

        if (proc_read_file(path, TAZ_PROC_STATUS_READ_CAP, &status_text,
                           &status_len, NULL, NULL) == 0)
        {
            unsigned long uid;

            if (taz_proc_parse_status_uid(status_text, status_len, &uid))
            {
                taz_user_name_from_uid(TAZ_PASSWD_PATH, uid, entry->user,
                                       sizeof(entry->user));
            }
            free(status_text);
        }
    }
}

/* Fills *entry for pid from /proc/<pid>/stat (+ /proc/<pid>/status for the
 * owning uid). Returns 0 when the process cannot be read at all (it
 * exited mid-scan, is inaccessible, or its stat line is malformed): the
 * caller drops such entries rather than failing the whole enumeration. An
 * unreadable status file (unknown user) is not such a failure: entry's
 * user is simply left "". */
static int proc_fill_entry(uint32_t pid, uint64_t btime, uint64_t clk_tck,
                           uint64_t page_size, taz_process_entry_t *entry)
{
    char path[TAZ_PROC_PATH_BUF_LEN];
    char *stat_text = NULL;
    size_t stat_len = 0U;
    taz_proc_stat_t stat_info;
    uint64_t start_time;

    memset(entry, 0, sizeof(*entry));
    entry->pid = pid;

    if (!proc_build_path(path, sizeof(path), pid, "/stat"))
    {
        return 0;
    }
    if (proc_read_file(path, TAZ_PROC_STAT_READ_CAP, &stat_text, &stat_len,
                       NULL, NULL) != 0)
    {
        return 0;
    }
    if (!taz_proc_parse_stat(stat_text, stat_len, &stat_info))
    {
        free(stat_text);
        return 0;
    }
    free(stat_text);

    proc_fill_entry_name_state_memory(&stat_info, page_size, entry);

    start_time = btime + (stat_info.starttime / clk_tck);
    {
        const double elapsed = (double)time(NULL) - (double)start_time;
        entry->cpu_percent = taz_proc_cpu_percent(
            stat_info.utime + stat_info.stime, clk_tck, elapsed);
    }

    proc_fill_entry_user(pid, entry);

    return 1;
}

int taz_process_enumerate(const char *filter, taz_process_list_t *out,
                          taz_v1_ErrorCode *code, const char **detail)
{
    uv_fs_t scan_req;
    uv_dirent_t ent;
    proc_u32_list_t pids = {NULL, 0U, 0U};
    uint64_t btime = 0U;
    const long clk_tck_raw = sysconf(_SC_CLK_TCK);
    const long page_size_raw = sysconf(_SC_PAGESIZE);
    const uint64_t clk_tck =
        (clk_tck_raw > 0) ? (uint64_t)clk_tck_raw : TAZ_PROC_DEFAULT_CLK_TCK;
    const uint64_t page_size = (page_size_raw > 0) ? (uint64_t)page_size_raw
                                                   : TAZ_PROC_DEFAULT_PAGE_SIZE;
    size_t i;

    memset(out, 0, sizeof(*out));
    proc_read_btime(&btime);

    if (uv_fs_scandir(NULL, &scan_req, TAZ_PROC_DIR, 0, NULL) < 0)
    {
        *code = taz_error_from_fs_req(&scan_req);
        *detail = taz_error_fs_detail(&scan_req);
        uv_fs_req_cleanup(&scan_req);
        return -1;
    }

    while (uv_fs_scandir_next(&scan_req, &ent) != UV_EOF)
    {
        uint32_t pid;

        if (!proc_parse_u32_name(ent.name, &pid))
        {
            continue;
        }
        if (!proc_u32_list_append(&pids, pid))
        {
            uv_fs_req_cleanup(&scan_req);
            free(pids.items);
            *code = taz_v1_ErrorCode_ERROR_CODE_INTERNAL;
            *detail = "out of memory";
            return -1;
        }
    }
    uv_fs_req_cleanup(&scan_req);

    if (pids.count > 0U)
    {
        qsort(pids.items, pids.count, sizeof(*pids.items), proc_u32_compare);
    }

    for (i = 0U; i < pids.count; i++)
    {
        taz_process_entry_t entry;

        if (!proc_fill_entry(pids.items[i], btime, clk_tck, page_size, &entry))
        {
            continue;
        }
        if ((filter[0] != '\0') && (strstr(entry.name, filter) == NULL))
        {
            continue;
        }
        if (!proc_entry_list_append(out, &entry))
        {
            free(pids.items);
            taz_process_list_free(out);
            *code = taz_v1_ErrorCode_ERROR_CODE_INTERNAL;
            *detail = "out of memory";
            return -1;
        }
    }

    free(pids.items);
    return 0;
}

void taz_process_list_free(taz_process_list_t *list)
{
    free(list->entries);
    memset(list, 0, sizeof(*list));
}

/* A growable array of 1024-byte rows: PROCESS_INFO's open_files, one
 * sanitized+truncated readlink target per live fd, in ascending fd
 * order. */
typedef struct
{
    char (*rows)[1024];
    size_t count;
    size_t capacity;
} proc_open_files_t;

/* Appends one sanitized+truncated copy of target, growing list->rows
 * geometrically. Returns 0 on OOM (including an overflowing
 * capacity*sizeof(*rows)) without touching the existing array. */
static int proc_open_files_append(proc_open_files_t *list, const char *target,
                                  size_t target_len)
{
    if (list->count == list->capacity)
    {
        const size_t new_capacity = (list->capacity == 0U)
                                        ? TAZ_PROC_OPEN_FILES_INITIAL_CAP
                                        : list->capacity * 2U;
        char (*grown)[1024];

        if (new_capacity > (SIZE_MAX / sizeof(*grown)))
        {
            return 0;
        }
        grown =
            (char (*)[1024])realloc(list->rows, new_capacity * sizeof(*grown));
        if (grown == NULL)
        {
            return 0;
        }
        list->rows = grown;
        list->capacity = new_capacity;
    }
    taz_fsutil_sanitize_utf8(target, target_len, list->rows[list->count],
                             sizeof(list->rows[0]));
    list->count++;
    return 1;
}

/* Fills out->open_files/open_files_count from /proc/<pid>/fd: every
 * readable fd's readlink target, ascending by fd number. Never fails the
 * caller: a fd directory that cannot be opened (EACCES, or the process
 * having exited between the stat read in taz_process_inspect and here)
 * just leaves open_files empty, and an individual fd that vanishes
 * between scandir and readlink is skipped the same way DIR_LIST skips a
 * vanished entry. */
static void proc_fill_open_files(uint32_t pid, taz_process_detail_t *out)
{
    char dir_path[TAZ_PROC_PATH_BUF_LEN];
    uv_fs_t scan_req;
    uv_dirent_t ent;
    proc_u32_list_t fds = {NULL, 0U, 0U};
    proc_open_files_t files = {NULL, 0U, 0U};
    size_t i;

    if (!proc_build_path(dir_path, sizeof(dir_path), pid, "/fd"))
    {
        return;
    }
    if (uv_fs_scandir(NULL, &scan_req, dir_path, 0, NULL) < 0)
    {
        uv_fs_req_cleanup(&scan_req);
        return;
    }
    while (uv_fs_scandir_next(&scan_req, &ent) != UV_EOF)
    {
        uint32_t fd_num;

        if (!proc_parse_u32_name(ent.name, &fd_num))
        {
            continue;
        }
        if (!proc_u32_list_append(&fds, fd_num))
        {
            break;
        }
    }
    uv_fs_req_cleanup(&scan_req);

    if (fds.count > 0U)
    {
        qsort(fds.items, fds.count, sizeof(*fds.items), proc_u32_compare);
    }

    for (i = 0U; i < fds.count; i++)
    {
        char link_path[TAZ_PROC_PATH_BUF_LEN];
        uv_fs_t link_req;

        if (!proc_build_fd_path(link_path, sizeof(link_path), pid,
                                fds.items[i]))
        {
            continue;
        }
        if (uv_fs_readlink(NULL, &link_req, link_path, NULL) == 0)
        {
            const char *target = (const char *)link_req.ptr;
            (void)proc_open_files_append(&files, target, strlen(target));
        }
        uv_fs_req_cleanup(&link_req);
    }
    free(fds.items);

    out->open_files = files.rows;
    out->open_files_count = files.count;
}

int taz_process_inspect(uint32_t pid, taz_process_detail_t *out,
                        taz_v1_ErrorCode *code, const char **detail)
{
    char path[TAZ_PROC_PATH_BUF_LEN];
    char *stat_text = NULL;
    size_t stat_len = 0U;
    taz_proc_stat_t stat_info;
    uint64_t btime = 0U;
    const long clk_tck_raw = sysconf(_SC_CLK_TCK);
    const long page_size_raw = sysconf(_SC_PAGESIZE);
    const uint64_t clk_tck =
        (clk_tck_raw > 0) ? (uint64_t)clk_tck_raw : TAZ_PROC_DEFAULT_CLK_TCK;
    const uint64_t page_size = (page_size_raw > 0) ? (uint64_t)page_size_raw
                                                   : TAZ_PROC_DEFAULT_PAGE_SIZE;

    memset(out, 0, sizeof(*out));

    if (!proc_build_path(path, sizeof(path), pid, "/stat"))
    {
        *code = taz_v1_ErrorCode_ERROR_CODE_INTERNAL;
        *detail = "pid path too long";
        return -1;
    }
    if (proc_read_file(path, TAZ_PROC_STAT_READ_CAP, &stat_text, &stat_len,
                       code, detail) != 0)
    {
        return -1;
    }
    if (!taz_proc_parse_stat(stat_text, stat_len, &stat_info))
    {
        free(stat_text);
        *code = taz_v1_ErrorCode_ERROR_CODE_INTERNAL;
        *detail = "unparseable /proc stat";
        return -1;
    }
    free(stat_text);

    proc_read_btime(&btime);

    out->info.pid = pid;
    proc_fill_entry_name_state_memory(&stat_info, page_size, &out->info);
    out->start_time = btime + (stat_info.starttime / clk_tck);
    {
        const double elapsed = (double)time(NULL) - (double)out->start_time;
        out->info.cpu_percent = taz_proc_cpu_percent(
            stat_info.utime + stat_info.stime, clk_tck, elapsed);
    }
    proc_fill_entry_user(pid, &out->info);

    out->command_line = (char *)calloc(1U, TAZ_PROCESS_CMDLINE_CAP);
    if (out->command_line == NULL)
    {
        taz_process_detail_free(out);
        *code = taz_v1_ErrorCode_ERROR_CODE_INTERNAL;
        *detail = "out of memory";
        return -1;
    }
    if (proc_build_path(path, sizeof(path), pid, "/cmdline"))
    {
        char *cmdline_raw = NULL;
        size_t cmdline_len = 0U;

        if (proc_read_file(path, TAZ_PROC_CMDLINE_READ_CAP, &cmdline_raw,
                           &cmdline_len, NULL, NULL) == 0)
        {
            taz_proc_cmdline_to_string((const uint8_t *)cmdline_raw,
                                       cmdline_len, out->command_line,
                                       TAZ_PROCESS_CMDLINE_CAP);
            free(cmdline_raw);
        }
    }

    proc_fill_open_files(pid, out);

    return 0;
}

void taz_process_detail_free(taz_process_detail_t *detail)
{
    free(detail->command_line);
    free(detail->open_files);
    memset(detail, 0, sizeof(*detail));
}

int taz_process_kill(uint32_t pid, int32_t signal, taz_v1_ErrorCode *code,
                     const char **detail)
{
    const int sig = (signal == 0) ? SIGTERM : (int)signal;
    int saved_errno;

    if (kill((pid_t)pid, sig) == 0)
    {
        return 0;
    }

    saved_errno = errno;
    switch (saved_errno)
    {
        case ESRCH:
            *code = taz_v1_ErrorCode_ERROR_CODE_NOT_FOUND;
            break;
        case EPERM:
            *code = taz_v1_ErrorCode_ERROR_CODE_PERMISSION_DENIED;
            break;
        case EINVAL:
            *code = taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST;
            break;
        default:
            *code = taz_v1_ErrorCode_ERROR_CODE_INTERNAL;
            break;
    }
    *detail = uv_strerror(uv_translate_sys_error(saved_errno));
    return -1;
}

/* ---------------------------------------------------------------------------
 * Watch/sample: taz_process_watch_open/_sample/_close. Open and close run
 * inline on the loop thread; sample does blocking /proc I/O and runs on
 * the pool.
 * ------------------------------------------------------------------------- */

int taz_process_watch_open(uint32_t pid, int use_pidfd, taz_process_watch_t *w,
                           taz_v1_ErrorCode *code, const char **detail)
{
    memset(w, 0, sizeof(*w));
    w->pid = pid;
    w->exit_fd = -1;

    if (use_pidfd == 0)
    {
        return 0;
    }

    {
        const long rc = syscall(SYS_pidfd_open, (pid_t)pid, 0);
        const int saved_errno = (rc < 0) ? errno : 0;

        if (rc >= 0)
        {
            w->exit_fd = (int)rc;
            return 0;
        }
        if (saved_errno == ESRCH)
        {
            *code = taz_v1_ErrorCode_ERROR_CODE_NOT_FOUND;
            *detail = uv_strerror(uv_translate_sys_error(saved_errno));
            return -1;
        }
        /* ENOSYS (kernel < 5.3), EPERM (container seccomp) or anything
         * else: fall back to the timer path (exit_fd stays -1). */
        return 0;
    }
}

int taz_process_watch_sample(const taz_process_watch_t *w,
                             taz_process_sample_t *out, taz_v1_ErrorCode *code,
                             const char **detail)
{
    char path[TAZ_PROC_PATH_BUF_LEN];
    char *stat_text = NULL;
    size_t stat_len = 0U;
    taz_proc_stat_t stat_info;
    const long clk_tck_raw = sysconf(_SC_CLK_TCK);
    const long page_size_raw = sysconf(_SC_PAGESIZE);
    const uint64_t clk_tck =
        (clk_tck_raw > 0) ? (uint64_t)clk_tck_raw : TAZ_PROC_DEFAULT_CLK_TCK;
    const uint64_t page_size = (page_size_raw > 0) ? (uint64_t)page_size_raw
                                                   : TAZ_PROC_DEFAULT_PAGE_SIZE;

    memset(out, 0, sizeof(*out));

    if (!proc_build_path(path, sizeof(path), w->pid, "/stat"))
    {
        *code = taz_v1_ErrorCode_ERROR_CODE_INTERNAL;
        *detail = "pid path too long";
        return -1;
    }
    if (proc_read_file(path, TAZ_PROC_STAT_READ_CAP, &stat_text, &stat_len,
                       code, detail) != 0)
    {
        return -1;
    }
    if (!taz_proc_parse_stat(stat_text, stat_len, &stat_info))
    {
        free(stat_text);
        *code = taz_v1_ErrorCode_ERROR_CODE_INTERNAL;
        *detail = "unparseable /proc stat";
        return -1;
    }
    free(stat_text);

    out->info.pid = w->pid;
    proc_fill_entry_name_state_memory(&stat_info, page_size, &out->info);
    proc_fill_entry_user(w->pid, &out->info);
    out->cpu_time_ns =
        taz_proc_ticks_to_ns(stat_info.utime + stat_info.stime, clk_tck);
    out->starttime = stat_info.starttime;
    out->state = taz_proc_stat_is_exited(&stat_info, w->first_starttime)
                     ? TAZ_PROCESS_SAMPLE_EXITED
                     : TAZ_PROCESS_SAMPLE_LIVE;
    return 0;
}

void taz_process_watch_close(taz_process_watch_t *w)
{
    if (w->exit_fd >= 0)
    {
        (void)close(w->exit_fd);
    }
    memset(w, 0, sizeof(*w));
    w->exit_fd = -1;
}

#endif /* !_WIN32 */
