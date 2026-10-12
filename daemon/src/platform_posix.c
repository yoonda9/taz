#include <signal.h>
#include <stdio.h>

#include <unistd.h>

#include "taz/platform.h"
#include "taz/shutdown.h"

static uv_signal_t g_sigint;
static uv_signal_t g_sigterm;

/* 1 from taz_platform_init_signals until taz_platform_close_signals. A
 * unit test may drive taz_server_shutdown/taz_shutdown_finish (and so
 * taz_platform_release_signals/close_signals) on a server that never
 * called taz_platform_init_signals; g_sigint/g_sigterm are then still
 * zero-initialized statics, and touching a handle uv_signal_init never
 * initialised is undefined. */
static int g_initialized = 0;

static void on_signal(uv_signal_t *handle, int signum)
{
    (void)handle;
    (void)signum;
    taz_shutdown_request(TAZ_SHUTDOWN_STOP);
}

void taz_platform_init_signals(uv_loop_t *loop)
{
    (void)uv_signal_init(loop, &g_sigint);
    (void)uv_signal_start(&g_sigint, on_signal, SIGINT);
    (void)uv_signal_init(loop, &g_sigterm);
    (void)uv_signal_start(&g_sigterm, on_signal, SIGTERM);
    g_initialized = 1;
}

void taz_platform_release_signals(void)
{
    if (!g_initialized)
    {
        return;
    }
    uv_unref((uv_handle_t *)&g_sigint);
    uv_unref((uv_handle_t *)&g_sigterm);
}

static void close_signal(uv_signal_t *handle)
{
    if (uv_is_closing((uv_handle_t *)handle))
    {
        return;
    }
    (void)uv_signal_stop(handle);
    uv_close((uv_handle_t *)handle, NULL);
}

void taz_platform_close_signals(void)
{
    if (!g_initialized)
    {
        return;
    }
    close_signal(&g_sigint);
    close_signal(&g_sigterm);
    g_initialized = 0;
}

void taz_platform_flush(void)
{
    (void)fflush(stdout);
    (void)fflush(stderr);
}

int taz_platform_reexec(char *const argv[])
{
    execv(argv[0], argv);
    return -1;
}
