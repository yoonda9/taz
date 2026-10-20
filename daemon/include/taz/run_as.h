#ifndef TAZ_RUN_AS_H
#define TAZ_RUN_AS_H

#include <stddef.h>
#include <stdint.h>

#include "taz/dispatch.h"
#include "taz/exec.h"
#include "taz/v1/common.pb.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /* Which half of a parsed RUN_AS/as_user name taz_run_as_parse_name found:
     * LOCAL names resolve against this machine's own accounts (passwd on
     * POSIX, the local SAM via MSV1_0_S4U_LOGON on Windows); DOMAIN names
     * need a domain controller (KERB_S4U_LOGON on Windows; POSIX never
     * resolves one, since passwd has no notion of a domain). */
    typedef enum
    {
        TAZ_RUN_AS_NAME_LOCAL = 0,
        TAZ_RUN_AS_NAME_DOMAIN = 1
    } taz_run_as_name_kind_t;

    typedef struct
    {
        taz_run_as_name_kind_t kind;
        /* The bare account name, except for "user@realm", which is kept
         * whole: it is already the UPN a Kerberos logon takes. */
        char user[TAZ_EXEC_USER_MAX];
        /* LOCAL: computer_name, copied through. DOMAIN: the domain part of
         * "DOMAIN\\user", or "" for "user@realm". */
        char domain[TAZ_EXEC_USER_MAX];
    } taz_run_as_name_t;

    /* Classify and split a RUN_AS/as_user name. Pure: no I/O, no platform
     * headers, same answer on every OS for the same inputs.
     *
     * Accepted forms:
     *   "user"             -> LOCAL,  user="user",        domain=computer_name
     *   ".\\user"          -> LOCAL,  user="user",        domain=computer_name
     *   "COMPUTER\\user"   -> LOCAL (COMPUTER case-insensitively ==
     *                         computer_name), user="user", domain=computer_name
     *   "DOMAIN\\user"     -> DOMAIN (COMPUTER did not match, or
     *                         computer_name is empty/NULL), user="user",
     *                         domain="DOMAIN"
     *   "user@realm"       -> DOMAIN, user=input verbatim, domain=""
     *
     * computer_name may be NULL or "" (nothing to match against): every
     * "X\\user" form is then DOMAIN, and a bare "user" is still LOCAL, with
     * domain="".
     *
     * Returns 0 on success. Returns -1, leaving *out untouched, for a
     * malformed name: empty; a leading or trailing '\\'; more than one
     * '\\'; both '\\' and '@' present; a leading or trailing '@'; or any
     * part that would not fit in TAZ_EXEC_USER_MAX bytes including the NUL. */
    int taz_run_as_parse_name(const char *input, const char *computer_name,
                              taz_run_as_name_t *out);

    /* The bare account name of any accepted RUN_AS/as_user form: "user"
     * from "user", ".\\user", "COMPUTER\\user", "DOMAIN\\user" or
     * "user@realm" (anything after the last '\\' and before the first
     * '@' after it). Copies it into buf, truncated to bufsize - 1 bytes,
     * and returns buf. Pure, like taz_run_as_parse_name. bufsize must be
     * at least 1. */
    char *taz_run_as_bare_user(const char *name, char *buf, size_t bufsize);

    /* A resolved RUN_AS/as_user identity: POSIX uid/gid plus the sanitized
     * username RUN_AS echoes back and the passwd home directory; token is
     * the Windows half (a primary HANDLE from an S4U logon), always NULL
     * on POSIX. Produced by taz_run_as_resolve, released by
     * taz_run_as_identity_close. */
    typedef struct
    {
        uint32_t uid;
        uint32_t gid;
        char user[TAZ_EXEC_USER_MAX];
        char home[TAZ_EXEC_HOME_MAX];
        void *token;
    } taz_run_as_identity_t;

    /* Closes id->token (NULL-safe, a no-op on POSIX) and clears it. The
     * rest of *id is left as is - callers that already copied uid/gid/
     * user/home elsewhere don't need this to touch them. */
    void taz_run_as_identity_close(taz_run_as_identity_t *id);

    /* Resolves a non-empty RUN_AS/as_user name into *out: POSIX looks it up
     * in taz_run_as_passwd_path() (NOT_FOUND on a miss; message left ""
     * either way, so the two existing callers keep their own literal
     * message); Windows runs an S4U logon (taz_run_as_win32_logon) and
     * fills message with the real LSA/Win32 failure text on failure. On success
     * returns 0, out->user holds the sanitized name, and (Windows only)
     * out->token is a primary HANDLE the caller owns (close with
     * taz_run_as_identity_close/taz_run_as_token_close). On failure returns -1,
     * *out is untouched, *code is a defined taz_v1_ErrorCode. Touches only the
     * global passwd path / the win32 module's own LSA calls, never a
     * taz_dispatch_t. name, out, code and message must not be NULL;
     * message_size must be at least 1. */
    int taz_run_as_resolve(const char *name, taz_run_as_identity_t *out,
                           taz_v1_ErrorCode *code, char *message,
                           size_t message_size);

    /* Duplicates a Windows primary token for a single spawn, so closing the
     * connection's own token later (taz_run_as_reset) can never pull it out
     * from under an in-flight child. Returns the duplicate, or NULL on
     * failure. Always NULL on POSIX, where there is no token to duplicate
     * (message is left untouched there - nothing ever fails). On a Windows
     * failure, message is filled with "DuplicateTokenEx: <Win32 text>";
     * message/message_size may be NULL/0 to discard it.
     * const: this only ever reads the handle value to hand to
     * DuplicateTokenEx, never writes through it. */
    void *taz_run_as_token_dup(const void *token, char *message,
                               size_t message_size);

    /* Closes a token from taz_run_as_token_dup/taz_run_as_resolve.
     * NULL-safe; a no-op on POSIX. const for the same reason as
     * taz_run_as_token_dup. */
    void taz_run_as_token_close(const void *token);

    /* Zeroes d's RUN_AS identity (run_as_active/uid/gid/user/home) and
     * closes and clears run_as_token. Called by the RUN_AS handler (before
     * a reset and before storing a newly resolved identity), by
     * connection.c's free path, and by tests' fixture teardown, so a
     * connection - real or faked - never outlives its token. */
    void taz_run_as_reset(taz_dispatch_t *d);

    /* True when this process can switch a spawned child's identity: POSIX
     * effective uid 0; Windows the cached privilege probe
     * (taz_run_as_win32_privileged - SeTcbPrivilege and friends). The
     * test seam below forces either answer on both platforms.
     * run_as.c is the only place in this codebase allowed to call
     * geteuid(). */
    int taz_run_as_privileged(void);

    /* The /etc/passwd-style file RUN_AS resolves usernames against.
     * Defaults to TAZ_PASSWD_PATH on POSIX; on Windows RUN_AS resolves
     * through an S4U logon instead, so nothing ever reads this path
     * there. */
    const char *taz_run_as_passwd_path(void);

    /* Copy the daemon's own user name into buf, NUL-terminated. What
     * RUN_AS reports after a reset. POSIX: the effective uid resolved
     * through taz_run_as_passwd_path(), or the decimal uid when no line
     * matches. Windows: the process token's own account,
     * "DOMAIN\\name" (taz_run_as_win32_daemon_user), "" only if that
     * lookup fails. */
    void taz_run_as_daemon_user(char *buf, size_t bufsize);

    /* Test-only override for taz_run_as_privileged()'s answer on either
     * platform, since the unit tests run neither as root nor as SYSTEM: 1
     * or 0 forces it, -1 restores the real answer (geteuid() on POSIX; the
     * SeTcbPrivilege probe on Windows). Callers must restore -1 when done,
     * since this is process-global state shared by every test in the binary. */
    void taz_run_as_set_privileged_for_tests(int privileged);

    /* Test-only override for taz_run_as_passwd_path()'s return value.
     * Passing NULL restores the platform default (TAZ_PASSWD_PATH on
     * POSIX, "" on Windows). Process-global state, same caveat as
     * taz_run_as_set_privileged_for_tests. */
    void taz_run_as_set_passwd_path_for_tests(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* TAZ_RUN_AS_H */
