#include "taz/process.h"

#ifdef _WIN32

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wchar.h>

#include <aclapi.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <windows.h>

#include "taz/error.h"
#include "taz/fsutil.h"

/* Note: Windows API typedefs (NTSTATUS, PROCESS_BASIC_INFORMATION,
 * UNICODE_STRING, PFN_NTQUERYINFORMATIONPROCESS) use their standard Windows
 * names intentionally to match the native API conventions and improve code
 * clarity when interfacing with Windows internals. Clang-tidy naming rules are
 * suppressed for these identifiers. */

/* Stack buffer sizes. MSVC's cl.exe has no VLA support, so these must be
 * true compile-time constants (a local const size_t is not one in C) -
 * #define, matching process_linux.c's TAZ_PROC_* buffer-size macros. */
#define TAZ_WIN32_CMDLINE_MAX_SIZE 4096U
#define TAZ_WIN32_PATH_BUFFER_SIZE 2048U
/* Longest UTF-8 form of a name of up to MAX_PATH UTF-16 units (a Toolhelp
 * szExeFile, or one path component): at most 3 bytes per unit, since a
 * surrogate pair's 4 bytes span 2 units, plus the NUL. */
#define TAZ_WIN32_NAME_UTF8_SIZE ((MAX_PATH * 3U) + 1U)
/* A FILETIME / GetProcessTimes tick is 100 ns; taz_process_sample_t's
 * cpu_time_ns is nanoseconds. */
#define TAZ_WIN32_FILETIME_UNIT_NS 100U

/* Heap-owned copy of a process snapshot: the enumeration base for process
 * listing. */
typedef struct
{
    PROCESSENTRY32W *entries;
    size_t count;
    size_t capacity;
} win32_process_snapshot_t;

/* Appends entry (a copy), growing geometrically. Returns 0 on OOM. */
static int win32_snapshot_append(win32_process_snapshot_t *snap,
                                 const PROCESSENTRY32W *entry)
{
    if (snap->count == snap->capacity)
    {
        const size_t new_capacity =
            (snap->capacity == 0U) ? 64U : snap->capacity * 2U;
        PROCESSENTRY32W *grown;

        if (new_capacity > (SIZE_MAX / sizeof(*grown)))
        {
            return 0;
        }
        grown = (PROCESSENTRY32W *)realloc(snap->entries,
                                           new_capacity * sizeof(*grown));
        if (grown == NULL)
        {
            return 0;
        }
        snap->entries = grown;
        snap->capacity = new_capacity;
    }
    snap->entries[snap->count] = *entry;
    snap->count++;
    return 1;
}

/* Appends a copy of *entry. Grows list->entries geometrically. Returns 0 on
 * OOM. */
static int win32_entry_list_append(taz_process_list_t *list,
                                   const taz_process_entry_t *entry)
{
    if (list->count == list->capacity)
    {
        const size_t new_capacity =
            (list->capacity == 0U) ? 64U : list->capacity * 2U;
        taz_process_entry_t *grown;

        if (new_capacity > (SIZE_MAX / sizeof(*grown)))
        {
            return 0;
        }
        grown = (taz_process_entry_t *)realloc(list->entries,
                                               new_capacity * sizeof(*grown));
        if (grown == NULL)
        {
            return 0;
        }
        list->entries = grown;
        list->capacity = new_capacity;
    }
    list->entries[list->count] = *entry;
    list->count++;
    return 1;
}

/* th32ProcessID is PROCESSENTRY32W's third field, not its first (that's
 * dwSize, identical on every entry since it is always sizeof(entry)), so
 * this cannot be a generic "sort by leading uint32_t" comparator. */
static int win32_processentry_compare(const void *a, const void *b)
{
    const DWORD va = ((const PROCESSENTRY32W *)a)->th32ProcessID;
    const DWORD vb = ((const PROCESSENTRY32W *)b)->th32ProcessID;

    if (va < vb)
    {
        return -1;
    }
    if (va > vb)
    {
        return 1;
    }
    return 0;
}

