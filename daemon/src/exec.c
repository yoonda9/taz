#include "taz/exec.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
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

struct taz_exec_s
{
    uv_process_t process;
    uv_pipe_t out_pipe;
    uv_pipe_t err_pipe;
    uv_timer_t timer;
#ifdef _WIN32
    /* Per-exec Job Object the child is assigned to right after uv_spawn, so
     * that killing the job (KILL_ON_JOB_CLOSE) takes the whole tree with it.
     * NULL if creation failed before a process was ever spawned. */
    HANDLE job;
#endif
    taz_exec_capture_t capture;
    taz_exec_done_fn on_done;
    void *arg;
    int pending; /* counts down from TAZ_EXEC_PENDING_COUNT to 0 */
    int closing; /* counts down from TAZ_EXEC_HANDLE_COUNT to 0 */
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
#endif
    taz_exec_capture_free(&x->capture);
    free(x);
}

/* Close every handle belonging to x. Safe to call for a spawn that never
 * started (uv_spawn always initializes the process handle before it can
 * fail) as well as for one that ran to completion; on_handle_closed frees x
 * once all close callbacks have fired. */
static void close_all(taz_exec_t *x)
{
    x->closing = TAZ_EXEC_HANDLE_COUNT;
    uv_close((uv_handle_t *)&x->process, on_handle_closed);
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
    finish_if_ready(x);
}

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

static void on_alloc(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf)
{
    (void)handle;

    buf->base = (char *)malloc(suggested_size);
    buf->len = (buf->base != NULL) ? suggested_size : 0U;
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
        free(env[i]);
    }
    free(env);
}

/* Build the NAME=VALUE environment for the child: the daemon's own
 * environment (from uv_os_environ) with extra/env entries added or, on a
 * name match (case-insensitive on Windows), overridden. The uv_os_environ
 * snapshot is freed here; the returned char ** is owned by the caller
 * (free_env). */
static int build_env(const taz_v1_KeyValue *extra, size_t extra_count,
                     char ***out_env)
{
    uv_env_item_t *base = NULL;
    int base_count = 0;
    int rc = uv_os_environ(&base, &base_count);
    if (rc != 0)
    {
        return rc;
    }

    char **merged =
        (char **)calloc((size_t)base_count + extra_count + 1U, sizeof(*merged));
    if (merged == NULL)
    {
        uv_os_free_environ(base, base_count);
        return UV_ENOMEM;
    }

    size_t n = 0U;
    for (int i = 0; i < base_count; i++)
    {
        int overridden = 0;
        for (size_t j = 0U; j < extra_count; j++)
        {
            if (TAZ_EXEC_ENV_NAME_CMP(base[i].name, extra[j].key) == 0)
            {
                overridden = 1;
                break;
            }
        }
        if (overridden)
        {
            continue;
        }

        merged[n] = join_name_value(base[i].name, base[i].value);
        if (merged[n] == NULL)
        {
            uv_os_free_environ(base, base_count);
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
            uv_os_free_environ(base, base_count);
            free_env(merged);
            return UV_ENOMEM;
        }
        n++;
    }
    merged[n] = NULL;

    uv_os_free_environ(base, base_count);
    *out_env = merged;
    return 0;
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

int taz_exec_start(uv_loop_t *loop, const taz_exec_spec_t *spec,
                   taz_exec_done_fn on_done, void *arg, taz_exec_t **out)
{
    taz_exec_t *x = (taz_exec_t *)calloc(1U, sizeof(*x));
    if (x == NULL)
    {
        return UV_ENOMEM;
    }
    x->on_done = on_done;
    x->arg = arg;
    x->pending = TAZ_EXEC_PENDING_COUNT;
    taz_exec_capture_init(&x->capture, spec->max_output_bytes);

    int rc = uv_pipe_init(loop, &x->out_pipe, 0);
    if (rc != 0)
    {
        taz_exec_capture_free(&x->capture);
        free(x);
        return rc;
    }
    x->out_pipe.data = x;

    rc = uv_pipe_init(loop, &x->err_pipe, 0);
    if (rc != 0)
    {
        x->closing = 1;
        uv_close((uv_handle_t *)&x->out_pipe, on_handle_closed);
        return rc;
    }
    x->err_pipe.data = x;

    rc = uv_timer_init(loop, &x->timer);
    if (rc != 0)
    {
        x->closing = 2;
        uv_close((uv_handle_t *)&x->out_pipe, on_handle_closed);
        uv_close((uv_handle_t *)&x->err_pipe, on_handle_closed);
        return rc;
    }
    x->timer.data = x;

    char **argv = NULL;
    char **envp = NULL;
    rc = build_argv(spec, &argv);
    if (rc == 0)
    {
        rc = build_env(spec->env, spec->env_count, &envp);
    }
#ifdef _WIN32
    if (rc == 0)
    {
        rc = create_job_object(&x->job);
    }
#endif
    if (rc != 0)
    {
        free_env(envp);
        free(argv);
        x->closing = 3;
        uv_close((uv_handle_t *)&x->out_pipe, on_handle_closed);
        uv_close((uv_handle_t *)&x->err_pipe, on_handle_closed);
        uv_close((uv_handle_t *)&x->timer, on_handle_closed);
        return rc;
    }

    uv_stdio_container_t stdio[3];
    stdio[0].flags = UV_IGNORE;
    stdio[1].flags = (uv_stdio_flags)(UV_CREATE_PIPE | UV_WRITABLE_PIPE);
    stdio[1].data.stream = (uv_stream_t *)&x->out_pipe;
    stdio[2].flags = (uv_stdio_flags)(UV_CREATE_PIPE | UV_WRITABLE_PIPE);
    stdio[2].data.stream = (uv_stream_t *)&x->err_pipe;

    uv_process_options_t options;
    (void)memset(&options, 0, sizeof(options));
    options.exit_cb = on_process_exit;
    options.file = spec->file;
    options.args = argv;
    options.env = envp;
    options.cwd =
        (spec->cwd != NULL && spec->cwd[0] != '\0') ? spec->cwd : NULL;
    options.stdio_count = 3;
    options.stdio = stdio;
#ifdef _WIN32
    options.flags = UV_PROCESS_WINDOWS_HIDE;
#else
    options.flags = UV_PROCESS_DETACHED;
#endif

    x->process.data = x;
    rc = uv_spawn(loop, &x->process, &options);

    free_env(envp);
    free(argv);

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
