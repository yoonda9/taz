#include "taz/exec.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "taz/process.h"
#include "taz/run_as.h"

#ifdef _WIN32
#include <io.h>
#include <userenv.h>
#include <windows.h>

#include "taz/cmdline.h"
#else
#include <signal.h>
#endif

/* Env var name matching for override/add merge: case-insensitive on
 * Windows (its environment is case-insensitive), case-sensitive on POSIX. */
#ifdef _WIN32
#define TAZ_EXEC_ENV_NAME_CMP _stricmp
#else
#define TAZ_EXEC_ENV_NAME_CMP strcmp
#endif

/* Largest size on_alloc can report through uv_buf_t.len without narrowing:
 * that field is size_t on the POSIX backend but ULONG (32-bit, always,
 * even on 64-bit Windows) on the Windows backend. libuv's own suggested
 * size is small (64 KiB), so this only ever clamps a hypothetically huge
 * suggestion rather than firing in practice. */
#ifdef _WIN32
#define TAZ_EXEC_ALLOC_MAX ((size_t)ULONG_MAX)
#else
#define TAZ_EXEC_ALLOC_MAX SIZE_MAX
#endif

/* Initial allocation for a stream buffer on its first append. */
#define TAZ_EXEC_CAPTURE_INITIAL_CAP 64U

/* Ensure *buf has room for at least `needed` bytes, growing by doubling
 * and never past `max_bytes` (the combined cap, which `needed` can never
 * exceed). Returns 0 on allocation failure, leaving *buf and *buf_cap
 * unchanged. */
static int grow(uint8_t **buf, size_t *buf_cap, size_t needed, size_t max_bytes)
{
    if (needed <= *buf_cap)
    {
        return 1;
    }

    size_t new_cap = (*buf_cap != 0U) ? *buf_cap : TAZ_EXEC_CAPTURE_INITIAL_CAP;
    while (new_cap < needed)
    {
        if (new_cap > SIZE_MAX / 2U)
        {
            new_cap = needed;
            break;
        }
        new_cap *= 2U;
    }
    if (new_cap > max_bytes)
    {
        new_cap = max_bytes;
    }

    uint8_t *grown = (uint8_t *)realloc(*buf, new_cap);
    if (grown == NULL)
    {
        return 0;
    }
    *buf = grown;
    *buf_cap = new_cap;
    return 1;
}

void taz_exec_capture_init(taz_exec_capture_t *cap, size_t max_bytes)
{
    cap->out = NULL;
    cap->out_len = 0U;
    cap->out_cap = 0U;
    cap->err = NULL;
    cap->err_len = 0U;
    cap->err_cap = 0U;
    cap->max_bytes = max_bytes;
    cap->truncated = 0;
}

void taz_exec_capture_append(taz_exec_capture_t *cap, taz_exec_stream_t which,
                             const void *data, size_t len)
{
    if (cap->truncated || len == 0U)
    {
        return;
    }

    size_t combined = cap->out_len + cap->err_len;
    size_t remaining =
        (combined < cap->max_bytes) ? cap->max_bytes - combined : 0U;
    if (remaining == 0U)
    {
        cap->truncated = 1;
        return;
    }

    size_t take = (len <= remaining) ? len : remaining;

    uint8_t **buf;
    size_t *buf_len;
    size_t *buf_cap;
    if (which == TAZ_EXEC_STREAM_OUT)
    {
        buf = &cap->out;
        buf_len = &cap->out_len;
        buf_cap = &cap->out_cap;
    }
    else
    {
        buf = &cap->err;
        buf_len = &cap->err_len;
        buf_cap = &cap->err_cap;
    }

    if (!grow(buf, buf_cap, *buf_len + take, cap->max_bytes))
    {
        cap->truncated = 1;
        return;
    }

    (void)memcpy(*buf + *buf_len, data, take);
    *buf_len += take;

    if (take < len)
    {
        cap->truncated = 1;
    }
}

void taz_exec_capture_free(taz_exec_capture_t *cap)
{
    free(cap->out);
    free(cap->err);
    cap->out = NULL;
    cap->out_len = 0U;
    cap->out_cap = 0U;
    cap->err = NULL;
    cap->err_len = 0U;
    cap->err_cap = 0U;
}

/* Number of completion events that must all arrive before an exec is
 * finished: the process exit and EOF on each of the two captured pipes. The
 * timeout timer is not one of these - it only ever triggers a tree kill,
 * never a completion by itself. */
#define TAZ_EXEC_PENDING_COUNT 3

/* Handles closed as a unit once an exec finishes, or once a spawn attempt
 * that got this far fails: the process handle, its two pipes, and the
 * timeout timer (always created, started only when a timeout was asked
 * for, but always closed so loop teardown never depends on that). */
#define TAZ_EXEC_HANDLE_COUNT 4

/* Handles closed when setup fails before uv_spawn is attempted: the two
 * pipes and the timer. The process handle is not part of this count - in
 * this path uv_spawn has not been called, so it is not yet valid to close. */
#define TAZ_EXEC_PRESPAWN_HANDLE_COUNT 3

/* stdio slots passed to uv_spawn: stdin (ignored), stdout, and stderr -
 * matching the two pipes captured above. */
#define TAZ_EXEC_STDIO_COUNT 3

struct taz_exec_s
{
    uv_process_t process;
    uv_pipe_t out_pipe;
    uv_pipe_t err_pipe;
    uv_timer_t timer;
#ifdef _WIN32
    /* Per-exec Job Object the child is assigned to right after spawn, so
     * that killing the job (KILL_ON_JOB_CLOSE) takes the whole tree with it.
     * NULL if creation failed before a process was ever spawned. Shared by
     * both spawn paths below. */
    HANDLE job;
    /* The CreateProcessAsUserW path (spec->token != NULL): signalled by
     * on_process_wait (a system thread pool callback that does nothing
     * else) once the child exits, so the real work - reading the exit code
     * and firing the on-exit hook/finish_if_ready - happens on the loop
     * thread instead. close_all closes this instead of `process` on this
     * path; `process`'s own fields (pid aside) are never touched, since
     * uv_spawn is never called for it. */
    uv_async_t exit_async;
    HANDLE process_handle;
    HANDLE wait;    /* RegisterWaitForSingleObject's handle; NULL once
                     * unregistered or never registered */
    HANDLE profile; /* LoadUserProfileW's hProfile; NULL if never loaded */
    HANDLE token;   /* spec->token, ownership moved in; closed on teardown */
    bool as_user;   /* true once the CreateProcessAsUserW path is taken */
#endif
    taz_exec_capture_t capture;
    taz_exec_done_fn_t on_done;
    void *arg;
    taz_exec_on_exit_fn_t on_exit; /* NULL unless taz_exec_set_on_exit armed
                                    * it; cleared once fired */
    void *on_exit_arg;
    uint64_t start_key; /* read by taz_exec_set_on_exit; valid once on_exit
                         * is non-NULL */
    int pending;        /* counts down from TAZ_EXEC_PENDING_COUNT to 0 */
    int closing;        /* counts down from TAZ_EXEC_HANDLE_COUNT to 0 */
    int64_t exit_status;
    int term_signal;
    bool timed_out;
    bool cancelled;
    bool done; /* true once on_done has fired; guards late cancel/timeout */
};

