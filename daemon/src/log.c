#include "taz/log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <uv.h>

#include "taz/fsutil.h"

/* Bounded scratch buffer for vsnprintf; independent of LogEntry.message's
 * wire limit so a long format still gets sanitized/cut uniformly below
 * rather than silently truncated mid-format. */
#define TAZ_LOG_FORMAT_BUF_SIZE 1024U
/* Mirrors LogEntry.message's max_size (advanced.options). */
#define TAZ_LOG_MESSAGE_MAX 512U

typedef struct
{
    uint64_t timestamp;
    taz_log_level_t level;
    char message[TAZ_LOG_MESSAGE_MAX];
} taz_log_entry_t;

static const char *const TAZ_LOG_LEVEL_NAMES[] = {"DEBUG", "INFO", "WARN",
                                                  "ERROR"};
#define TAZ_LOG_LEVEL_COUNT                                                    \
    (sizeof(TAZ_LOG_LEVEL_NAMES) / sizeof(TAZ_LOG_LEVEL_NAMES[0]))

static taz_log_entry_t g_ring[TAZ_LOG_RING_CAPACITY];
static size_t g_ring_capacity = TAZ_LOG_RING_CAPACITY;
static size_t g_ring_head = 0U;  /* index the next taz_log() call writes to */
static size_t g_ring_count = 0U; /* valid entries, always <= g_ring_capacity */
/* Mirrors config.c's "log.level" default; config.c pushes every change
 * through taz_log_set_level so this never diverges from the config store. */
static taz_log_level_t g_log_level = TAZ_LOG_INFO;
static uv_mutex_t g_log_mutex;
static int g_initialized = 0;

void taz_log_init(void)
{
    if (g_initialized)
    {
        return;
    }
    (void)uv_mutex_init(&g_log_mutex);
    g_ring_head = 0U;
    g_ring_count = 0U;
    g_ring_capacity = TAZ_LOG_RING_CAPACITY;
    g_log_level = TAZ_LOG_INFO;
    g_initialized = 1;
}

void taz_log_shutdown(void)
{
    if (!g_initialized)
    {
        return;
    }
    g_initialized = 0;
    uv_mutex_destroy(&g_log_mutex);
}

int taz_log_level_parse(const char *s, taz_log_level_t *out)
{
    if (s == NULL || s[0] == '\0')
    {
        *out = TAZ_LOG_DEBUG;
        return 0;
    }
    for (size_t i = 0; i < TAZ_LOG_LEVEL_COUNT; i++)
    {
        if (strcmp(s, TAZ_LOG_LEVEL_NAMES[i]) == 0)
        {
            *out = (taz_log_level_t)i;
            return 0;
        }
    }
    return 1;
}

void taz_log_set_level(taz_log_level_t level)
{
    if (!g_initialized)
    {
        return;
    }
    uv_mutex_lock(&g_log_mutex);
    g_log_level = level;
    uv_mutex_unlock(&g_log_mutex);
}

void taz_log(taz_log_level_t level, TAZ_LOG_FORMAT_STRING const char *fmt, ...)
{
    if (!g_initialized)
    {
        return;
    }

    uv_mutex_lock(&g_log_mutex);
    taz_log_level_t min_level = g_log_level;
    uv_mutex_unlock(&g_log_mutex);
    if (level < min_level)
    {
        return;
    }

    char formatted[TAZ_LOG_FORMAT_BUF_SIZE];
    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(formatted, sizeof(formatted), fmt, args);
    va_end(args);
    if (written < 0)
    {
        formatted[0] = '\0';
    }

    char message[TAZ_LOG_MESSAGE_MAX];
    taz_fsutil_sanitize_utf8(formatted, strlen(formatted), message,
                             sizeof(message));

    uint64_t timestamp = (uint64_t)time(NULL);

    uv_mutex_lock(&g_log_mutex);

    taz_log_entry_t *slot = &g_ring[g_ring_head];
    slot->timestamp = timestamp;
    slot->level = level;
    (void)memcpy(slot->message, message, sizeof(slot->message));

    g_ring_head = (g_ring_head + 1U) % g_ring_capacity;
    if (g_ring_count < g_ring_capacity)
    {
        g_ring_count++;
    }

    (void)fprintf(stderr, "%s %s\n", TAZ_LOG_LEVEL_NAMES[level], message);

    uv_mutex_unlock(&g_log_mutex);
}

size_t taz_log_collect(uint32_t lines, uint64_t since,
                       taz_log_level_t min_level, taz_v1_LogEntry *out,
                       size_t out_cap)
{
    if (!g_initialized || out_cap == 0U)
    {
        return 0U;
    }

    uv_mutex_lock(&g_log_mutex);

    size_t capacity = g_ring_capacity;
    size_t count = g_ring_count;
    size_t oldest = (count < capacity) ? 0U : g_ring_head;

    size_t match_count = 0U;
    for (size_t i = 0; i < count; i++)
    {
        size_t idx = (oldest + i) % capacity;
        if (g_ring[idx].timestamp > since && g_ring[idx].level >= min_level)
        {
            match_count++;
        }
    }

    size_t skip = 0U;
    if (lines != 0U && match_count > (size_t)lines)
    {
        skip = match_count - (size_t)lines;
    }

    size_t seen = 0U;
    size_t written = 0U;
    for (size_t i = 0; i < count && written < out_cap; i++)
    {
        size_t idx = (oldest + i) % capacity;
        const taz_log_entry_t *entry = &g_ring[idx];
        if (entry->timestamp > since && entry->level >= min_level)
        {
            if (seen >= skip)
            {
                taz_v1_LogEntry *dst = &out[written];
                dst->timestamp = entry->timestamp;
                (void)strncpy(dst->level, TAZ_LOG_LEVEL_NAMES[entry->level],
                              sizeof(dst->level) - 1U);
                dst->level[sizeof(dst->level) - 1U] = '\0';
                (void)strncpy(dst->message, entry->message,
                              sizeof(dst->message) - 1U);
                dst->message[sizeof(dst->message) - 1U] = '\0';
                written++;
            }
            seen++;
        }
    }

    uv_mutex_unlock(&g_log_mutex);
    return written;
}

void taz_log_reset_for_tests(void)
{
    if (!g_initialized)
    {
        return;
    }
    uv_mutex_lock(&g_log_mutex);
    g_ring_head = 0U;
    g_ring_count = 0U;
    uv_mutex_unlock(&g_log_mutex);
}

void taz_log_set_capacity_for_tests(size_t n)
{
    if (!g_initialized)
    {
        return;
    }
    if (n == 0U)
    {
        n = 1U;
    }
    if (n > TAZ_LOG_RING_CAPACITY)
    {
        n = TAZ_LOG_RING_CAPACITY;
    }
    uv_mutex_lock(&g_log_mutex);
    g_ring_capacity = n;
    g_ring_head = 0U;
    g_ring_count = 0U;
    uv_mutex_unlock(&g_log_mutex);
}
