#include "taz/run_as.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "taz/fsutil.h"

#ifndef _WIN32
#include <unistd.h>

#define TAZ_RUN_AS_DEFAULT_PASSWD_PATH TAZ_PASSWD_PATH
#else
#include <windows.h>

#include "taz/run_as_win32.h"

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
    return taz_run_as_win32_privileged();
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
    taz_run_as_win32_daemon_user(buf, bufsize);
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

int taz_run_as_resolve(const char *name, taz_run_as_identity_t *out,
                       taz_v1_ErrorCode *code, char *message,
                       size_t message_size)
{
    if (message_size > 0U)
    {
        message[0] = '\0';
    }

#ifndef _WIN32
    {
        unsigned long uid = 0UL;
        unsigned long gid = 0UL;
        char home[TAZ_EXEC_HOME_MAX];

        if (!taz_passwd_lookup_by_name(g_passwd_path, name, &uid, &gid, home,
                                       sizeof(home)))
        {
            *code = taz_v1_ErrorCode_ERROR_CODE_NOT_FOUND;
            return -1;
        }
        out->uid = (uint32_t)uid;
        out->gid = (uint32_t)gid;
        (void)snprintf(out->home, sizeof(out->home), "%s", home);
        out->token = NULL;
    }
#else
    {
        void *token = NULL;

        out->uid = 0U;
        out->gid = 0U;
        out->home[0] = '\0';
        if (taz_run_as_win32_logon(name, &token, code, message, message_size) !=
            0)
        {
            return -1;
        }
        out->token = token;
    }
#endif

    taz_fsutil_sanitize_utf8(name, strlen(name), out->user, sizeof(out->user));
    return 0;
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