static void on_handle_closed(uv_handle_t *handle)
{
    taz_exec_t *x = (taz_exec_t *)handle->data;

    if (--x->closing > 0)
    {
        return;
    }

#ifdef _WIN32
    if (x->job != NULL)
    {
        (void)CloseHandle(x->job);
    }
    if (x->as_user)
    {
        /* On_exit_async already unregisters the wait before this point is
         * ever reachable through the normal (post-spawn) lifecycle; this
         * second check only matters for a setup failure that parked the
         * token/profile/process handle in x but never got as far as
         * registering a wait. */
        if (x->wait != NULL)
        {
            (void)UnregisterWaitEx(x->wait, NULL);
        }
        if (x->process_handle != NULL)
        {
            (void)CloseHandle(x->process_handle);
        }
        if (x->profile != NULL)
        {
            (void)UnloadUserProfile(x->token, x->profile);
        }
        taz_run_as_token_close(x->token);
    }
#endif
    taz_exec_capture_free(&x->capture);
    free(x);
}

/* Close every handle belonging to x. Safe to call for a spawn that never
 * started (uv_spawn always initializes the process handle before it can
 * fail, and on the CreateProcessAsUserW path exit_async is always
 * initialized before anything else fallible is attempted - see
 * spawn_as_user) as well as for one that ran to completion; on_handle_closed
 * frees x once all close callbacks have fired. */
static void close_all(taz_exec_t *x)
{
    x->closing = TAZ_EXEC_HANDLE_COUNT;
#ifdef _WIN32
    if (x->as_user)
    {
        uv_close((uv_handle_t *)&x->exit_async, on_handle_closed);
    }
    else
    {
        uv_close((uv_handle_t *)&x->process, on_handle_closed);
    }
#else
    uv_close((uv_handle_t *)&x->process, on_handle_closed);
#endif
    uv_close((uv_handle_t *)&x->out_pipe, on_handle_closed);
    uv_close((uv_handle_t *)&x->err_pipe, on_handle_closed);
    uv_close((uv_handle_t *)&x->timer, on_handle_closed);
}

/* Kill x's whole process tree. A no-op once the child has already exited
 * (POSIX: ESRCH from uv_kill, silently ignored) or if the process was never
 * successfully spawned (Windows: x->job is NULL). */
#ifdef _WIN32
static void tree_kill(taz_exec_t *x)
{
    if (x->job != NULL)
    {
        (void)TerminateJobObject(x->job, 1);
    }
}
#else
static void tree_kill(taz_exec_t *x)
{
    if (x->process.pid > 0)
    {
        int rc = uv_kill(-x->process.pid, SIGKILL);
        (void)rc;
    }
}
#endif

static void finish_if_ready(taz_exec_t *x)
{
    taz_exec_result_t result;

    if (--x->pending > 0)
    {
        return;
    }

    (void)uv_timer_stop(&x->timer);

    result.exit_status = x->exit_status;
    result.term_signal = x->term_signal;
    result.out = x->capture.out;
    result.out_len = x->capture.out_len;
    result.err = x->capture.err;
    result.err_len = x->capture.err_len;
    result.timed_out = x->timed_out;
    result.truncated = (x->capture.truncated != 0);
    result.cancelled = x->cancelled;

    x->done = true;
    x->on_done(&result, x->arg);
    close_all(x);
}

static void on_process_exit(uv_process_t *process, int64_t exit_status,
                            int term_signal)
{
    taz_exec_t *x = (taz_exec_t *)process->data;

    x->exit_status = exit_status;
    x->term_signal = term_signal;

    if (x->on_exit != NULL)
    {
        taz_exec_on_exit_fn_t fn = x->on_exit;
        void *fn_arg = x->on_exit_arg;

        x->on_exit = NULL;
        fn((uint32_t)process->pid, x->start_key, exit_status, term_signal,
           fn_arg);
    }

    finish_if_ready(x);
}

#ifdef _WIN32
/* RegisterWaitForSingleObject's callback: runs on a system thread pool
 * thread, so it must do as little as possible and nothing that is not
 * thread-safe - just wake the loop. The real work (reading the exit code,
 * firing the on-exit hook, finish_if_ready) happens in on_exit_async below,
 * on the loop thread. */
static void CALLBACK on_process_wait(void *context, BOOLEAN timed_out)
{
    (void)timed_out;
    taz_exec_t *x = (taz_exec_t *)context;

    (void)uv_async_send(&x->exit_async);
}

/* Loop-thread counterpart of on_process_exit for the CreateProcessAsUserW
 * path: x->process.pid is already set (spawn_as_user sets it right after
 * CreateProcessAsUserW returns, since the child can exit before
 * ResumeThread even returns), so taz_exec_pid/taz_exec_set_on_exit work the
 * same as on the uv_spawn path once this fires. */
static void on_exit_async(uv_async_t *async)
{
    taz_exec_t *x = (taz_exec_t *)async->data;
    DWORD exit_code = 0;

    (void)GetExitCodeProcess(x->process_handle, &exit_code);
    x->exit_status = (int64_t)exit_code;
    x->term_signal = 0;

    /* Must run before x->process_handle is closed (on_handle_closed, once
     * finish_if_ready's close_all finishes), and never from inside
     * on_process_wait itself - this runs on the loop thread, after that
     * callback has already returned. */
    if (x->wait != NULL)
    {
        (void)UnregisterWaitEx(x->wait, NULL);
        x->wait = NULL;
    }

    if (x->on_exit != NULL)
    {
        taz_exec_on_exit_fn_t fn = x->on_exit;
        void *fn_arg = x->on_exit_arg;

        x->on_exit = NULL;
        fn((uint32_t)x->process.pid, x->start_key, x->exit_status,
           x->term_signal, fn_arg);
    }

    finish_if_ready(x);
}
#endif /* _WIN32 */

static void on_timeout(uv_timer_t *timer)
{
    taz_exec_t *x = (taz_exec_t *)timer->data;

    if (x->done)
    {
        return;
    }
    x->timed_out = true;
    tree_kill(x);
}

void taz_exec_cancel(taz_exec_t *x)
{
    if (x->done)
    {
        return;
    }
    x->cancelled = true;
    tree_kill(x);
}

