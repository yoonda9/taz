#include "taz/cmdline.h"

#include <string.h>

/* Windows command-line quoting, per CommandLineToArgvW's own documented
 * algorithm: a backslash is literal unless it immediately precedes a
 * quote, in which case every pair of backslashes collapses to one literal
 * backslash and an odd trailing backslash escapes the quote after it. */
static int arg_needs_quoting(const char *arg)
{
    const char *p = arg;

    if (*p == '\0')
    {
        return 1;
    }
    for (; *p != '\0'; p++)
    {
        if (*p == ' ' || *p == '\t' || *p == '"')
        {
            return 1;
        }
    }
    return 0;
}

static int append_quoted(char *out, size_t out_size, size_t *pos,
                         const char *arg)
{
    size_t backslashes;
    const char *p;

    if (!arg_needs_quoting(arg))
    {
        size_t len = strlen(arg);
        if (*pos + len >= out_size)
        {
            return -1;
        }
        /* out is a fixed command-line buffer under construction, not a
         * string yet at this point - taz_cmdline_build NUL-terminates it
         * once, after every token has been appended. */
        // NOLINTNEXTLINE(bugprone-not-null-terminated-result)
        (void)memcpy(out + *pos, arg, len);
        *pos += len;
        return 0;
    }

    if (*pos + 1U >= out_size)
    {
        return -1;
    }
    out[(*pos)++] = '"';

    backslashes = 0U;
    for (p = arg; *p != '\0'; p++)
    {
        if (*p == '\\')
        {
            backslashes++;
            continue;
        }
        if (*p == '"')
        {
            if (*pos + (backslashes * 2U) + 2U >= out_size)
            {
                return -1;
            }
            for (; backslashes > 0U; backslashes--)
            {
                out[(*pos)++] = '\\';
                out[(*pos)++] = '\\';
            }
            out[(*pos)++] = '\\';
            out[(*pos)++] = '"';
            continue;
        }
        if (backslashes > 0U)
        {
            if (*pos + backslashes >= out_size)
            {
                return -1;
            }
            for (; backslashes > 0U; backslashes--)
            {
                out[(*pos)++] = '\\';
            }
        }
        if (*pos + 1U >= out_size)
        {
            return -1;
        }
        out[(*pos)++] = *p;
    }
    if (backslashes > 0U)
    {
        if (*pos + (backslashes * 2U) >= out_size)
        {
            return -1;
        }
        for (; backslashes > 0U; backslashes--)
        {
            out[(*pos)++] = '\\';
            out[(*pos)++] = '\\';
        }
    }
    if (*pos + 1U >= out_size)
    {
        return -1;
    }
    out[(*pos)++] = '"';
    return 0;
}

int taz_cmdline_build(char *const argv[], char *out, size_t out_size)
{
    size_t pos = 0U;
    size_t i;

    for (i = 0U; argv[i] != NULL; i++)
    {
        if (i > 0U)
        {
            if (pos + 1U >= out_size)
            {
                return -1;
            }
            out[pos++] = ' ';
        }
        if (append_quoted(out, out_size, &pos, argv[i]) != 0)
        {
            return -1;
        }
    }
    if (pos >= out_size)
    {
        return -1;
    }
    out[pos] = '\0';
    return 0;
}