/* Best-effort user lookup via process handle: gets the token's User SID
 * (the account the process runs as) and delegates to
 * taz_win32_account_from_sid. Deliberately TokenUser, not TokenOwner: the
 * owner is the default DACL owner for new objects the process creates,
 * which can differ from the running account (e.g. some Administrators-group
 * configurations) and would report the wrong identity for the "user"
 * field. Any failure leaves user untouched (already "" from calloc). */
static void win32_fill_process_user(HANDLE h_process, char *user,
                                    size_t user_size)
{
    HANDLE h_token = NULL;
    TOKEN_USER *token_user = NULL;
    DWORD token_size = 0U;

    if ((user_size == 0U) || (h_process == NULL))
    {
        return;
    }

    if (!OpenProcessToken(h_process, TOKEN_QUERY, &h_token))
    {
        return;
    }

    if (!GetTokenInformation(h_token, TokenUser, NULL, 0U, &token_size))
    {
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER)
        {
            CloseHandle(h_token);
            return;
        }
    }

    token_user = (TOKEN_USER *)malloc(token_size);
    if (token_user == NULL)
    {
        CloseHandle(h_token);
        return;
    }

    if (GetTokenInformation(h_token, TokenUser, token_user, token_size,
                            &token_size))
    {
        if (token_user->User.Sid != NULL)
        {
            taz_win32_account_from_sid(token_user->User.Sid, user, user_size);
        }
    }

    free(token_user);
    CloseHandle(h_token);
}

/* Converts FILETIME (100-nanosecond intervals since 1601-01-01) to Unix
 * seconds (since 1970-01-01). */
static uint64_t win32_filetime_to_unix_seconds(const FILETIME *ft)
{
    /* Windows epoch: 1601-01-01. Unix epoch: 1970-01-01.
     * Difference in 100-nanosecond intervals: 116444736000000000. */
    const uint64_t filetime_unix_epoch = 116444736000000000ULL;
    const uint64_t hundred_ns_per_second = 10000000ULL;
    uint64_t filetime_value;
    uint64_t unix_seconds;

    filetime_value = ((uint64_t)ft->dwHighDateTime << 32) | ft->dwLowDateTime;
    if (filetime_value <= filetime_unix_epoch)
    {
        return 0U;
    }
    unix_seconds =
        (filetime_value - filetime_unix_epoch) / hundred_ns_per_second;
    return unix_seconds;
}

/* Calculates CPU percentage: (cpu_ticks / ticks_per_sec) / elapsed_seconds
 * * 100, one CPU == 100%. Returns 0.0F when elapsed <= 0. */
static float win32_cpu_percent(uint64_t cpu_time_100ns, double elapsed_seconds)
{
    const double nanoseconds_per_second = 10000000.0;
    const double cpu_percentage_multiplier = 100.0;
    double cpu_seconds;
    double ratio;

    if (elapsed_seconds <= 0.0)
    {
        return 0.0F;
    }
    cpu_seconds = (double)cpu_time_100ns / nanoseconds_per_second;
    ratio = (cpu_seconds / elapsed_seconds) * cpu_percentage_multiplier;
    return (float)ratio;
}

/* Converts a NUL-terminated wide process name (a Toolhelp szExeFile or an
 * image path's basename) to sanitized UTF-8, cut to name_size at a codepoint
 * boundary. Returns 1 on success, 0 if the wide-to-UTF-8 conversion fails
 * (name left untouched). */
static int win32_name_to_utf8(const WCHAR *wide_name, char *name,
                              size_t name_size)
{
    char name_utf8[TAZ_WIN32_NAME_UTF8_SIZE];
    int name_mbc =
        WideCharToMultiByte(CP_UTF8, 0, wide_name, -1, name_utf8,
                            (int)TAZ_WIN32_NAME_UTF8_SIZE, NULL, NULL);
    if (name_mbc <= 0)
    {
        return 0;
    }
    taz_fsutil_sanitize_utf8(name_utf8, (size_t)name_mbc - 1, name, name_size);
    return 1;
}

