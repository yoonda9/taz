#include "taz/restart_args.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAZ_RESTART_ARGS_PORT_FLAG "--port"
#define TAZ_RESTART_ARGS_EXTRA_SLOTS                                           \
    3U /* exepath, "--port", NUL terminator                                    \
        * (the appended-port value shares                                      \
        * one of these three) */
#define TAZ_RESTART_ARGS_PORT_DIGITS                                           \
    16U /* fits any 32-bit int plus sign                                       \
         * and NUL with room to spare */

static char *dup_str(const char *s)
{
    size_t len = strlen(s) + 1U;
    char *copy = (char *)malloc(len);

    if (copy != NULL)
    {
        (void)memcpy(copy, s, len);
    }
    return copy;
}

static char *port_str(int port)
{
    char *buf = (char *)malloc(TAZ_RESTART_ARGS_PORT_DIGITS);

    if (buf != NULL)
    {
        (void)snprintf(buf, TAZ_RESTART_ARGS_PORT_DIGITS, "%d", port);
    }
    return buf;
}

static void free_built(char **argv, size_t count)
{
    size_t i;

    if (argv == NULL)
    {
        return;
    }
    for (i = 0U; i < count; i++)
    {
        free(argv[i]);
    }
    free((void *)argv);
}

void taz_restart_args_free(char **argv)
{
    size_t count = 0U;

    if (argv == NULL)
    {
        return;
    }
    while (argv[count] != NULL)
    {
        count++;
    }
    free_built(argv, count);
}

void taz_restart_exepath_strip_deleted(char *exepath)
{
    static const char suffix[] = " (deleted)";
    const size_t suffix_len = sizeof(suffix) - 1U;
    const size_t len = strlen(exepath);

    if (len > suffix_len && strcmp(exepath + (len - suffix_len), suffix) == 0)
    {
        exepath[len - suffix_len] = '\0';
    }
}

/* Appends a single built entry, freeing everything built so far and
 * returning 0 on an allocation failure so every call site can just
 * `if (!append(...)) goto fail;`-style bail without repeating the cleanup. */
static int append(char **built, size_t *n, char *entry)
{
    if (entry == NULL)
    {
        free_built(built, *n);
        return 0;
    }
    built[*n] = entry;
    (*n)++;
    return 1;
}

int taz_restart_args_build(int argc, char *const argv[], const char *exepath,
                           int bound_port, char ***out_argv)
{
    size_t max_count =
        (size_t)((argc > 0) ? argc : 1) + TAZ_RESTART_ARGS_EXTRA_SLOTS;
    char **built = (char **)calloc(max_count, sizeof(char *));
    size_t n = 0U;
    int port_handled = 0;
    int i;

    if (built == NULL)
    {
        return -1;
    }

    if (!append(built, &n, dup_str(exepath)))
    {
        return -1;
    }

    for (i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], TAZ_RESTART_ARGS_PORT_FLAG) == 0)
        {
            if (i + 1 >= argc)
            {
                free_built(built, n);
                return -1;
            }
            if (!append(built, &n, dup_str(TAZ_RESTART_ARGS_PORT_FLAG)) ||
                !append(built, &n, port_str(bound_port)))
            {
                return -1;
            }
            port_handled = 1;
            i++; /* skip the original value: already substituted above */
            continue;
        }
        if (!append(built, &n, dup_str(argv[i])))
        {
            return -1;
        }
    }

    if (!port_handled)
    {
        if (!append(built, &n, dup_str(TAZ_RESTART_ARGS_PORT_FLAG)) ||
            !append(built, &n, port_str(bound_port)))
        {
            return -1;
        }
    }

    built[n] = NULL;
    *out_argv = built;
    return 0;
}
