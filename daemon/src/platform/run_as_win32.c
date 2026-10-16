#include "taz/run_as_win32.h"

#ifdef _WIN32

#include <stdio.h>
#include <string.h>
#include <wchar.h>

/* WIN32_NO_STATUS keeps winnt.h (pulled in by windows.h) from defining its
 * own short STATUS_* subset, so <ntstatus.h> right after can define the
 * full set without redefinition warnings - the standard trick for code
 * that wants every NTSTATUS value LsaLogonUser can return. */
#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include <ntsecapi.h>
#include <ntstatus.h>

#include "taz/fsutil.h"
#include "taz/run_as.h"

/* UNICODE_STRING's Length/MaximumLength are USHORT byte counts. */
#define TAZ_RUN_AS_WIN32_UNICODE_STRING_MAX_BYTES 0xFFFFU

/* Scratch size for one FormatMessageW result after CR/LF stripping and
 * UTF-8 conversion - comfortably more than any system message text, and
 * still small next to the 256-byte ErrorInfo.message it feeds into. */
#define TAZ_RUN_AS_WIN32_WIN32_TEXT_MAX 200U

/* -1 until the first probe; 0/1 afterward (table P1: "cached"). */
static int g_privileged_cached = -1;

/* NULL = the real logon below; non-NULL replaces it wholesale for tests
 * (table L12). */
static taz_run_as_win32_logon_fn_t g_logon_override = NULL;

/* ---------------------------------------------------------------------
 * P1 - the privilege probe
 * --------------------------------------------------------------------- */

/* Enables name on token if it is present but disabled; leaves an already-
 * enabled privilege untouched. Returns 0 if the privilege is absent from
 * the token altogether (LookupPrivilegeValueW failing, or
 * AdjustTokenPrivileges reporting ERROR_NOT_ALL_ASSIGNED, both mean
 * "this token does not have it") or if the call itself fails outright. */
static int enable_privilege_if_present(HANDLE token, LPCWSTR name)
{
    LUID luid;
    TOKEN_PRIVILEGES priv;

    if (!LookupPrivilegeValueW(NULL, name, &luid))
    {
        return 0;
    }

    priv.PrivilegeCount = 1;
    priv.Privileges[0].Luid = luid;
    priv.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    SetLastError(0);
    if (!AdjustTokenPrivileges(token, FALSE, &priv, sizeof(priv), NULL, NULL))
    {
        return 0;
    }
    return GetLastError() != ERROR_NOT_ALL_ASSIGNED;
}

static int probe_privileged(void)
{
    HANDLE token = NULL;
    const LPCWSTR privilege_names[] = {
        L"SeTcbPrivilege",           L"SeAssignPrimaryTokenPrivilege",
        L"SeIncreaseQuotaPrivilege", L"SeBackupPrivilege",
        L"SeRestorePrivilege",
    };
    size_t i;

    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_QUERY | TOKEN_ADJUST_PRIVILEGES, &token))
    {
        return 0;
    }

    for (i = 0U; i < sizeof(privilege_names) / sizeof(privilege_names[0]); i++)
    {
        if (!enable_privilege_if_present(token, privilege_names[i]))
        {
            (void)CloseHandle(token);
            return 0;
        }
    }

    (void)CloseHandle(token);
    return 1;
}

int taz_run_as_win32_privileged(void)
{
    if (g_privileged_cached < 0)
    {
        g_privileged_cached = probe_privileged();
    }
    return g_privileged_cached;
}

/* ---------------------------------------------------------------------
 * P5 - the daemon's own account
 * --------------------------------------------------------------------- */

void taz_run_as_win32_daemon_user(char *buf, size_t bufsize)
{
    HANDLE token = NULL;
    TOKEN_USER *token_user = NULL;
    DWORD token_size = 0U;

    if (bufsize == 0U)
    {
        return;
    }
    buf[0] = '\0';

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
    {
        return;
    }

    if (!GetTokenInformation(token, TokenUser, NULL, 0U, &token_size) &&
        (GetLastError() != ERROR_INSUFFICIENT_BUFFER))
    {
        (void)CloseHandle(token);
        return;
    }

    token_user = (TOKEN_USER *)malloc(token_size);
    if (token_user == NULL)
    {
        (void)CloseHandle(token);
        return;
    }

    if (GetTokenInformation(token, TokenUser, token_user, token_size,
                            &token_size) &&
        (token_user->User.Sid != NULL))
    {
        taz_win32_account_from_sid(token_user->User.Sid, buf, bufsize);
    }

    free(token_user);
    (void)CloseHandle(token);
}