/* Fills the fields every listed process gets, even one OpenProcess refuses
 * (e.g. pid 4 "System"): pid, name and state. */
static void win32_init_entry(const PROCESSENTRY32W *snap_entry,
                             taz_process_entry_t *entry)
{
    memset(entry, 0, sizeof(*entry));
    entry->pid = snap_entry->th32ProcessID;
    (void)win32_name_to_utf8(snap_entry->szExeFile, entry->name,
                             sizeof(entry->name));

    /* Windows reports state as "running" always. */
    (void)strncpy(entry->state, "running", sizeof(entry->state) - 1U);
    entry->state[sizeof(entry->state) - 1U] = '\0';
}

/* Adds memory and CPU usage from an open process handle. Best effort: a
 * query that fails leaves its field 0 rather than dropping the process from
 * the list. */
static void win32_fill_entry_usage(HANDLE h_process, taz_process_entry_t *entry)
{
    FILETIME create_time;
    FILETIME exit_time;
    FILETIME kernel_time;
    FILETIME user_time;
    uint64_t start_time_unix;
    PROCESS_MEMORY_COUNTERS mem_info;

    mem_info.cb = sizeof(mem_info);
    if (K32GetProcessMemoryInfo(h_process, &mem_info, sizeof(mem_info)))
    {
        entry->memory_bytes = mem_info.WorkingSetSize;
    }

    /* Start time and CPU time (user + kernel). */
    if (GetProcessTimes(h_process, &create_time, &exit_time, &kernel_time,
                        &user_time))
    {
        start_time_unix = win32_filetime_to_unix_seconds(&create_time);
        entry->cpu_percent =
            win32_cpu_percent((((uint64_t)kernel_time.dwHighDateTime << 32) |
                               kernel_time.dwLowDateTime) +
                                  (((uint64_t)user_time.dwHighDateTime << 32) |
                                   user_time.dwLowDateTime),
                              (double)time(NULL) - (double)start_time_unix);
    }
}

/* OpenProcess ignores the low two bits of a pid, so a request for pid P+1
 * opens process P. Returns 1 when h_process is the process numbered pid. */
static int win32_handle_matches_pid(HANDLE h_process, uint32_t pid)
{
    return GetProcessId(h_process) == (DWORD)pid;
}

int taz_process_enumerate(const char *filter, taz_process_list_t *out,
                          taz_v1_ErrorCode *code, const char **detail)
{
    HANDLE h_snapshot = NULL;
    PROCESSENTRY32W entry;
    win32_process_snapshot_t snapshot = {NULL, 0U, 0U};
    size_t i;

    memset(out, 0, sizeof(*out));

    /* Create a process snapshot. */
    h_snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (h_snapshot == INVALID_HANDLE_VALUE)
    {
        DWORD err = GetLastError();
        *code = taz_error_from_win32(err);
        *detail = uv_strerror(uv_translate_sys_error((int)err));
        return -1;
    }

    entry.dwSize = sizeof(entry);
    if (!Process32FirstW(h_snapshot, &entry))
    {
        DWORD err = GetLastError();
        CloseHandle(h_snapshot);
        *code = taz_error_from_win32(err);
        *detail = uv_strerror(uv_translate_sys_error((int)err));
        return -1;
    }

    /* Collect all processes from the snapshot. */
    do
    {
        if (!win32_snapshot_append(&snapshot, &entry))
        {
            CloseHandle(h_snapshot);
            free(snapshot.entries);
            *code = taz_v1_ErrorCode_ERROR_CODE_INTERNAL;
            *detail = "out of memory";
            return -1;
        }
    } while (Process32NextW(h_snapshot, &entry));

    CloseHandle(h_snapshot);

    /* Sort by PID for consistent ordering. */
    if (snapshot.count > 0U)
    {
        qsort(snapshot.entries, snapshot.count, sizeof(*snapshot.entries),
              win32_processentry_compare);
    }

    /* Open each process and fill in the entry data. */
    for (i = 0U; i < snapshot.count; i++)
    {
        HANDLE h_process = NULL;
        taz_process_entry_t proc_entry;

        /* Toolhelp lists the System Idle Process as pid 0, which no request
         * can address (pid 0 is INVALID_REQUEST), so leave it out. */
        if (snapshot.entries[i].th32ProcessID == 0U)
        {
            continue;
        }

        win32_init_entry(&snapshot.entries[i], &proc_entry);

        /* OpenProcess fails ACCESS_DENIED on protected/system processes
         * (e.g. pid 4 "System") -- list the entry with blanks rather than
         * dropping it. */
        h_process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                snapshot.entries[i].th32ProcessID);
        if (h_process != NULL)
        {
            win32_fill_entry_usage(h_process, &proc_entry);
            win32_fill_process_user(h_process, proc_entry.user,
                                    sizeof(proc_entry.user));
            CloseHandle(h_process);
        }

        /* Apply filter. */
        if ((filter[0] != '\0') && (strstr(proc_entry.name, filter) == NULL))
        {
            continue;
        }

        if (!win32_entry_list_append(out, &proc_entry))
        {
            free(snapshot.entries);
            taz_process_list_free(out);
            *code = taz_v1_ErrorCode_ERROR_CODE_INTERNAL;
            *detail = "out of memory";
            return -1;
        }
    }

    free(snapshot.entries);
    return 0;
}