uint32_t taz_exec_pid(const taz_exec_t *x)
{
    if (x->done || x->process.pid <= 0)
    {
        return 0U;
    }
    return (uint32_t)x->process.pid;
}

#ifdef _WIN32
bool taz_exec_as_user(const taz_exec_t *x)
{
    return x->as_user;
}
#else
bool taz_exec_as_user(const taz_exec_t *x)
{
    (void)x;
    return false;
}
#endif

void taz_exec_set_on_exit(taz_exec_t *x, taz_exec_on_exit_fn_t fn, void *arg)
{
    if (x->done)
    {
        return;
    }

    uint64_t start_key = 0U;

    if (x->process.pid > 0)
    {
        (void)taz_process_start_key((uint32_t)x->process.pid, &start_key);
    }

    x->start_key = start_key;
    x->on_exit = fn;
    x->on_exit_arg = arg;
}

uint64_t taz_exec_start_key(const taz_exec_t *x)
{
    return x->start_key;
}

#ifdef _WIN32
int taz_exec_signal(taz_exec_t *x, int signum)
{
    if (x->done)
    {
        return 0;
    }
    /* No POSIX signal numbering on Windows, but TASK_CANCEL still needs
     * INVALID_REQUEST for the same obviously-invalid values uv_kill/kill(2)
     * reject on POSIX, so reject out-of-range signums before ignoring the
     * (valid) value and terminating the job unconditionally. 0 is POSIX's
     * null signal, which delivers nothing, so it terminates nothing here
     * either. */
    if (signum < 0 || signum > 64)
    {
        return UV_EINVAL;
    }
    if (signum != 0 && x->job != NULL)
    {
        (void)TerminateJobObject(x->job, 1);
    }
    return 0;
}
#else
int taz_exec_signal(taz_exec_t *x, int signum)
{
    if (x->done || x->process.pid <= 0)
    {
        return 0;
    }
    return uv_kill(-x->process.pid, signum);
}
#endif

static void on_alloc(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf)
{
    (void)handle;

    size_t capped = (suggested_size <= TAZ_EXEC_ALLOC_MAX) ? suggested_size
                                                           : TAZ_EXEC_ALLOC_MAX;
    buf->base = (char *)malloc(capped);
    buf->len = (buf->base != NULL) ? (unsigned long)capped : 0UL;
}

static void on_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf,
                    taz_exec_stream_t which)
{
    taz_exec_t *x = (taz_exec_t *)stream->data;

    if (nread > 0)
    {
        taz_exec_capture_append(&x->capture, which, buf->base, (size_t)nread);
    }
    free(buf->base);

    if (nread < 0)
    {
        (void)uv_read_stop(stream);
        finish_if_ready(x);
    }
}

static void on_read_out(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf)
{
    on_read(stream, nread, buf, TAZ_EXEC_STREAM_OUT);
}

static void on_read_err(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf)
{
    on_read(stream, nread, buf, TAZ_EXEC_STREAM_ERR);
}

/* Build argv for uv_spawn: spec->file as argv[0] followed by spec->args,
 * NULL-terminated. Valid only for the duration of the uv_spawn call that
 * uses it; the caller frees it right after. */
static int build_argv(const taz_exec_spec_t *spec, char ***out_argv)
{
    char **argv = (char **)malloc((spec->args_count + 2U) * sizeof(*argv));
    if (argv == NULL)
    {
        return UV_ENOMEM;
    }

    argv[0] = (char *)spec->file;
    for (size_t i = 0U; i < spec->args_count; i++)
    {
        argv[i + 1U] = (char *)spec->args[i];
    }
    argv[spec->args_count + 1U] = NULL;

    *out_argv = argv;
    return 0;
}

static char *join_name_value(const char *name, const char *value)
{
    size_t name_len = strlen(name);
    size_t value_len = strlen(value);
    char *s = (char *)malloc(name_len + 1U + value_len + 1U);
    if (s == NULL)
    {
        return NULL;
    }

    (void)memcpy(s, name, name_len);
    s[name_len] = '=';
    (void)memcpy(s + name_len + 1U, value, value_len);
    s[name_len + 1U + value_len] = '\0';
    return s;
}

static void free_env(char **env)
{
    if (env == NULL)
    {
        return;
    }
    for (size_t i = 0U; env[i] != NULL; i++)
    {
        /* Free through a copy: MSVC /analyze treats every env[i] as one
         * location, so free(env[i]) would make the next env[i] read look
         * uninitialized (C6001). Counting first instead trips GCC 13's
         * -fanalyzer (a false malloc leak). */
        char *entry = env[i];
        free(entry);
    }
    free((void *)env);
}

void taz_exec_free_env(char **env)
{
    free_env(env);
}

/* The variables taz_exec_build_env sets for a switched identity. */
#define TAZ_EXEC_IDENTITY_ENV_COUNT 3U

/* One NAME/VALUE pair from an environment source: either the daemon's own
 * process environment (uv_os_environ, on every platform), or - on Windows,
 * when spawning as another user - that user's own environment block
 * (CreateEnvironmentBlock). Neither pointer is owned by the pair; it just
 * lets the merge below treat both sources the same way. */
typedef struct
{
    const char *name;
    const char *value;
} env_pair_t;

static int env_name_in_extra(const char *name, const taz_v1_KeyValue *extra,
                             size_t extra_count)
{
    for (size_t j = 0U; j < extra_count; j++)
    {
        if (TAZ_EXEC_ENV_NAME_CMP(name, extra[j].key) == 0)
        {
            return 1;
        }
    }
    return 0;
}

/* Merges base (identity_count later entries override a same-named base
 * entry, extra overrides both) into a NULL-terminated NAME=VALUE array.
 * identity_names/identity_values may be NULL when identity_count is 0. */
static int merge_env(const env_pair_t *base, size_t base_count,
                     const char *const *identity_names,
                     const char *const *identity_values, size_t identity_count,
                     const taz_v1_KeyValue *extra, size_t extra_count,
                     char ***out_env)
{
    char **merged = (char **)calloc(
        base_count + identity_count + extra_count + 1U, sizeof(*merged));
    if (merged == NULL)
    {
        return UV_ENOMEM;
    }

    size_t n = 0U;
    for (size_t i = 0U; i < base_count; i++)
    {
        int overridden = env_name_in_extra(base[i].name, extra, extra_count);
        for (size_t k = 0U; (overridden == 0) && (k < identity_count); k++)
        {
            if (TAZ_EXEC_ENV_NAME_CMP(base[i].name, identity_names[k]) == 0)
            {
                overridden = 1;
            }
        }
        if (overridden)
        {
            continue;
        }

        merged[n] = join_name_value(base[i].name, base[i].value);
        if (merged[n] == NULL)
        {
            free_env(merged);
            return UV_ENOMEM;
        }
        n++;
    }

    for (size_t k = 0U; k < identity_count; k++)
    {
        if (env_name_in_extra(identity_names[k], extra, extra_count))
        {
            continue;
        }
        merged[n] = join_name_value(identity_names[k], identity_values[k]);
        if (merged[n] == NULL)
        {
            free_env(merged);
            return UV_ENOMEM;
        }
        n++;
    }

    for (size_t j = 0U; j < extra_count; j++)
    {
        merged[n] = join_name_value(extra[j].key, extra[j].value);
        if (merged[n] == NULL)
        {
            free_env(merged);
            return UV_ENOMEM;
        }
        n++;
    }
    merged[n] = NULL;

    *out_env = merged;
    return 0;
}