/* ---------------------------------------------------------------------
 * L1/L2 - S4U submit buffer layouts
 * --------------------------------------------------------------------- */

size_t taz_run_as_win32_msv_s4u_layout(const wchar_t *user,
                                       const wchar_t *domain, void *buf,
                                       size_t bufsize)
{
    const size_t user_bytes = wcslen(user) * sizeof(wchar_t);
    const size_t domain_bytes = wcslen(domain) * sizeof(wchar_t);
    const size_t header = sizeof(MSV1_0_S4U_LOGON);
    size_t need;
    unsigned char *base;
    MSV1_0_S4U_LOGON *msg;
    unsigned char *user_ptr;
    unsigned char *domain_ptr;

    if ((user_bytes > TAZ_RUN_AS_WIN32_UNICODE_STRING_MAX_BYTES) ||
        (domain_bytes > TAZ_RUN_AS_WIN32_UNICODE_STRING_MAX_BYTES))
    {
        return 0U;
    }
    need = header + user_bytes + domain_bytes;
    if ((buf == NULL) || (bufsize < need))
    {
        return 0U;
    }

    base = (unsigned char *)buf;
    msg = (MSV1_0_S4U_LOGON *)buf;
    msg->MessageType = MsV1_0S4ULogon;
    msg->Flags = 0UL;

    user_ptr = base + header;
    /* A UNICODE_STRING is not NUL-terminated by convention - its Length
     * field is authoritative - so this is not a string copy needing one. */
    // NOLINTNEXTLINE(bugprone-not-null-terminated-result)
    memcpy(user_ptr, user, user_bytes);
    msg->UserPrincipalName.Buffer = (PWSTR)user_ptr;
    msg->UserPrincipalName.Length = (USHORT)user_bytes;
    msg->UserPrincipalName.MaximumLength = (USHORT)user_bytes;

    domain_ptr = user_ptr + user_bytes;
    // NOLINTNEXTLINE(bugprone-not-null-terminated-result)
    memcpy(domain_ptr, domain, domain_bytes);
    msg->DomainName.Buffer = (PWSTR)domain_ptr;
    msg->DomainName.Length = (USHORT)domain_bytes;
    msg->DomainName.MaximumLength = (USHORT)domain_bytes;

    return need;
}

size_t taz_run_as_win32_kerb_s4u_layout(const wchar_t *upn, void *buf,
                                        size_t bufsize)
{
    const size_t upn_bytes = wcslen(upn) * sizeof(wchar_t);
    const size_t header = sizeof(KERB_S4U_LOGON);
    size_t need;
    unsigned char *base;
    KERB_S4U_LOGON *msg;
    unsigned char *upn_ptr;

    if (upn_bytes > TAZ_RUN_AS_WIN32_UNICODE_STRING_MAX_BYTES)
    {
        return 0U;
    }
    need = header + upn_bytes;
    if ((buf == NULL) || (bufsize < need))
    {
        return 0U;
    }

    base = (unsigned char *)buf;
    msg = (KERB_S4U_LOGON *)buf;
    msg->MessageType = KerbS4ULogon;
    msg->Flags = 0UL;

    upn_ptr = base + header;
    /* Not NUL-terminated by design - see the MSV1_0_S4U_LOGON copy above. */
    // NOLINTNEXTLINE(bugprone-not-null-terminated-result)
    memcpy(upn_ptr, upn, upn_bytes);
    msg->ClientUpn.Buffer = (PWSTR)upn_ptr;
    msg->ClientUpn.Length = (USHORT)upn_bytes;
    msg->ClientUpn.MaximumLength = (USHORT)upn_bytes;

    msg->ClientRealm.Buffer = NULL;
    msg->ClientRealm.Length = 0;
    msg->ClientRealm.MaximumLength = 0;

    return need;
}

/* ---------------------------------------------------------------------
 * L4-L7 - status/error mapping
 * --------------------------------------------------------------------- */

