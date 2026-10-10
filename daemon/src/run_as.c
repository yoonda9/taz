#include "taz/run_as.h"

#include <stddef.h>

#ifndef _WIN32
#include <unistd.h>

#include "taz/fsutil.h"

#define TAZ_RUN_AS_DEFAULT_PASSWD_PATH TAZ_PASSWD_PATH

/* -1 means "no override, use the real geteuid()"; the test seam forces 0 or
 * 1 and restores -1. */
static int g_privileged_override = -1;
#else
#define TAZ_RUN_AS_DEFAULT_PASSWD_PATH ""
#endif

static const char *g_passwd_path = TAZ_RUN_AS_DEFAULT_PASSWD_PATH;

int taz_run_as_privileged(void)
{
#ifndef _WIN32
    if (g_privileged_override >= 0)
    {
        return g_privileged_override;
    }
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
#ifndef _WIN32
    g_privileged_override = privileged;
#else
    (void)privileged;
#endif
}

void taz_run_as_set_passwd_path_for_tests(const char *path)
{
    g_passwd_path = (path != NULL) ? path : TAZ_RUN_AS_DEFAULT_PASSWD_PATH;
}