#ifdef _WIN32
/* Converts one entry ("NAME=VALUE", or a handful of pseudo-variables whose
 * own name starts with '=' - the per-drive cwd entries, "=ExitCode" - that
 * libuv's uv_os_environ skips the same way) from a double-NUL-terminated
 * wide multi-string into a single malloc'd, NUL-split UTF-8 buffer: *out
 * points at NAME, *out_value at VALUE, both inside that one allocation.
 * Returns UV_EINVAL (not a real error, just "skip this entry") when there
 * is no '=' to split on. */
static int wide_env_entry_to_utf8(const WCHAR *entry, char **out,
                                  const char **out_value)
{
    int needed =
        WideCharToMultiByte(CP_UTF8, 0, entry, -1, NULL, 0, NULL, NULL);
    if (needed <= 0)
    {
        return uv_translate_sys_error((int)GetLastError());
    }

    char *buf = (char *)malloc((size_t)needed);
    if (buf == NULL)
    {
        return UV_ENOMEM;
    }
    (void)WideCharToMultiByte(CP_UTF8, 0, entry, -1, buf, needed, NULL, NULL);

    /* Search from offset 1: those pseudo-variables have a name that itself
     * starts with '=', so the first '=' in the string is not the
     * separator. */
    char *eq = strchr(buf + 1, '=');
    if (eq == NULL)
    {
        free(buf);
        return UV_EINVAL;
    }
    *eq = '\0';

    *out = buf;
    *out_value = eq + 1;
    return 0;
}

/* The target's own environment (table S2): CreateEnvironmentBlock(token,
 * FALSE) - never the daemon's own uv_os_environ - with no USER/LOGNAME/
 * HOME trio (those are POSIX-only; the target's own profile sets its
 * equivalents), request env on top. */
static int build_env_from_token(void *token, const taz_v1_KeyValue *extra,
                                size_t extra_count, char ***out_env)
{
    LPVOID block = NULL;
    if (!CreateEnvironmentBlock(&block, (HANDLE)token, FALSE))
    {
        return uv_translate_sys_error((int)GetLastError());
    }

    size_t count = 0U;
    for (const WCHAR *p = (const WCHAR *)block; *p != L'\0';
         p += wcslen(p) + 1U)
    {
        count++;
    }

    /* +1 so pairs/owned are never NULL when count is 0 - see the matching
     * comment in taz_exec_build_env. */
    env_pair_t *pairs = (env_pair_t *)calloc(count + 1U, sizeof(*pairs));
    char **owned = (char **)calloc(count + 1U, sizeof(*owned));
    if ((pairs == NULL) || (owned == NULL))
    {
        free(pairs);
        free((void *)owned);
        (void)DestroyEnvironmentBlock(block);
        return UV_ENOMEM;
    }

    size_t n = 0U;
    int rc = 0;
    for (const WCHAR *p = (const WCHAR *)block; *p != L'\0';
         p += wcslen(p) + 1U)
    {
        char *name = NULL;
        const char *value = NULL;
        int entry_rc = wide_env_entry_to_utf8(p, &name, &value);
        if (entry_rc == UV_EINVAL)
        {
            continue; /* no '=': not a real entry, skip it */
        }
        if (entry_rc != 0)
        {
            rc = entry_rc;
            break;
        }

        /* Can't happen: this loop walks the same multi-string the count
         * above already walked once, so it fills at most `count` slots.
         * MSVC /analyze cannot see that across the two loops (C6386), so
         * spell it out. */
        if (n >= count)
        {
            free(name);
            break;
        }

        owned[n] = name;
        pairs[n].name = name;
        pairs[n].value = value;
        n++;
    }

    if (rc == 0)
    {
        rc = merge_env(pairs, n, NULL, NULL, 0U, extra, extra_count, out_env);
    }

    for (size_t i = 0U; i < n; i++)
    {
        /* Free through a copy, same reason as free_env. */
        char *entry = owned[i];
        free(entry);
    }
    free((void *)owned);
    free(pairs);
    (void)DestroyEnvironmentBlock(block);
    return rc;
}
#endif /* _WIN32 */

int taz_exec_build_env(const taz_exec_spec_t *spec, char ***out_env)
{
    static const char *const identity_names[TAZ_EXEC_IDENTITY_ENV_COUNT] = {
        "USER", "LOGNAME", "HOME"};
    const char *identity_values[TAZ_EXEC_IDENTITY_ENV_COUNT] = {NULL, NULL,
                                                                NULL};
    size_t identity_count = 0U;
    const taz_v1_KeyValue *extra = spec->env;
    size_t extra_count = spec->env_count;

    if (spec->switch_identity)
    {
        identity_values[0] = spec->identity_user;
        identity_values[1] = spec->identity_user;
        identity_values[2] =
            (spec->identity_home[0] != '\0') ? spec->identity_home : "/";
        identity_count = TAZ_EXEC_IDENTITY_ENV_COUNT;
    }

#ifdef _WIN32
    if (spec->token != NULL)
    {
        return build_env_from_token(spec->token, extra, extra_count, out_env);
    }
#endif

    uv_env_item_t *base = NULL;
    int base_count = 0;
    int rc = uv_os_environ(&base, &base_count);
    if (rc != 0)
    {
        return rc;
    }

    /* +1 and calloc (not malloc) so pairs is never NULL when base_count is
     * 0 and every slot starts zeroed - GCC 13's -fanalyzer (unlike local
     * GCC 16) cannot otherwise tell, across the merge_env call below, that
     * a NULL pairs always pairs with a zero count, or that the loop below
     * always fills indices [0, base_count) before merge_env reads them. */
    env_pair_t *pairs =
        (env_pair_t *)calloc((size_t)base_count + 1U, sizeof(*pairs));
    if (pairs == NULL)
    {
        uv_os_free_environ(base, base_count);
        return UV_ENOMEM;
    }
    for (int i = 0; i < base_count; i++)
    {
        pairs[i].name = base[i].name;
        pairs[i].value = base[i].value;
    }

    rc = merge_env(pairs, (size_t)base_count, identity_names, identity_values,
                   identity_count, extra, extra_count, out_env);

    free(pairs);
    uv_os_free_environ(base, base_count);
    return rc;
}