taz_v1_ErrorCode taz_run_as_win32_map_status(unsigned long ntstatus)
{
    switch (ntstatus)
    {
        case STATUS_NO_SUCH_USER:
        case STATUS_NO_SUCH_DOMAIN:
        case STATUS_LOGON_FAILURE:
        case STATUS_NO_TRUST_SAM_ACCOUNT:
            return taz_v1_ErrorCode_ERROR_CODE_NOT_FOUND;
        case STATUS_LOGON_TYPE_NOT_GRANTED:
        case STATUS_ACCOUNT_DISABLED:
        case STATUS_ACCOUNT_LOCKED_OUT:
        case STATUS_ACCOUNT_RESTRICTION:
        case STATUS_ACCOUNT_EXPIRED:
        case STATUS_PASSWORD_EXPIRED:
        case STATUS_PASSWORD_MUST_CHANGE:
        case STATUS_INVALID_LOGON_HOURS:
        case STATUS_INVALID_WORKSTATION:
        case STATUS_PRIVILEGE_NOT_HELD:
        case STATUS_ACCESS_DENIED:
            return taz_v1_ErrorCode_ERROR_CODE_PERMISSION_DENIED;
        default:
            return taz_v1_ErrorCode_ERROR_CODE_INTERNAL;
    }
}

taz_v1_ErrorCode taz_run_as_win32_map_dup_error(unsigned long win32_error)
{
    switch (win32_error)
    {
        case ERROR_BAD_IMPERSONATION_LEVEL:
        case ERROR_PRIVILEGE_NOT_HELD:
        case ERROR_ACCESS_DENIED:
            return taz_v1_ErrorCode_ERROR_CODE_PERMISSION_DENIED;
        default:
            return taz_v1_ErrorCode_ERROR_CODE_INTERNAL;
    }
}

/* ---------------------------------------------------------------------
 * L9 - message text
 * --------------------------------------------------------------------- */

/* Looks up win32_error's system text, strips the trailing CR/LF
 * FormatMessageW always appends, converts to sanitized UTF-8. Leaves out
 * as "" on any failure. */
static void format_win32_text(unsigned long win32_error, char *out,
                              size_t out_size)
{
    LPWSTR wide = NULL;
    DWORD wide_len;
    char utf8[256];
    int mbc;

    if (out_size == 0U)
    {
        return;
    }
    out[0] = '\0';

    wide_len = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER |
                                  FORMAT_MESSAGE_FROM_SYSTEM |
                                  FORMAT_MESSAGE_IGNORE_INSERTS,
                              NULL, win32_error, 0, (LPWSTR)&wide, 0, NULL);
    if ((wide_len == 0U) || (wide == NULL))
    {
        return;
    }

    while ((wide_len > 0U) &&
           ((wide[wide_len - 1U] == L'\n') || (wide[wide_len - 1U] == L'\r')))
    {
        wide_len--;
    }
    wide[wide_len] = L'\0';

    mbc = WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, (int)sizeof(utf8),
                              NULL, NULL);
    (void)LocalFree(wide);
    if (mbc > 0)
    {
        taz_fsutil_sanitize_utf8(utf8, (size_t)mbc - 1U, out, out_size);
    }
}

/* Builds "<api>: <win32 text>" into message, truncated to message_size.
 * No-op when message is NULL or message_size is 0. */
static void format_api_message(const char *api, unsigned long win32_error,
                               char *message, size_t message_size)
{
    char text[TAZ_RUN_AS_WIN32_WIN32_TEXT_MAX];

    if ((message == NULL) || (message_size == 0U))
    {
        return;
    }
    format_win32_text(win32_error, text, sizeof(text));
    (void)snprintf(message, message_size, "%s: %s", api, text);
}

/* Builds "LsaLogonUser(<package>): 0x%08lX (<win32 text>)" - the raw
 * status always appears, per table L9, even when FormatMessageW itself
 * cannot translate it. */