void taz_process_list_free(taz_process_list_t *list)
{
    free(list->entries);
    memset(list, 0, sizeof(*list));
}

/* Retrieves the command line for a process via
 * NtQueryInformationProcess(ProcessCommandLineInformation), resolved through
 * GetProcAddress on ntdll (undocumented but stable since Windows 8.1). This
 * info class exists specifically to avoid hand-walking the PEB with
 * hardcoded, version/bitness-fragile offsets: a size-probe call (NULL buffer)
 * reports the required size via STATUS_INFO_LENGTH_MISMATCH, then a second
 * call fills a heap buffer whose first bytes are a UNICODE_STRING pointing
 * into the same allocation. Works with PROCESS_QUERY_LIMITED_INFORMATION, so
 * it needs no PROCESS_VM_READ. Returns a heap-allocated, NUL-terminated
 * UTF-8 string or NULL on failure. */
static char *win32_get_command_line(HANDLE h_process)
{
    /* clang-tidy: suppress identifier naming violations for Windows API
     * typedefs */
    typedef LONG NTSTATUS; /* NOLINT(readability-identifier-naming) */
    typedef struct
    {
        USHORT Length;
        USHORT MaximumLength;
        PWSTR Buffer;
    } UNICODE_STRING; /* NOLINT(readability-identifier-naming) */

    /* NOLINTBEGIN(readability-identifier-naming) - intentional Windows API
     * naming for the typedef name and its parameters. */
    typedef NTSTATUS(WINAPI * PFN_NTQUERYINFORMATIONPROCESS)(
        HANDLE ProcessHandle, ULONG ProcessInformationClass,
        PVOID ProcessInformation, ULONG ProcessInformationLength,
        PULONG ReturnLength);
    /* NOLINTEND(readability-identifier-naming) */

    /* Resolved on every call rather than cached in a static: this runs on
     * pool threads, and the lookup is cheap next to the query itself. */
    PFN_NTQUERYINFORMATIONPROCESS nt_query_information_process_fn;
    /* ProcessCommandLineInformation: undocumented NtQueryInformationProcess
     * class, available since Windows 8.1, that returns a process's command
     * line without reading the PEB directly. */
    const ULONG process_command_line_information_class = 60UL;
    /* STATUS_INFO_LENGTH_MISMATCH: the probe call's expected "too small"
     * status, used to read back the required buffer size. */
    const NTSTATUS status_info_length_mismatch = (NTSTATUS)0xC0000004L;
    HMODULE h_ntdll;
    NTSTATUS status;
    ULONG required_size = 0U;
    PVOID buffer = NULL;
    UNICODE_STRING *cmd_line;
    int cmdline_len_wchars;
    int cmdline_len_mbc;
    char *result;

    h_ntdll = GetModuleHandleA("ntdll.dll");
    if (h_ntdll == NULL)
    {
        return NULL;
    }
    nt_query_information_process_fn =
        (PFN_NTQUERYINFORMATIONPROCESS)GetProcAddress(
            h_ntdll, "NtQueryInformationProcess");
    if (nt_query_information_process_fn == NULL)
    {
        return NULL;
    }

    /* Size probe: a NULL buffer returns STATUS_INFO_LENGTH_MISMATCH and
     * fills required_size. Any other status means the class is unsupported
     * on this OS build or the handle lacks the rights. */
    status = nt_query_information_process_fn(
        h_process, process_command_line_information_class, NULL, 0U,
        &required_size);
    if ((status != status_info_length_mismatch) || (required_size == 0U))
    {
        return NULL;
    }

    buffer = malloc(required_size);
    if (buffer == NULL)
    {
        return NULL;
    }

    status = nt_query_information_process_fn(
        h_process, process_command_line_information_class, buffer,
        required_size, &required_size);
    if (status < 0)
    {
        free(buffer);
        return NULL;
    }

    /* The UNICODE_STRING header is the first bytes of the returned buffer;
     * its Buffer field points within that same allocation. */
    cmd_line = (UNICODE_STRING *)buffer;
    if ((cmd_line->Length == 0U) || (cmd_line->Buffer == NULL))
    {
        free(buffer);
        return NULL;
    }

    cmdline_len_wchars = (int)(cmd_line->Length / sizeof(WCHAR));
    cmdline_len_mbc = WideCharToMultiByte(
        CP_UTF8, 0, cmd_line->Buffer, cmdline_len_wchars, NULL, 0, NULL, NULL);
    if (cmdline_len_mbc <= 0)
    {
        free(buffer);
        return NULL;
    }

    result = (char *)malloc((size_t)cmdline_len_mbc + 1U);
    if (result == NULL)
    {
        free(buffer);
        return NULL;
    }

    if (WideCharToMultiByte(CP_UTF8, 0, cmd_line->Buffer, cmdline_len_wchars,
                            result, cmdline_len_mbc, NULL, NULL) <= 0)
    {
        free(result);
        free(buffer);
        return NULL;
    }
    result[cmdline_len_mbc] = '\0';

    free(buffer);
    return result;
}