#ifdef _WIN32
/* Create a Job Object that kills every process assigned to it as soon as
 * its last handle closes, so tree_kill only has to close one handle to take
 * down the whole tree. The child is assigned to *out_job right after
 * uv_spawn (taz_exec_start), since libuv offers no way to spawn suspended. */
static int create_job_object(HANDLE *out_job)
{
    HANDLE job = CreateJobObjectW(NULL, NULL);
    if (job == NULL)
    {
        return uv_translate_sys_error((int)GetLastError());
    }

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info;
    (void)memset(&info, 0, sizeof(info));
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info,
                                 sizeof(info)))
    {
        int rc = uv_translate_sys_error((int)GetLastError());
        (void)CloseHandle(job);
        return rc;
    }

    *out_job = job;
    return 0;
}
#endif

void taz_exec_apply_identity(uv_process_options_t *options,
                             const taz_exec_spec_t *spec)
{
#ifndef _WIN32
    if (spec->switch_identity)
    {
        options->uid = spec->uid;
        options->gid = spec->gid;
        options->flags |= UV_PROCESS_SETUID | UV_PROCESS_SETGID;
    }
#else
    (void)options;
    (void)spec;
#endif
}

#ifdef _WIN32

/* Windows' own hard limit for a CreateProcess command line, matching
 * taz_platform_reexec's TAZ_REEXEC_CMDLINE_MAX. Heap, not stack, for the
 * same MSVC /analyze C6262 large-stack-frame reason that constant is heap
 * allocated there. */
#define TAZ_EXEC_CMDLINE_MAX 32768U

/* Converts a UTF-8 NUL-terminated string into a heap wchar_t buffer. NULL
 * and *out_rc a UV_E* code on failure. */
static wchar_t *utf8_to_wide(const char *s, int *out_rc)
{
    int needed = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (needed <= 0)
    {
        *out_rc = uv_translate_sys_error((int)GetLastError());
        return NULL;
    }

    wchar_t *w = (wchar_t *)malloc((size_t)needed * sizeof(wchar_t));
    if (w == NULL)
    {
        *out_rc = UV_ENOMEM;
        return NULL;
    }
    (void)MultiByteToWideChar(CP_UTF8, 0, s, -1, w, needed);
    *out_rc = 0;
    return w;
}

/* The value half of the first entry in envp (UTF-8 "NAME=VALUE", as built by
 * taz_exec_build_env) whose name matches name case-insensitively, or NULL.
 * Points into envp; not owned. */
static const char *find_env_value(char *const *envp, const char *name)
{
    size_t len = strlen(name);
    for (size_t i = 0U; envp[i] != NULL; i++)
    {
        if ((_strnicmp(envp[i], name, len) == 0) && (envp[i][len] == '='))
        {
            return envp[i] + len + 1U;
        }
    }
    return NULL;
}

/* Converts envp (UTF-8 "NAME=VALUE", NULL-terminated, as built by
 * taz_exec_build_env) into a Windows environment block for
 * CreateProcessAsUserW's lpEnvironment: every entry's wide form back to
 * back, each NUL-terminated, the whole block additionally NUL-terminated
 * (a double NUL at the end, including the empty-envp case). This is never a
 * block from CreateEnvironmentBlock (taz_exec_build_env already consumed
 * and destroyed that one) - just a plain heap buffer, freed with free(). */
static int build_wide_env_block(char *const *envp, wchar_t **out_block)
{
    size_t count = 0U;
    for (; envp[count] != NULL; count++)
    {
    }

    wchar_t **wide = NULL;
    if (count > 0U)
    {
        wide = (wchar_t **)calloc(count, sizeof(*wide));
        if (wide == NULL)
        {
            return UV_ENOMEM;
        }
    }

    int rc = 0;
    size_t converted = 0U;
    size_t total = 0U;
    for (; converted < count; converted++)
    {
        wide[converted] = utf8_to_wide(envp[converted], &rc);
        if (wide[converted] == NULL)
        {
            break;
        }
        total += wcslen(wide[converted]) + 1U;
    }

    wchar_t *block = NULL;
    if (rc == 0)
    {
        block = (wchar_t *)malloc((total + 1U) * sizeof(wchar_t));
        if (block == NULL)
        {
            rc = UV_ENOMEM;
        }
    }
    if (rc == 0)
    {
        size_t pos = 0U;
        for (size_t i = 0U; i < converted; i++)
        {
            size_t len = wcslen(wide[i]) + 1U;
            (void)memcpy(block + pos, wide[i], len * sizeof(wchar_t));
            pos += len;
        }
        block[pos] = L'\0';
    }

    for (size_t i = 0U; i < converted; i++)
    {
        /* Free through a copy, same reason as free_env. */
        wchar_t *entry = wide[i];
        free(entry);
    }
    free((void *)wide);

    if (rc == 0)
    {
        *out_block = block;
    }
    return rc;
}

/* Table S9: resolves file_w against path_w (the child's own PATH, extracted
 * from its own env block by the caller - never the daemon's) the same way
 * SearchPathW always does: a bare name is searched along path_w (or the
 * current directory alone when path_w is NULL), a name that already
 * contains a path is used as is. Not found -> UV_ENOENT. */
static int search_path_as_user(const wchar_t *file_w, const wchar_t *path_w,
                               wchar_t **out_resolved)
{
    DWORD needed = SearchPathW(path_w, file_w, L".exe", 0, NULL, NULL);
    if (needed == 0U)
    {
        return uv_translate_sys_error((int)GetLastError());
    }

    wchar_t *buf = (wchar_t *)malloc((size_t)(needed + 1U) * sizeof(wchar_t));
    if (buf == NULL)
    {
        return UV_ENOMEM;
    }

    DWORD written =
        SearchPathW(path_w, file_w, L".exe", needed + 1U, buf, NULL);
    if (written == 0U || written > needed)
    {
        int rc = (written == 0U) ? uv_translate_sys_error((int)GetLastError())
                                 : UV_E2BIG;
        free(buf);
        return rc;
    }

    *out_resolved = buf;
    return 0;
}

static LONG g_exec_pipe_serial = 0;

/* One end of a pipe the child will write into and the daemon will read from
 * (table S2): *daemon_end is the overlapped server end, never inheritable,
 * wrapped with uv_pipe_open by the caller; *child_end is a plain,
 * inheritable client end, closed by the caller as soon as
 * CreateProcessAsUserW has its own copy. */