static void format_lsa_message(const char *package, unsigned long ntstatus,
                               char *message, size_t message_size)
{
    char text[TAZ_RUN_AS_WIN32_WIN32_TEXT_MAX];

    if ((message == NULL) || (message_size == 0U))
    {
        return;
    }
    format_win32_text(LsaNtStatusToWinError((NTSTATUS)ntstatus), text,
                      sizeof(text));
    (void)snprintf(message, message_size, "LsaLogonUser(%s): 0x%08lX (%s)",
                   package, ntstatus, text);
}

void taz_run_as_win32_format_error(const char *api, unsigned long win32_error,
                                   char *message, size_t message_size)
{
    format_api_message(api, win32_error, message, message_size);
}

static void set_message(char *message, size_t message_size, const char *text)
{
    if ((message == NULL) || (message_size == 0U))
    {
        return;
    }
    (void)snprintf(message, message_size, "%s", text);
}

/* ---------------------------------------------------------------------
 * The real logon (L1-L8)
 * --------------------------------------------------------------------- */

static void get_computer_name_utf8(char *buf, size_t bufsize)
{
    WCHAR wide[64];
    DWORD len = (DWORD)(sizeof(wide) / sizeof(wide[0]));
    int mbc;

    buf[0] = '\0';
    if (!GetComputerNameExW(ComputerNameNetBIOS, wide, &len))
    {
        return;
    }
    mbc = WideCharToMultiByte(CP_UTF8, 0, wide, -1, buf, (int)bufsize, NULL,
                              NULL);
    if (mbc <= 0)
    {
        buf[0] = '\0';
    }
}

/* Converts a UTF-8 name (bounded by TAZ_EXEC_USER_MAX) into a fixed wide
 * buffer. Returns 0 if the conversion fails or does not fit. */
static int utf8_to_wide_fixed(const char *utf8, WCHAR *out, size_t out_count)
{
    int needed = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);

    if ((needed <= 0) || ((size_t)needed > out_count))
    {
        return 0;
    }
    return MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out, (int)out_count) > 0;
}

