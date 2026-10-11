#include "taz/task_registry.h"

#include <stddef.h>
#include <string.h>

#include <uv.h>

typedef enum
{
    ENTRY_FREE = 0,
    ENTRY_RUNNING = 1,
    ENTRY_EXITED = 2
} entry_state_t;

typedef struct
{
    uint64_t start_key;
    int64_t exit_code;
    uint64_t exit_seq; /* orders exited entries for reuse; 0 while running */
    uint32_t pid;
    entry_state_t state;
} registry_entry_t;

static registry_entry_t g_entries[TAZ_TASK_REGISTRY_CAPACITY];
static uint64_t g_next_exit_seq = 0U;
static uv_mutex_t g_mutex;
static uv_once_t g_once = UV_ONCE_INIT;

static void init_mutex(void)
{
    (void)uv_mutex_init(&g_mutex);
}

static void ensure_init(void)
{
    uv_once(&g_once, init_mutex);
}

/* Caller holds g_mutex. */
static registry_entry_t *find_entry(uint32_t pid, uint64_t start_key)
{
    for (size_t i = 0; i < TAZ_TASK_REGISTRY_CAPACITY; i++)
    {
        registry_entry_t *e = &g_entries[i];
        if (e->state != ENTRY_FREE && e->pid == pid &&
            e->start_key == start_key)
        {
            return e;
        }
    }
    return NULL;
}

/* A free slot, else the entry that exited longest ago, else NULL (every
 * slot running). Caller holds g_mutex. */
static registry_entry_t *find_reusable(void)
{
    registry_entry_t *oldest = NULL;
    for (size_t i = 0; i < TAZ_TASK_REGISTRY_CAPACITY; i++)
    {
        registry_entry_t *e = &g_entries[i];
        if (e->state == ENTRY_FREE)
        {
            return e;
        }
        if (e->state == ENTRY_EXITED &&
            (oldest == NULL || e->exit_seq < oldest->exit_seq))
        {
            oldest = e;
        }
    }
    return oldest;
}

int taz_task_registry_has_room(void)
{
    ensure_init();
    uv_mutex_lock(&g_mutex);
    const int room = (find_reusable() != NULL) ? 1 : 0;
    uv_mutex_unlock(&g_mutex);
    return room;
}

int taz_task_registry_register(uint32_t pid, uint64_t start_key)
{
    ensure_init();
    uv_mutex_lock(&g_mutex);

    int rc = 0;
    registry_entry_t *e = find_entry(pid, start_key);
    if (e == NULL)
    {
        e = find_reusable();
    }
    if (e == NULL)
    {
        rc = UV_ENOMEM;
    }
    else
    {
        e->pid = pid;
        e->start_key = start_key;
        e->exit_code = 0;
        e->exit_seq = 0U;
        e->state = ENTRY_RUNNING;
    }

    uv_mutex_unlock(&g_mutex);
    return rc;
}

void taz_task_registry_record_exit(uint32_t pid, uint64_t start_key,
                                   int64_t exit_code)
{
    ensure_init();
    uv_mutex_lock(&g_mutex);

    registry_entry_t *e = find_entry(pid, start_key);
    if (e != NULL && e->state == ENTRY_RUNNING)
    {
        e->state = ENTRY_EXITED;
        e->exit_code = exit_code;
        e->exit_seq = ++g_next_exit_seq;
    }

    uv_mutex_unlock(&g_mutex);
}

int taz_task_registry_lookup(uint32_t pid, uint64_t start_key,
                             int64_t *out_exit_code)
{
    ensure_init();
    uv_mutex_lock(&g_mutex);

    int found = 0;
    const registry_entry_t *e = find_entry(pid, start_key);
    if (e != NULL && e->state == ENTRY_EXITED)
    {
        *out_exit_code = e->exit_code;
        found = 1;
    }

    uv_mutex_unlock(&g_mutex);
    return found;
}

int taz_task_registry_is_pending(uint32_t pid, uint64_t start_key)
{
    ensure_init();
    uv_mutex_lock(&g_mutex);
    const registry_entry_t *e = find_entry(pid, start_key);
    const int pending = (e != NULL && e->state == ENTRY_RUNNING) ? 1 : 0;
    uv_mutex_unlock(&g_mutex);
    return pending;
}

void taz_task_registry_reset_for_tests(void)
{
    ensure_init();
    uv_mutex_lock(&g_mutex);
    (void)memset(g_entries, 0, sizeof(g_entries));
    g_next_exit_seq = 0U;
    uv_mutex_unlock(&g_mutex);
}