/* Resolves a process's name for PROCESS_INFO: the basename of
 * QueryFullProcessImageNameW first, falling back to the Toolhelp snapshot's
 * szExeFile only when that fails (e.g. pid 4 "System", which has no image
 * path). Best-effort: out_name is left untouched if neither source works. */
static void win32_resolve_process_name(HANDLE h_process, DWORD pid,
                                       char *out_name, size_t out_size)
{
    WCHAR path[TAZ_WIN32_PATH_BUFFER_SIZE];
    DWORD path_len = TAZ_WIN32_PATH_BUFFER_SIZE;
    HANDLE h_snapshot;
    PROCESSENTRY32W entry;
    int found_entry = 0;

    if (QueryFullProcessImageNameW(h_process, 0, path, &path_len) &&
        path_len > 0)
    {
        WCHAR *base = wcsrchr(path, L'\\');
        WCHAR *name_start = (base != NULL) ? base + 1 : path;
        if (win32_name_to_utf8(name_start, out_name, out_size))
        {
            return;
        }
    }

    h_snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (h_snapshot == INVALID_HANDLE_VALUE)
    {
        return;
    }

    entry.dwSize = sizeof(entry);
    if (Process32FirstW(h_snapshot, &entry))
    {
        do
        {
            if (entry.th32ProcessID == pid)
            {
                found_entry = 1;
                break;
            }
        } while (Process32NextW(h_snapshot, &entry));
    }
    CloseHandle(h_snapshot);

    if (found_entry)
    {
        (void)win32_name_to_utf8(entry.szExeFile, out_name, out_size);
    }
}

