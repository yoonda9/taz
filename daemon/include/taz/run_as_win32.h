#ifndef TAZ_RUN_AS_WIN32_H
#define TAZ_RUN_AS_WIN32_H

/* Windows-only RUN_AS/as_user support: the privilege probe, the daemon's
 * own account name, and the S4U logon that resolves a name to a primary
 * token. Implemented in src/platform/run_as_win32.c, compiled into
 * taz_core only on Windows (daemon/CMakeLists.txt's if(WIN32) split, the
 * same one process_win32.c uses) - this header is declared only under
 * _WIN32 so nothing on POSIX can see or call into it. run_as.c's Windows
 * branch and test_run_as_win32.cpp (both Windows-only) are its only
 * callers. */
#ifdef _WIN32

#include <stddef.h>
#include <wchar.h>

#include "taz/v1/common.pb.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /* True when this process holds every privilege a real RUN_AS/as_user
     * switch needs - SeTcbPrivilege, SeAssignPrimaryTokenPrivilege,
     * SeIncreaseQuotaPrivilege, SeBackupPrivilege, SeRestorePrivilege -
     * enabling each one that is present but currently disabled
     * (AdjustTokenPrivileges; GetLastError() != ERROR_NOT_ALL_ASSIGNED
     * decides "present"). Probed once and cached for the process
     * lifetime. taz_run_as_privileged() and its test seam
     * (taz_run_as_set_privileged_for_tests) are what every other caller
     * should use; this is the real, uncached answer the seam overrides. */
    int taz_run_as_win32_privileged(void);

    /* Copies the process token's own account, "DOMAIN\\name" (e.g.
     * "NT AUTHORITY\\SYSTEM" under SYSTEM), into buf, NUL-terminated;
     * "" if the lookup fails (never expected on a real host). No-op when
     * bufsize is 0. */
    void taz_run_as_win32_daemon_user(char *buf, size_t bufsize);

    /* Resolves name (any form taz_run_as_parse_name accepts) to a primary
     * token through an S4U logon: MSV1_0_S4U_LOGON for a LOCAL name,
     * KERB_S4U_LOGON for a DOMAIN one, both submitted with the Network
     * logon type. On success returns 0 and *token is a HANDLE cast to
     * void *, confirmed TokenPrimary, owned by the caller (close with
     * taz_run_as_token_close). On failure returns -1, *token is NULL,
     * *code is set to a defined taz_v1_ErrorCode and message
     * holds a human-readable, non-empty reason naming the failing LSA/
     * Win32 call and its raw status. name, token, code and message must
     * not be NULL; message_size must be at least 1.
     * taz_run_as_win32_set_logon_for_tests replaces this wholesale for
     * unit tests. */
    int taz_run_as_win32_logon(const char *name, void **token,
                               taz_v1_ErrorCode *code, char *message,
                               size_t message_size);

    /* Builds an MSV1_0_S4U_LOGON submit buffer for LsaLogonUser: the
     * struct at offset 0, with user then domain copied right after it so
     * every UNICODE_STRING.Buffer pointer stays inside this one buffer
     * (LsaLogonUser copies the whole submit buffer into the LSA process,
     * so no pointer in it may point outside it). Returns the number of
     * bytes used, or 0 if bufsize is too small or either string would
     * overflow a UNICODE_STRING's 16-bit Length (buf is left untouched in
     * either case). */
    size_t taz_run_as_win32_msv_s4u_layout(const wchar_t *user,
                                           const wchar_t *domain, void *buf,
                                           size_t bufsize);

    /* Same layout discipline for KERB_S4U_LOGON: ClientUpn = upn, then
     * ClientRealm = realm right after it, or an empty ClientRealm when
     * realm is NULL or "". */
    size_t taz_run_as_win32_kerb_s4u_layout(const wchar_t *upn,
                                            const wchar_t *realm, void *buf,
                                            size_t bufsize);

    /* Picks KERB_S4U_LOGON's ClientUpn/ClientRealm for a DOMAIN-kind name
     * (taz_run_as_parse_name's user and domain): "user@realm" (domain "")
     * is already a UPN; for "DOMAIN\\user" the UPN the directory
     * translated it to (translated, NULL when translation failed) wins,
     * else the bare user with DOMAIN as the realm. *realm is NULL when
     * ClientRealm stays empty. */
    void taz_run_as_win32_kerb_names(const wchar_t *user, const wchar_t *domain,
                                     const wchar_t *translated,
                                     const wchar_t **upn,
                                     const wchar_t **realm);

    /* Maps an LsaLogonUser NTSTATUS to a wire error code: a
     * not-found-shaped status -> NOT_FOUND, a permission-shaped one ->
     * PERMISSION_DENIED, anything else -> INTERNAL. ntstatus is passed as
     * unsigned long (NTSTATUS's underlying width) so this header never
     * needs <ntstatus.h>'s own typedef. */
    taz_v1_ErrorCode taz_run_as_win32_map_status(unsigned long ntstatus);

    /* Maps a DuplicateTokenEx GetLastError() to a wire error code: an
     * impersonation-level/privilege/access-denied failure ->
     * PERMISSION_DENIED, anything else -> INTERNAL. */
    taz_v1_ErrorCode taz_run_as_win32_map_dup_error(unsigned long win32_error);

    /* Builds "<api>: <win32 text>" into message, in the same style as
     * the logon's own LSA messages, truncated to message_size; no-op when
     * message is NULL or message_size is 0. Exposed so any caller that fails a
     * plain Win32 API (not an LSA call - see format_lsa_message, kept
     * file-local since only the logon itself produces an NTSTATUS) can report
     * it the same way taz_run_as_win32_logon's own DuplicateTokenEx failure
     * does. */
    void taz_run_as_win32_format_error(const char *api,
                                       unsigned long win32_error, char *message,
                                       size_t message_size);

    typedef int (*taz_run_as_win32_logon_fn_t)(const char *, void **,
                                               taz_v1_ErrorCode *, char *,
                                               size_t);

    /* Test-only seam: fn replaces taz_run_as_win32_logon's real body
     * wholesale; NULL restores it. Process-global state, like
     * taz_run_as_set_privileged_for_tests - callers must restore NULL
     * when done, since this is shared by every test in the binary. */
    void taz_run_as_win32_set_logon_for_tests(taz_run_as_win32_logon_fn_t fn);

#ifdef __cplusplus
}
#endif

#endif /* _WIN32 */

#endif /* TAZ_RUN_AS_WIN32_H */
