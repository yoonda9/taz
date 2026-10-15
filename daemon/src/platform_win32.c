#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>

#include "taz/cmdline.h"
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
    /* A process created with CREATE_NEW_PROCESS_GROUP (as a RESTART
     * successor is) starts with CTRL_C ignored; the daemon handles it. */
    (void)SetConsoleCtrlHandler(NULL, FALSE);
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

/* Windows' own hard limit for a CreateProcess command line. */
#define TAZ_REEXEC_CMDLINE_MAX 32768U

int taz_platform_reexec(char *const argv[])
{
    /* Heap, not stack: TAZ_REEXEC_CMDLINE_MAX*(1+sizeof(wchar_t)) bytes
     * would otherwise trip MSVC /analyze's C6262 large-stack-frame check
     * (the project's -Wframe-larger-than=16384 equivalent). */
    char *cmdline_utf8 = NULL;
    wchar_t *cmdline_w = NULL;
    wchar_t *exe_w = NULL;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    int ok = 0;

    cmdline_utf8 = (char *)malloc(TAZ_REEXEC_CMDLINE_MAX);
    cmdline_w = (wchar_t *)malloc(TAZ_REEXEC_CMDLINE_MAX * sizeof(wchar_t));
    exe_w = (wchar_t *)malloc(TAZ_REEXEC_CMDLINE_MAX * sizeof(wchar_t));
    if (cmdline_utf8 == NULL || cmdline_w == NULL || exe_w == NULL)
    {
        goto done;
    }

    if (taz_cmdline_build(argv, cmdline_utf8, TAZ_REEXEC_CMDLINE_MAX) != 0)
    {
        goto done;
    }
    /* Not MAX_PATH: with long paths enabled, the executable's own path
     * can run to the same 32767-character limit as a command line. */
    if (MultiByteToWideChar(CP_UTF8, 0, argv[0], -1, exe_w,
                            (int)TAZ_REEXEC_CMDLINE_MAX) == 0)
    {
        goto done;
    }
    if (MultiByteToWideChar(CP_UTF8, 0, cmdline_utf8, -1, cmdline_w,
                            (int)TAZ_REEXEC_CMDLINE_MAX) == 0)
    {
        goto done;
    }

    (void)memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    (void)memset(&pi, 0, sizeof(pi));

    if (!CreateProcessW(exe_w, cmdline_w, NULL, NULL, TRUE,
                        CREATE_NEW_PROCESS_GROUP, NULL, NULL, &si, &pi))
    {
        goto done;
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    ok = 1;

done:
    free(cmdline_utf8);
    free(cmdline_w);
    free(exe_w);
    return ok ? 0 : -1;
}