int taz_process_inspect(uint32_t pid, taz_process_detail_t *out,
                        taz_v1_ErrorCode *code, const char **detail)
{
    HANDLE h_process = NULL;
    FILETIME create_time;
    FILETIME exit_time;
    FILETIME kernel_time;
    FILETIME user_time;
    PROCESS_MEMORY_COUNTERS mem_info;
    DWORD process_state;
    char *cmdline = NULL;

    memset(out, 0, sizeof(*out));

    /* Open the process. If OpenProcess fails, the process may not exist or we
     * may not have permission. SYNCHRONIZE is required for the
     * WaitForSingleObject liveness check below; without it the call fails
     * with WAIT_FAILED instead of reporting whether the process has
     * exited, silently defeating that check. */
    h_process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
                            FALSE, pid);
    if (h_process == NULL)
    {
        DWORD err = GetLastError();
        if (err == ERROR_INVALID_PARAMETER)
        {
            *code = taz_v1_ErrorCode_ERROR_CODE_NOT_FOUND;
        }
        else if (err == ERROR_ACCESS_DENIED)
        {
            *code = taz_v1_ErrorCode_ERROR_CODE_PERMISSION_DENIED;
        }
        else
        {
            *code = taz_error_from_win32(err);
        }
        *detail = uv_strerror(uv_translate_sys_error((int)err));
        return -1;
    }

    if (!win32_handle_matches_pid(h_process, pid))
    {
        CloseHandle(h_process);
        *code = taz_v1_ErrorCode_ERROR_CODE_NOT_FOUND;
        *detail = uv_strerror(UV_ESRCH);
        return -1;
    }

    /* Check if the process is still alive. */
    process_state = WaitForSingleObject(h_process, 0);
    if (process_state == WAIT_OBJECT_0)
    {
        /* Process has exited. */
        CloseHandle(h_process);
        *code = taz_v1_ErrorCode_ERROR_CODE_NOT_FOUND;
        *detail = "process has exited";
        return -1;
    }

    /* Find the process's name. */
    win32_resolve_process_name(h_process, pid, out->info.name,
                               sizeof(out->info.name));

    /* Get memory info. */
    mem_info.cb = sizeof(mem_info);
    if (K32GetProcessMemoryInfo(h_process, &mem_info, sizeof(mem_info)))
    {
        out->info.memory_bytes = mem_info.WorkingSetSize;
    }

    /* Get timing info. */
    if (GetProcessTimes(h_process, &create_time, &exit_time, &kernel_time,
                        &user_time))
    {
        out->start_time = win32_filetime_to_unix_seconds(&create_time);
        out->info.cpu_percent =
            win32_cpu_percent((((uint64_t)kernel_time.dwHighDateTime << 32) |
                               kernel_time.dwLowDateTime) +
                                  (((uint64_t)user_time.dwHighDateTime << 32) |
                                   user_time.dwLowDateTime),
                              (double)time(NULL) - (double)out->start_time);
    }

    /* State is always "running" on Windows. */
    (void)strncpy(out->info.state, "running", sizeof(out->info.state) - 1U);
    out->info.state[sizeof(out->info.state) - 1U] = '\0';

    out->info.pid = pid;

    /* Best-effort user lookup. */
    win32_fill_process_user(h_process, out->info.user, sizeof(out->info.user));

    /* Best-effort command line retrieval. */
    cmdline = win32_get_command_line(h_process);
    if (cmdline != NULL)
    {
        out->command_line = (char *)malloc(TAZ_WIN32_CMDLINE_MAX_SIZE);
        if (out->command_line != NULL)
        {
            taz_fsutil_sanitize_utf8(cmdline, strlen(cmdline),
                                     out->command_line,
                                     TAZ_WIN32_CMDLINE_MAX_SIZE);
        }
        free(cmdline);
    }
    else
    {
        out->command_line = (char *)calloc(1U, TAZ_WIN32_CMDLINE_MAX_SIZE);
    }

    if (out->command_line == NULL)
    {
        CloseHandle(h_process);
        taz_process_detail_free(out);
        *code = taz_v1_ErrorCode_ERROR_CODE_INTERNAL;
        *detail = "out of memory";
        return -1;
    }

    /* open_files is always empty on Windows. */
    out->open_files = NULL;
    out->open_files_count = 0;

    CloseHandle(h_process);
    return 0;
}

