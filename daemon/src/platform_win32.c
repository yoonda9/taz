#include <stdio.h>

#include <windows.h>

#include "taz/platform.h"
#include "taz/shutdown.h"

static uv_async_t g_stop_async;

/* Set by taz_platform_close_signals, on the loop thread, before it closes
 * g_stop_async. g_stop_lock covers both the flag and the handler's
 * uv_async_send: checking the flag and then sending is not atomic on its
 * own, and a control event's thread that read 0 just before the close
 * would otherwise send to a closing handle (a libuv assert) or, after
 * uv_loop_close, to a dead completion port (uv_fatal_error). */
static int g_stopping = 0;
static SRWLOCK g_stop_lock = SRWLOCK_INIT;

/* 1 from taz_platform_init_signals until taz_platform_close_signals. A
 * unit test may drive taz_server_shutdown (and so
 * taz_platform_release_signals/close_signals) on a loop that never called
 * taz_platform_init_signals; g_stop_async is then still a zero-initialized
 * static, and uv_close on a handle uv_async_init never touched is
 * undefined. Loop thread only. */
static int g_initialized = 0;

static BOOL WINAPI console_ctrl_handler(DWORD ctrl_type)
{
    (void)ctrl_type;
    AcquireSRWLockExclusive(&g_stop_lock);
    if (!g_stopping)
    {
        (void)uv_async_send(&g_stop_async);
    }
    ReleaseSRWLockExclusive(&g_stop_lock);
    return TRUE;
}

static void on_stop_async(uv_async_t *handle)
{
    (void)handle;
    taz_shutdown_request(TAZ_SHUTDOWN_STOP);
}

void taz_platform_init_signals(uv_loop_t *loop)
{
    (void)uv_async_init(loop, &g_stop_async, on_stop_async);
    AcquireSRWLockExclusive(&g_stop_lock);
    g_stopping = 0;
    ReleaseSRWLockExclusive(&g_stop_lock);
    (void)SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
    g_initialized = 1;
}

void taz_platform_release_signals(void)
{
    if (!g_initialized)
    {
        return;
    }
    uv_unref((uv_handle_t *)&g_stop_async);
}

void taz_platform_close_signals(void)
{
    if (!g_initialized)
    {
        return;
    }
    AcquireSRWLockExclusive(&g_stop_lock);
    g_stopping = 1;
    ReleaseSRWLockExclusive(&g_stop_lock);
    if (!uv_is_closing((uv_handle_t *)&g_stop_async))
    {
        uv_close((uv_handle_t *)&g_stop_async, NULL);
    }
    g_initialized = 0;
}

void taz_platform_flush(void)
{
    (void)_flushall();
}