static int make_output_pipe(HANDLE *daemon_end, HANDLE *child_end)
{
    wchar_t name[64];
    LONG serial = InterlockedIncrement(&g_exec_pipe_serial);

    (void)swprintf(name, sizeof(name) / sizeof(name[0]),
                   L"\\\\.\\pipe\\taz-exec-%lu-%ld",
                   (unsigned long)GetCurrentProcessId(), (long)serial);

    HANDLE server =
        CreateNamedPipeW(name,
                         PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED |
                             FILE_FLAG_FIRST_PIPE_INSTANCE,
                         PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
                             PIPE_REJECT_REMOTE_CLIENTS,
                         1, 0, 65536, 0, NULL);
    if (server == INVALID_HANDLE_VALUE)
    {
        return uv_translate_sys_error((int)GetLastError());
    }

    HANDLE client = CreateFileW(name, GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, NULL);
    if (client == INVALID_HANDLE_VALUE)
    {
        int rc = uv_translate_sys_error((int)GetLastError());
        (void)CloseHandle(server);
        return rc;
    }

    if (!SetHandleInformation(client, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT))
    {
        int rc = uv_translate_sys_error((int)GetLastError());
        (void)CloseHandle(server);
        (void)CloseHandle(client);
        return rc;
    }

    *daemon_end = server;
    *child_end = client;
    return 0;
}

/* The child's stdin (table S2): an inheritable, read-only handle on NUL -
 * the daemon never writes to the child, so there is no daemon-side pipe to
 * wrap here, unlike the output streams above. */
static int make_null_stdin(HANDLE *out)
{
    HANDLE h =
        CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
    {
        return uv_translate_sys_error((int)GetLastError());
    }
    if (!SetHandleInformation(h, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT))
    {
        int rc = uv_translate_sys_error((int)GetLastError());
        (void)CloseHandle(h);
        return rc;
    }
    *out = h;
    return 0;
}

/* stdin, stdout, stderr: the only handles PROC_THREAD_ATTRIBUTE_HANDLE_LIST
 * is ever given below. */
#define TAZ_EXEC_STD_HANDLE_COUNT 3U

/* The second spawn path (table S2): spec->token != NULL. Takes ownership of
 * spec->token into x (x->as_user, x->token) as its very first action after
 * x->exit_async itself is live - from that point on, every failure in here
 * is reported the same way a uv_spawn failure is on the plain path: the
 * caller's close_all tears x down, closing whatever of job/token/profile/
 * process_handle/wait this function got around to setting. Only resources
 * that never made it into x (the four local-only handles, the various
 * wide-string/environment-block conversions) are this function's own job to
 * release, in the `fail` block below. argv is only read here, never
 * modified or freed - the caller owns it. */