void taz_process_detail_free(taz_process_detail_t *detail)
{
    free(detail->command_line);
    free(detail->open_files);
    memset(detail, 0, sizeof(*detail));
}

int taz_process_kill(uint32_t pid, int32_t signal, taz_v1_ErrorCode *code,
                     const char **detail)
{
    HANDLE h_process;
    BOOL success;

    (void)signal; /* Signal numbers don't apply on Windows. */

    /* PROCESS_QUERY_LIMITED_INFORMATION is what GetProcessId needs. */
    h_process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE |
                                PROCESS_QUERY_LIMITED_INFORMATION,
                            FALSE, pid);
    if (h_process == NULL)
    {
        DWORD err = GetLastError();
        if (err == ERROR_INVALID_PARAMETER)
        {
            *code = taz_v1_ErrorCode_ERROR_CODE_NOT_FOUND;
        }
        else if (err == ERROR_ACCESS_DENIED)
        {
            *code = taz_v1_ErrorCode_ERROR_CODE_PERMISSION_DENIED;
        }
        else
        {
            *code = taz_error_from_win32(err);
        }
        *detail = uv_strerror(uv_translate_sys_error((int)err));
        return -1;
    }

    if (!win32_handle_matches_pid(h_process, pid))
    {
        CloseHandle(h_process);
        *code = taz_v1_ErrorCode_ERROR_CODE_NOT_FOUND;
        *detail = uv_strerror(UV_ESRCH);
        return -1;
    }

    /* A handle opened by PID can still succeed after the process has
     * exited, as long as some other handle (e.g. libuv's own process
     * handle) keeps the kernel object alive; TerminateProcess on it then
     * fails with ERROR_ACCESS_DENIED rather than reporting "gone". Check
     * liveness first so this reports NOT_FOUND like every other platform. */
    if (WaitForSingleObject(h_process, 0) == WAIT_OBJECT_0)
    {
        CloseHandle(h_process);
        *code = taz_v1_ErrorCode_ERROR_CODE_NOT_FOUND;
        *detail = "process has exited";
        return -1;
    }

    success = TerminateProcess(h_process, 1U);
    CloseHandle(h_process);

    if (!success)
    {
        DWORD err = GetLastError();
        if (err == ERROR_ACCESS_DENIED)
        {
            *code = taz_v1_ErrorCode_ERROR_CODE_PERMISSION_DENIED;
        }
        else
        {
            *code = taz_error_from_win32(err);
        }
        *detail = uv_strerror(uv_translate_sys_error((int)err));
        return -1;
    }

    return 0;
}

/* ---------------------------------------------------------------------------
 * Watch/sample: taz_process_watch_open/_sample/_close. All three are inline
 * on the loop thread except _sample, which runs on the pool but only reads
 * *w (the handle it holds is never touched by the loop while a sample is in
 * flight).
 * ------------------------------------------------------------------------- */

