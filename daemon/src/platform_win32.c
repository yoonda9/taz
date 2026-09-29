#include <stdio.h>

#include <windows.h>

#include "taz/platform.h"

static uv_async_t g_stop_async;

static BOOL WINAPI console_ctrl_handler(DWORD ctrl_type)
{
    (void)ctrl_type;
    (void)uv_async_send(&g_stop_async);
    return TRUE;
}

static void on_stop_async(uv_async_t *handle)
{
    uv_loop_t *loop = handle->loop;
    uv_close((uv_handle_t *)handle, NULL);
    uv_stop(loop);
}

void taz_platform_init_signals(uv_loop_t *loop)
{
    (void)uv_async_init(loop, &g_stop_async, on_stop_async);
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
}

void taz_platform_flush(void)
{
    (void)_flushall();
}
