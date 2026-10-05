#include <signal.h>
#include <stdio.h>

#include "taz/platform.h"
#include "taz/work.h"

static uv_signal_t g_sigint;
static uv_signal_t g_sigterm;

static void on_signal(uv_signal_t *handle, int signum)
{
    (void)signum;
    taz_work_request_shutdown(handle->loop);
}

void taz_platform_init_signals(uv_loop_t *loop)
{
    (void)uv_signal_init(loop, &g_sigint);
    (void)uv_signal_start(&g_sigint, on_signal, SIGINT);
    (void)uv_signal_init(loop, &g_sigterm);
    (void)uv_signal_start(&g_sigterm, on_signal, SIGTERM);
}

void taz_platform_flush(void)
{
    (void)fflush(stdout);
    (void)fflush(stderr);
}
