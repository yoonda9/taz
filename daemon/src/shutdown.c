#include "taz/shutdown.h"

#include <stdlib.h>

#include "taz/connection.h"
#include "taz/log.h"
#include "taz/platform.h"
#include "taz/work.h"

/* taz_shutdown_request/mode/in_progress state. g_mode is the single
 * source of truth: NONE until the first request, then sticky, except that
 * STOP overrides RESTART. */
static taz_shutdown_mode_t g_mode = TAZ_SHUTDOWN_NONE;
static taz_server_t *g_bound_server = NULL;

/* taz_server_shutdown's own idempotence guard, independent of g_mode: a
 * test may call it directly, more than once, without ever going through
 * taz_shutdown_request. */
static int g_walk_done = 0;

/* The walk's uv_unref'd deadline timer, heap-allocated so it outlives the
 * taz_server_shutdown call that starts it; NULL once never started or
 * already closed by taz_shutdown_finish. g_force_closed tracks whether the
 * timer's first (TAZ_SHUTDOWN_DRAIN_MS) firing has already force-closed
 * every still-open connection, so the second firing only logs. */
static uv_timer_t *g_deadline_timer = NULL;
static int g_force_closed = 0;

static void log_handle_warn_cb(uv_handle_t *handle, void *arg)
{
    (void)arg;
    taz_log(TAZ_LOG_WARN, "shutdown: loop still has an open %s handle",
            uv_handle_type_name(uv_handle_get_type(handle)));
}

static void log_handle_error_cb(uv_handle_t *handle, void *arg)
{
    (void)arg;
    taz_log(TAZ_LOG_ERROR, "shutdown: loop still has an open %s handle",
            uv_handle_type_name(uv_handle_get_type(handle)));
}

static void on_deadline_timer_closed(uv_handle_t *handle)
{
    free(handle);
    g_deadline_timer = NULL;
}

static void on_deadline_timer(uv_timer_t *handle)
{
    taz_server_t *server = (taz_server_t *)handle->data;

    if (!g_force_closed)
    {
        g_force_closed = 1;
        taz_conn_force_close_all(&server->loop);
        (void)uv_timer_start(
            handle, on_deadline_timer,
            (uint64_t)(TAZ_SHUTDOWN_STALL_LOG_MS - TAZ_SHUTDOWN_DRAIN_MS), 0U);
        return;
    }

    uv_walk(&server->loop, log_handle_warn_cb, NULL);
}

void taz_shutdown_bind(taz_server_t *server)
{
    g_bound_server = server;
}

void taz_server_shutdown(taz_server_t *server)
{
    if (g_walk_done)
    {
        return;
    }
    g_walk_done = 1;

    taz_work_begin_shutdown();
    taz_server_close_listener(server);
    taz_conn_shutdown_all(&server->loop);
    taz_platform_release_signals();

    g_force_closed = 0;
    g_deadline_timer = (uv_timer_t *)malloc(sizeof(*g_deadline_timer));
    if (g_deadline_timer != NULL)
    {
        (void)uv_timer_init(&server->loop, g_deadline_timer);
        g_deadline_timer->data = server;
        uv_unref((uv_handle_t *)g_deadline_timer);
        (void)uv_timer_start(g_deadline_timer, on_deadline_timer,
                             TAZ_SHUTDOWN_DRAIN_MS, 0U);
    }
}

void taz_shutdown_request(taz_shutdown_mode_t mode)
{
    if (g_mode != TAZ_SHUTDOWN_NONE)
    {
        /* A stop asked for while a restart is already draining means the
         * daemon must not come back: the walk is already running, so only
         * what main does once the loop returns changes. */
        if (mode == TAZ_SHUTDOWN_STOP)
        {
            g_mode = TAZ_SHUTDOWN_STOP;
        }
        return;
    }
    g_mode = mode;
    if (g_bound_server != NULL)
    {
        taz_server_shutdown(g_bound_server);
    }
}

taz_shutdown_mode_t taz_shutdown_mode(void)
{
    return g_mode;
}

int taz_shutdown_in_progress(void)
{
    return g_mode != TAZ_SHUTDOWN_NONE;
}

int taz_shutdown_finish(taz_server_t *server)
{
    uv_loop_t *loop = &server->loop;
    int rc;

    if (g_deadline_timer != NULL &&
        !uv_is_closing((uv_handle_t *)g_deadline_timer))
    {
        uv_close((uv_handle_t *)g_deadline_timer, on_deadline_timer_closed);
    }
    taz_platform_close_signals();
    (void)uv_run(loop, UV_RUN_DEFAULT);

    rc = uv_loop_close(loop);
    if (rc != 0)
    {
        uv_walk(loop, log_handle_error_cb, NULL);
    }
    return rc;
}

void taz_shutdown_reset_for_tests(void)
{
    g_mode = TAZ_SHUTDOWN_NONE;
    g_bound_server = NULL;
    g_walk_done = 0;
    g_force_closed = 0;
}
