#include "taz/run_as.h"

#include <stddef.h>

#ifndef _WIN32
#include <unistd.h>

#include "taz/fsutil.h"

#define TAZ_RUN_AS_DEFAULT_PASSWD_PATH TAZ_PASSWD_PATH
#else
#include <windows.h>

#define TAZ_RUN_AS_DEFAULT_PASSWD_PATH ""
#endif

/* -1 means "no override, use the real answer"; the test seam forces 0 or 1
 * and restores -1. Shared by both platforms since the seam now overrides
 * Windows too (the real Windows answer is still a constant 0 here). */
static int g_privileged_override = -1;

static const char *g_passwd_path = TAZ_RUN_AS_DEFAULT_PASSWD_PATH;

int taz_run_as_privileged(void)
{
    if (g_privileged_override >= 0)
    {
        return g_privileged_override;
    }
#ifndef _WIN32
    return geteuid() == 0 ? 1 : 0;
#else
    return 0;
#endif
}

const char *taz_run_as_passwd_path(void)
{
    return g_passwd_path;
}

void taz_run_as_daemon_user(char *buf, size_t bufsize)
{
    if (bufsize == 0U)
    {
        return;
    }
#ifndef _WIN32
    taz_user_name_from_uid(g_passwd_path, (unsigned long)geteuid(), buf,
                           bufsize);
#else
    buf[0] = '\0';
#endif
}

void taz_run_as_set_privileged_for_tests(int privileged)
{
    g_privileged_override = privileged;
}

void taz_run_as_set_passwd_path_for_tests(const char *path)
{
    g_passwd_path = (path != NULL) ? path : TAZ_RUN_AS_DEFAULT_PASSWD_PATH;
}

void *taz_run_as_token_dup(const void *token)
{
    if (token == NULL)
    {
        return NULL;
    }
#ifndef _WIN32
    return NULL;
#else
    HANDLE dup = NULL;

    if (!DuplicateTokenEx((HANDLE)token, TOKEN_ALL_ACCESS, NULL,
                          SecurityImpersonation, TokenPrimary, &dup))
    {
        return NULL;
    }
    return (void *)dup;
#endif
}

void taz_run_as_token_close(const void *token)
{
    if (token == NULL)
    {
        return;
    }
#ifdef _WIN32
    (void)CloseHandle((HANDLE)token);
#endif
}

void taz_run_as_identity_close(taz_run_as_identity_t *id)
{
    taz_run_as_token_close(id->token);
    id->token = NULL;
}

void taz_run_as_reset(taz_dispatch_t *d)
{
    taz_run_as_token_close(d->run_as_token);
    d->run_as_token = NULL;
    d->run_as_active = 0;
    d->run_as_uid = 0U;
    d->run_as_gid = 0U;
    d->run_as_user[0] = '\0';
    d->run_as_home[0] = '\0';
}