static int spawn_as_user(uv_loop_t *loop, const taz_exec_spec_t *spec,
                         taz_exec_t *x, char *const *argv)
{
    wchar_t *user_w = NULL;
    char **envp = NULL;
    wchar_t *path_w = NULL;
    wchar_t *file_w = NULL;
    wchar_t *app_w = NULL;
    char *cmdline_utf8 = NULL;
    wchar_t *cmdline_w = NULL;
    wchar_t *env_block_w = NULL;
    wchar_t *cwd_w = NULL;
    HANDLE stdin_h = NULL;
    HANDLE out_daemon = NULL;
    HANDLE out_child = NULL;
    HANDLE err_daemon = NULL;
    HANDLE err_child = NULL;
    int out_fd = -1;
    int err_fd = -1;
    LPPROC_THREAD_ATTRIBUTE_LIST attr_list = NULL;
    SIZE_T attr_list_size = 0U;
    /* Must stay valid from UpdateProcThreadAttribute through
     * DeleteProcThreadAttributeList (MS docs): a nested block that closed
     * before that call let the compiler reuse this stack slot. */
    HANDLE handles[TAZ_EXEC_STD_HANDLE_COUNT];
    PROCESS_INFORMATION pi;
    STARTUPINFOEXW si;
    PROFILEINFOW profile_info;
    BOOL created = FALSE;
    int rc;

    (void)memset(&pi, 0, sizeof(pi));
    (void)memset(&si, 0, sizeof(si));
    (void)memset(&profile_info, 0, sizeof(profile_info));

    rc = uv_async_init(loop, &x->exit_async, on_exit_async);
    if (rc != 0)
    {
        return rc;
    }
    x->exit_async.data = x;
    x->as_user = true;
    x->token = (HANDLE)spec->token;

    user_w = utf8_to_wide(spec->identity_user, &rc);
    if (user_w == NULL)
    {
        goto fail;
    }
    profile_info.dwSize = sizeof(profile_info);
    profile_info.dwFlags = PI_NOUI;
    profile_info.lpUserName = user_w;
    if (!LoadUserProfileW(x->token, &profile_info))
    {
        rc = uv_translate_sys_error((int)GetLastError());
        goto fail;
    }
    x->profile = profile_info.hProfile;
    free(user_w);
    user_w = NULL;

    rc = taz_exec_build_env(spec, &envp);
    if (rc != 0)
    {
        envp = NULL;
        goto fail;
    }
    if (envp == NULL)
    {
        /* Can't happen: merge_env always sets *out_env on a 0 return, and
         * every taz_exec_build_env path returns 0 only via merge_env - the
         * analyzer can't see that across two calls. */
        rc = UV_EINVAL;
        goto fail;
    }

    {
        const char *path_value = find_env_value(envp, "PATH");
        if (path_value != NULL)
        {
            path_w = utf8_to_wide(path_value, &rc);
            if (path_w == NULL)
            {
                goto fail;
            }
        }
    }

    file_w = utf8_to_wide(spec->file, &rc);
    if (file_w == NULL)
    {
        goto fail;
    }
    rc = search_path_as_user(file_w, path_w, &app_w);
    if (rc != 0)
    {
        goto fail;
    }
    free(file_w);
    file_w = NULL;
    free(path_w);
    path_w = NULL;

    cmdline_utf8 = (char *)malloc(TAZ_EXEC_CMDLINE_MAX);
    cmdline_w = (wchar_t *)malloc(TAZ_EXEC_CMDLINE_MAX * sizeof(wchar_t));
    if (cmdline_utf8 == NULL || cmdline_w == NULL)
    {
        rc = UV_ENOMEM;
        goto fail;
    }
    if (taz_cmdline_build(argv, cmdline_utf8, TAZ_EXEC_CMDLINE_MAX) != 0)
    {
        rc = UV_E2BIG;
        goto fail;
    }
    if (MultiByteToWideChar(CP_UTF8, 0, cmdline_utf8, -1, cmdline_w,
                            (int)TAZ_EXEC_CMDLINE_MAX) == 0)
    {
        rc = uv_translate_sys_error((int)GetLastError());
        goto fail;
    }
    free(cmdline_utf8);
    cmdline_utf8 = NULL;

    rc = build_wide_env_block(envp, &env_block_w);
    if (rc != 0)
    {
        goto fail;
    }
    taz_exec_free_env(envp);
    envp = NULL;

    if (spec->cwd != NULL && spec->cwd[0] != '\0')
    {
        cwd_w = utf8_to_wide(spec->cwd, &rc);
        if (cwd_w == NULL)
        {
            goto fail;
        }
    }

    rc = make_null_stdin(&stdin_h);
    if (rc != 0)
    {
        goto fail;
    }
    rc = make_output_pipe(&out_daemon, &out_child);
    if (rc != 0)
    {
        goto fail;
    }
    rc = make_output_pipe(&err_daemon, &err_child);
    if (rc != 0)
    {
        goto fail;
    }

    /* uv_pipe_open takes ownership of the CRT fd from _open_osfhandle: from
     * here on the daemon-side HANDLE is released through the fd (_close) or
     * through the uv_pipe_t (uv_close), never through a direct CloseHandle -
     * doing both would double-close it. */
    out_fd = _open_osfhandle((intptr_t)out_daemon, 0);
    if (out_fd < 0)
    {
        rc = UV_EINVAL;
        goto fail;
    }
    out_daemon = NULL;
    rc = uv_pipe_open(&x->out_pipe, out_fd);
    if (rc != 0)
    {
        goto fail;
    }
    out_fd = -1;

    err_fd = _open_osfhandle((intptr_t)err_daemon, 0);
    if (err_fd < 0)
    {
        rc = UV_EINVAL;
        goto fail;
    }
    err_daemon = NULL;
    rc = uv_pipe_open(&x->err_pipe, err_fd);
    if (rc != 0)
    {
        goto fail;
    }
    err_fd = -1;

    InitializeProcThreadAttributeList(NULL, 1, 0, &attr_list_size);
    if (attr_list_size == 0U)
    {
        rc = uv_translate_sys_error((int)GetLastError());
        goto fail;
    }
    attr_list = (LPPROC_THREAD_ATTRIBUTE_LIST)HeapAlloc(GetProcessHeap(), 0,
                                                        attr_list_size);
    if (attr_list == NULL)
    {
        rc = UV_ENOMEM;
        goto fail;
    }
    if (!InitializeProcThreadAttributeList(attr_list, 1, 0, &attr_list_size))
    {
        rc = uv_translate_sys_error((int)GetLastError());
        goto fail;
    }

    /* Exactly these three, so no other inheritable handle in this
     * process (there are none right now, but a future caller's choice
     * is not this function's to make) rides along. */
    handles[0] = stdin_h;
    handles[1] = out_child;
    handles[2] = err_child;
    if (!UpdateProcThreadAttribute(
            attr_list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, (void *)handles,
            sizeof(handles), NULL, NULL))
    {
        rc = uv_translate_sys_error((int)GetLastError());
        goto fail;
    }

    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = stdin_h;
    si.StartupInfo.hStdOutput = out_child;
    si.StartupInfo.hStdError = err_child;
    si.StartupInfo.lpDesktop = L"";
    si.lpAttributeList = attr_list;

    {
        DWORD flags = CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT |
                      CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT;
        created = CreateProcessAsUserW(x->token, app_w, cmdline_w, NULL, NULL,
                                       TRUE, flags, env_block_w, cwd_w,
                                       &si.StartupInfo, &pi);
        if (!created)
        {
            rc = uv_translate_sys_error((int)GetLastError());
        }
    }

    /* Needed on neither outcome from here on: attr_list and the three child
     * handle copies are the daemon's own, separate from whatever the child
     * inherited on success. */
    DeleteProcThreadAttributeList(attr_list);
    HeapFree(GetProcessHeap(), 0, attr_list);
    attr_list = NULL;
    (void)CloseHandle(stdin_h);
    stdin_h = NULL;
    (void)CloseHandle(out_child);
    out_child = NULL;
    (void)CloseHandle(err_child);
    err_child = NULL;
    free(cmdline_w);
    cmdline_w = NULL;
    free(app_w);
    app_w = NULL;
    free(env_block_w);
    env_block_w = NULL;
    free(cwd_w);
    cwd_w = NULL;

    if (!created)
    {
        return rc;
    }

    /* Set before ResumeThread: the child can exit before that call even
     * returns, and taz_exec_pid/taz_exec_set_on_exit must see the real pid
     * regardless. */
    x->process.pid = (int)pi.dwProcessId;
    x->process_handle = pi.hProcess;

    if (!AssignProcessToJobObject(x->job, pi.hProcess))
    {
        /* Unlike the uv_spawn path (which cannot stop a child once spawned),
         * the child is still suspended here: no child ever runs outside the
         * job (S6). x->process_handle is already set, so the normal
         * teardown (close_all -> on_handle_closed) still closes it. */
        rc = uv_translate_sys_error((int)GetLastError());
        (void)TerminateProcess(pi.hProcess, 1U);
        (void)CloseHandle(pi.hThread);
        return rc;
    }

    if (!RegisterWaitForSingleObject(&x->wait, pi.hProcess, on_process_wait, x,
                                     INFINITE, WT_EXECUTEONLYONCE))
    {
        rc = uv_translate_sys_error((int)GetLastError());
        (void)TerminateJobObject(x->job, 1);
        (void)ResumeThread(pi.hThread);
        (void)CloseHandle(pi.hThread);
        return rc;
    }

    (void)ResumeThread(pi.hThread);
    (void)CloseHandle(pi.hThread);
    return 0;

fail:
    free(user_w);
    taz_exec_free_env(envp);
    free(path_w);
    free(file_w);
    free(app_w);
    free(cmdline_utf8);
    free(cmdline_w);
    free(env_block_w);
    free(cwd_w);
    if (attr_list != NULL)
    {
        DeleteProcThreadAttributeList(attr_list);
        HeapFree(GetProcessHeap(), 0, attr_list);
    }
    if (stdin_h != NULL)
    {
        (void)CloseHandle(stdin_h);
    }
    if (out_child != NULL)
    {
        (void)CloseHandle(out_child);
    }
    if (err_child != NULL)
    {
        (void)CloseHandle(err_child);
    }
    if (out_fd >= 0)
    {
        (void)_close(out_fd);
    }
    else if (out_daemon != NULL)
    {
        (void)CloseHandle(out_daemon);
    }
    if (err_fd >= 0)
    {
        (void)_close(err_fd);
    }
    else if (err_daemon != NULL)
    {
        (void)CloseHandle(err_daemon);
    }
    return rc;
}