int taz_process_watch_open(uint32_t pid, int use_pidfd, taz_process_watch_t *w,
                           taz_v1_ErrorCode *code, const char **detail)
{
    HANDLE h_process;

    /* Windows has no pidfd equivalent; the held handle below is the only
     * exit-detection mechanism, so use_pidfd does not change anything. */
    (void)use_pidfd;

    memset(w, 0, sizeof(*w));
    w->pid = pid;
    w->exit_fd = -1;

    h_process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
                            FALSE, pid);
    if (h_process == NULL)
    {
        DWORD err = GetLastError();
        if (err == ERROR_INVALID_PARAMETER)
        {
            *code = taz_v1_ErrorCode_ERROR_CODE_NOT_FOUND;
        }
        else if (err == ERROR_ACCESS_DENIED)
        {
            *code = taz_v1_ErrorCode_ERROR_CODE_PERMISSION_DENIED;
        }
        else
        {
            *code = taz_error_from_win32(err);
        }
        *detail = uv_strerror(uv_translate_sys_error((int)err));
        return -1;
    }

    if (!win32_handle_matches_pid(h_process, pid))
    {
        CloseHandle(h_process);
        *code = taz_v1_ErrorCode_ERROR_CODE_NOT_FOUND;
        *detail = uv_strerror(UV_ESRCH);
        return -1;
    }

    if (WaitForSingleObject(h_process, 0) == WAIT_OBJECT_0)
    {
        CloseHandle(h_process);
        *code = taz_v1_ErrorCode_ERROR_CODE_NOT_FOUND;
        *detail = "process has exited";
        return -1;
    }

    w->handle = (void *)h_process;
    return 0;
}

/* code/detail are never written here (a sample never fails once the handle
 * is held - every query below is best-effort and leaves its field blank on
 * failure): NOLINT(readability-non-const-parameter) on both, since the
 * signature is shared with process_linux.c's implementation, which does
 * write through them. */
int taz_process_watch_sample(
    const taz_process_watch_t *w, taz_process_sample_t *out,
    taz_v1_ErrorCode *code, // NOLINT(readability-non-const-parameter)
    const char **detail)    // NOLINT(readability-non-const-parameter)
{
    HANDLE h_process = (HANDLE)w->handle;

    (void)code;
    (void)detail;

    memset(out, 0, sizeof(*out));
    out->info.pid = w->pid;

    if (WaitForSingleObject(h_process, 0) == WAIT_OBJECT_0)
    {
        DWORD exit_code = 0U;

        out->state = TAZ_PROCESS_SAMPLE_EXITED;
        if (GetExitCodeProcess(h_process, &exit_code))
        {
            out->exit_code = (int32_t)exit_code;
            out->exit_code_known = 1;
        }
        return 0;
    }

    out->state = TAZ_PROCESS_SAMPLE_LIVE;

    {
        FILETIME create_time;
        FILETIME exit_time;
        FILETIME kernel_time;
        FILETIME user_time;

        if (GetProcessTimes(h_process, &create_time, &exit_time, &kernel_time,
                            &user_time))
        {
            out->starttime = (((uint64_t)create_time.dwHighDateTime << 32) |
                              create_time.dwLowDateTime);
            out->cpu_time_ns = ((((uint64_t)kernel_time.dwHighDateTime << 32) |
                                 kernel_time.dwLowDateTime) +
                                (((uint64_t)user_time.dwHighDateTime << 32) |
                                 user_time.dwLowDateTime)) *
                               TAZ_WIN32_FILETIME_UNIT_NS;
        }
    }

    {
        PROCESS_MEMORY_COUNTERS mem_info;

        mem_info.cb = sizeof(mem_info);
        if (K32GetProcessMemoryInfo(h_process, &mem_info, sizeof(mem_info)))
        {
            out->info.memory_bytes = mem_info.WorkingSetSize;
        }
    }

    win32_resolve_process_name(h_process, (DWORD)w->pid, out->info.name,
                               sizeof(out->info.name));
    win32_fill_process_user(h_process, out->info.user, sizeof(out->info.user));

    (void)strncpy(out->info.state, "running", sizeof(out->info.state) - 1U);
    out->info.state[sizeof(out->info.state) - 1U] = '\0';

    return 0;
}

void taz_process_watch_close(taz_process_watch_t *w)
{
    if (w->handle != NULL)
    {
        CloseHandle((HANDLE)w->handle);
    }
    memset(w, 0, sizeof(*w));
    w->exit_fd = -1;
}

#endif /* _WIN32 */
