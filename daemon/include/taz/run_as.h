#ifndef TAZ_RUN_AS_H
#define TAZ_RUN_AS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /* True when this process can switch a spawned child's identity: POSIX
     * effective uid 0. Windows RUN_AS is deferred, so this is always false
     * there. run_as.c is the only place in this codebase allowed to call
     * geteuid(). */
    int taz_run_as_privileged(void);

    /* The /etc/passwd-style file RUN_AS resolves usernames against.
     * Defaults to TAZ_PASSWD_PATH on POSIX; on Windows taz_run_as_privileged
     * is always false, so nothing ever reads this path there. */
    const char *taz_run_as_passwd_path(void);

    /* Copy the daemon's own user name (its effective uid, resolved through
     * taz_run_as_passwd_path(), or the decimal uid when no line matches)
     * into buf, NUL-terminated. What RUN_AS reports after a reset. Always
     * "" on Windows, where RUN_AS is deferred. */
    void taz_run_as_daemon_user(char *buf, size_t bufsize);

    /* Test-only override for taz_run_as_privileged()'s POSIX answer, since
     * no gate runs as root: 1 or 0 forces it, -1 restores the real
     * geteuid() answer. A no-op on Windows (RUN_AS is deferred there
     * regardless). Callers must restore -1 when done, since this is
     * process-global state shared by every test in the binary. */
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