#endif /* _WIN32 */

int taz_exec_start(uv_loop_t *loop, const taz_exec_spec_t *spec,
                   taz_exec_done_fn_t on_done, void *arg, taz_exec_t **out)
{
#ifndef _WIN32
    /* On POSIX there is no token-aware spawn path: refuse and close it
     * right away, on every platform this is ever reached on. */
    if (spec->token != NULL)
    {
        taz_run_as_token_close(spec->token);
        return UV_ENOTSUP;
    }
#endif

    taz_exec_t *x = (taz_exec_t *)calloc(1U, sizeof(*x));
    if (x == NULL)
    {
#ifdef _WIN32
        taz_run_as_token_close(spec->token);
#endif
        return UV_ENOMEM;
    }
    x->on_done = on_done;
    x->arg = arg;
    x->pending = TAZ_EXEC_PENDING_COUNT;
    taz_exec_capture_init(&x->capture, spec->max_output_bytes);

    int rc = uv_pipe_init(loop, &x->out_pipe, 0);
    if (rc != 0)
    {
#ifdef _WIN32
        taz_run_as_token_close(spec->token);
#endif
        taz_exec_capture_free(&x->capture);
        free(x);
        return rc;
    }
    x->out_pipe.data = x;

    rc = uv_pipe_init(loop, &x->err_pipe, 0);
    if (rc != 0)
    {
#ifdef _WIN32
        taz_run_as_token_close(spec->token);
#endif
        x->closing = 1;
        uv_close((uv_handle_t *)&x->out_pipe, on_handle_closed);
        return rc;
    }
    x->err_pipe.data = x;

    rc = uv_timer_init(loop, &x->timer);
    if (rc != 0)
    {
#ifdef _WIN32
        taz_run_as_token_close(spec->token);
#endif
        x->closing = 2;
        uv_close((uv_handle_t *)&x->out_pipe, on_handle_closed);
        uv_close((uv_handle_t *)&x->err_pipe, on_handle_closed);
        return rc;
    }
    x->timer.data = x;

    char **argv = NULL;
    rc = build_argv(spec, &argv);
#ifdef _WIN32
    if (rc == 0)
    {
        rc = create_job_object(&x->job);
    }
#endif
    if (rc != 0)
    {
#ifdef _WIN32
        taz_run_as_token_close(spec->token);
#endif
        free((void *)argv);
        x->closing = TAZ_EXEC_PRESPAWN_HANDLE_COUNT;
        uv_close((uv_handle_t *)&x->out_pipe, on_handle_closed);
        uv_close((uv_handle_t *)&x->err_pipe, on_handle_closed);
        uv_close((uv_handle_t *)&x->timer, on_handle_closed);
        return rc;
    }

#ifdef _WIN32
    if (spec->token != NULL)
    {
        rc = spawn_as_user(loop, spec, x, argv);
        free((void *)argv);
        if (rc != 0)
        {
            if (x->as_user)
            {
                /* spawn_as_user already parked the token/profile/process
                 * handle it acquired into x; close_all's as_user branch
                 * (exit_async is live by now too) tears all of it down. */
                close_all(x);
            }
            else
            {
                /* The only spawn_as_user failure before it takes ownership
                 * of spec->token: its own uv_async_init call. */
                taz_run_as_token_close(spec->token);
                x->closing = TAZ_EXEC_PRESPAWN_HANDLE_COUNT;
                uv_close((uv_handle_t *)&x->out_pipe, on_handle_closed);
                uv_close((uv_handle_t *)&x->err_pipe, on_handle_closed);
                uv_close((uv_handle_t *)&x->timer, on_handle_closed);
            }
            return rc;
        }

        (void)uv_read_start((uv_stream_t *)&x->out_pipe, on_alloc, on_read_out);
        (void)uv_read_start((uv_stream_t *)&x->err_pipe, on_alloc, on_read_err);
        if (spec->timeout_ms != 0U)
        {
            (void)uv_timer_start(&x->timer, on_timeout, spec->timeout_ms, 0U);
        }

        *out = x;
        return 0;
    }
#endif

    char **envp = NULL;
    rc = taz_exec_build_env(spec, &envp);
    if (rc != 0)
    {
        free((void *)argv);
        x->closing = TAZ_EXEC_PRESPAWN_HANDLE_COUNT;
        uv_close((uv_handle_t *)&x->out_pipe, on_handle_closed);
        uv_close((uv_handle_t *)&x->err_pipe, on_handle_closed);
        uv_close((uv_handle_t *)&x->timer, on_handle_closed);
        return rc;
    }

    uv_stdio_container_t stdio[TAZ_EXEC_STDIO_COUNT];
    stdio[0].flags = UV_IGNORE;
    stdio[1].flags = UV_CREATE_PIPE | UV_WRITABLE_PIPE;
    stdio[1].data.stream = (uv_stream_t *)&x->out_pipe;
    stdio[2].flags = UV_CREATE_PIPE | UV_WRITABLE_PIPE;
    stdio[2].data.stream = (uv_stream_t *)&x->err_pipe;

    uv_process_options_t options;
    (void)memset(&options, 0, sizeof(options));
    options.exit_cb = on_process_exit;
    options.file = spec->file;
    options.args = argv;
    options.env = envp;
    options.cwd =
        (spec->cwd != NULL && spec->cwd[0] != '\0') ? spec->cwd : NULL;
    options.stdio_count = TAZ_EXEC_STDIO_COUNT;
    options.stdio = stdio;
#ifdef _WIN32
    options.flags = UV_PROCESS_WINDOWS_HIDE;
#else
    options.flags = UV_PROCESS_DETACHED;
#endif
    taz_exec_apply_identity(&options, spec);

    x->process.data = x;
    rc = uv_spawn(loop, &x->process, &options);

    free_env(envp);
    free((void *)argv);

    if (rc != 0)
    {
        close_all(x);
        return rc;
    }

#ifdef _WIN32
    if (!AssignProcessToJobObject(x->job, x->process.process_handle))
    {
        /* The child escaped containment (e.g. nested jobs unsupported);
         * TerminateJobObject can no longer help, so take down at least the
         * direct child. The exec still completes normally through the exit
         * callback once it does. */
        (void)TerminateProcess(x->process.process_handle, 1U);
    }
#endif

    (void)uv_read_start((uv_stream_t *)&x->out_pipe, on_alloc, on_read_out);
    (void)uv_read_start((uv_stream_t *)&x->err_pipe, on_alloc, on_read_err);

    if (spec->timeout_ms != 0U)
    {
        (void)uv_timer_start(&x->timer, on_timeout, spec->timeout_ms, 0U);
    }

    *out = x;
    return 0;
}