static int real_logon(const char *name, void **token, taz_v1_ErrorCode *code,
                      char *message, size_t message_size)
{
    char computer_name[TAZ_EXEC_USER_MAX];
    taz_run_as_name_t parsed;
    unsigned char submit_buf[1024];
    size_t submit_len;
    char package[64];
    WCHAR user_w[TAZ_EXEC_USER_MAX];
    WCHAR domain_w[TAZ_EXEC_USER_MAX];
    HANDLE lsa_handle = NULL;
    LSA_STRING package_str;
    ULONG auth_package = 0UL;
    LSA_STRING origin;
    char origin_name[] = "tazd";
    TOKEN_SOURCE source;
    PVOID profile_buffer = NULL;
    ULONG profile_buffer_len = 0UL;
    LUID logon_id = {0};
    HANDLE impersonation_token = NULL;
    HANDLE primary_token = NULL;
    QUOTA_LIMITS quotas = {0};
    NTSTATUS sub_status = 0;
    NTSTATUS status;
    TOKEN_TYPE token_type = TokenPrimary;
    DWORD type_len = (DWORD)sizeof(token_type);

    get_computer_name_utf8(computer_name, sizeof(computer_name));

    if (taz_run_as_parse_name(name, computer_name, &parsed) != 0)
    {
        *code = taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST;
        set_message(message, message_size, "invalid user name");
        return -1;
    }

    if (parsed.kind == TAZ_RUN_AS_NAME_LOCAL)
    {
        if (!utf8_to_wide_fixed(parsed.user, user_w, TAZ_EXEC_USER_MAX) ||
            !utf8_to_wide_fixed(parsed.domain, domain_w, TAZ_EXEC_USER_MAX))
        {
            *code = taz_v1_ErrorCode_ERROR_CODE_INTERNAL;
            set_message(message, message_size, "user name is not valid UTF-16");
            return -1;
        }
        submit_len = taz_run_as_win32_msv_s4u_layout(
            user_w, domain_w, submit_buf, sizeof(submit_buf));
        (void)snprintf(package, sizeof(package), "%s", MSV1_0_PACKAGE_NAME);
    }
    else
    {
        if (!utf8_to_wide_fixed(parsed.user, user_w, TAZ_EXEC_USER_MAX))
        {
            *code = taz_v1_ErrorCode_ERROR_CODE_INTERNAL;
            set_message(message, message_size, "user name is not valid UTF-16");
            return -1;
        }
        submit_len = taz_run_as_win32_kerb_s4u_layout(user_w, submit_buf,
                                                      sizeof(submit_buf));
        (void)snprintf(package, sizeof(package), "%s",
                       MICROSOFT_KERBEROS_NAME_A);
    }

    if (submit_len == 0U)
    {
        *code = taz_v1_ErrorCode_ERROR_CODE_INTERNAL;
        set_message(message, message_size, "S4U submit buffer too small");
        return -1;
    }

    status = LsaConnectUntrusted(&lsa_handle);
    if (status != STATUS_SUCCESS)
    {
        *code = taz_run_as_win32_map_status((unsigned long)status);
        format_lsa_message(package, (unsigned long)status, message,
                           message_size);
        return -1;
    }

    package_str.Buffer = package;
    package_str.Length = (USHORT)strlen(package);
    package_str.MaximumLength = (USHORT)(strlen(package) + 1U);

    status =
        LsaLookupAuthenticationPackage(lsa_handle, &package_str, &auth_package);
    if (status != STATUS_SUCCESS)
    {
        *code = taz_run_as_win32_map_status((unsigned long)status);
        format_lsa_message(package, (unsigned long)status, message,
                           message_size);
        (void)LsaDeregisterLogonProcess(lsa_handle);
        return -1;
    }

    origin.Buffer = origin_name;
    origin.Length = (USHORT)(sizeof(origin_name) - 1U);
    origin.MaximumLength = (USHORT)sizeof(origin_name);

    memset(&source, 0, sizeof(source));
    memcpy(source.SourceName, origin_name, sizeof(origin_name) - 1U);
    if (!AllocateLocallyUniqueId(&source.SourceIdentifier))
    {
        *code = taz_v1_ErrorCode_ERROR_CODE_INTERNAL;
        format_api_message("AllocateLocallyUniqueId", GetLastError(), message,
                           message_size);
        (void)LsaDeregisterLogonProcess(lsa_handle);
        return -1;
    }

    status = LsaLogonUser(lsa_handle, &origin, Network, auth_package,
                          submit_buf, (ULONG)submit_len, NULL, &source,
                          &profile_buffer, &profile_buffer_len, &logon_id,
                          &impersonation_token, &quotas, &sub_status);
    (void)LsaDeregisterLogonProcess(lsa_handle);

    if (status != STATUS_SUCCESS)
    {
        *code = taz_run_as_win32_map_status((unsigned long)status);
        format_lsa_message(package, (unsigned long)status, message,
                           message_size);
        return -1;
    }

    if (profile_buffer != NULL)
    {
        (void)LsaFreeReturnBuffer(profile_buffer);
    }

    if (!DuplicateTokenEx(impersonation_token, TOKEN_ALL_ACCESS, NULL,
                          SecurityImpersonation, TokenPrimary, &primary_token))
    {
        const DWORD dup_error = GetLastError();

        (void)CloseHandle(impersonation_token);
        *code = taz_run_as_win32_map_dup_error((unsigned long)dup_error);
        format_api_message("DuplicateTokenEx", (unsigned long)dup_error,
                           message, message_size);
        return -1;
    }
    (void)CloseHandle(impersonation_token);

    if (!GetTokenInformation(primary_token, TokenType, &token_type, type_len,
                             &type_len) ||
        (token_type != TokenPrimary))
    {
        (void)CloseHandle(primary_token);
        *code = taz_v1_ErrorCode_ERROR_CODE_INTERNAL;
        set_message(message, message_size,
                    "DuplicateTokenEx did not produce a primary token");
        return -1;
    }

    *token = (void *)primary_token;
    return 0;
}

int taz_run_as_win32_logon(const char *name, void **token,
                           taz_v1_ErrorCode *code, char *message,
                           size_t message_size)
{
    *token = NULL;
    if (message_size > 0U)
    {
        message[0] = '\0';
    }

    if (g_logon_override != NULL)
    {
        return g_logon_override(name, token, code, message, message_size);
    }
    return real_logon(name, token, code, message, message_size);
}

void taz_run_as_win32_set_logon_for_tests(taz_run_as_win32_logon_fn_t fn)
{
    g_logon_override = fn;
}

#endif /* _WIN32 */
