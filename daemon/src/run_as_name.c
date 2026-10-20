#include <string.h>

#include "taz/run_as.h"

/* Local ASCII case fold, not _stricmp/strcasecmp: this file has no platform
 * headers and builds the same way on every OS. Computer names are ASCII
 * (NetBIOS names), so this is the whole alphabet that matters here. */
static int ascii_tolower(int c)
{
    if (c >= 'A' && c <= 'Z')
    {
        return c - 'A' + 'a';
    }
    return c;
}

/* True when part (bounded, not NUL-terminated) is the same length as
 * computer_name and matches it case-insensitively. False whenever
 * computer_name is NULL or empty: with nothing to match, every
 * "X\user" form falls through to DOMAIN. */
static int computer_matches(const char *part, size_t part_len,
                            const char *computer_name)
{
    size_t name_len;
    size_t i;

    if (computer_name == NULL || computer_name[0] == '\0')
    {
        return 0;
    }
    name_len = strlen(computer_name);
    if (name_len != part_len)
    {
        return 0;
    }
    for (i = 0U; i < part_len; i++)
    {
        if (ascii_tolower((unsigned char)part[i]) !=
            ascii_tolower((unsigned char)computer_name[i]))
        {
            return 0;
        }
    }
    return 1;
}

/* Copies len bytes of src plus a NUL into dst (size dst_size). -1 without
 * touching dst when it would not fit. */
static int copy_part(char *dst, size_t dst_size, const char *src, size_t len)
{
    if (len >= dst_size)
    {
        return -1;
    }
    (void)memcpy(dst, src, len);
    dst[len] = '\0';
    return 0;
}

int taz_run_as_parse_name(const char *input, const char *computer_name,
                          taz_run_as_name_t *out)
{
    const char *backslash;
    const char *at;
    taz_run_as_name_t result;

    if (input == NULL || input[0] == '\0')
    {
        return -1;
    }

    backslash = strchr(input, '\\');
    at = strchr(input, '@');

    if (backslash != NULL && at != NULL)
    {
        return -1; /* e.g. "a@b\c": neither form allows the other's mark */
    }

    if (at != NULL)
    {
        const size_t len = strlen(input);

        if (at == input || at[1] == '\0')
        {
            return -1; /* "@x" or "x@" */
        }
        if (copy_part(result.user, sizeof(result.user), input, len) != 0)
        {
            return -1;
        }
        result.kind = TAZ_RUN_AS_NAME_DOMAIN;
        result.domain[0] = '\0';
        *out = result;
        return 0;
    }

    if (backslash != NULL)
    {
        const char *computer_part = input;
        const size_t computer_len = (size_t)(backslash - input);
        const char *user_part = backslash + 1;
        const size_t user_len = strlen(user_part);
        int this_computer;

        if (computer_len == 0U || user_len == 0U)
        {
            return -1; /* "\alice" or "CORP\" */
        }
        if (strchr(user_part, '\\') != NULL)
        {
            return -1; /* "a\b\c": more than one backslash */
        }

        this_computer =
            (computer_len == 1U && computer_part[0] == '.') ||
            computer_matches(computer_part, computer_len, computer_name);

        if (this_computer)
        {
            const char *domain = (computer_name != NULL) ? computer_name : "";

            if (copy_part(result.user, sizeof(result.user), user_part,
                          user_len) != 0 ||
                copy_part(result.domain, sizeof(result.domain), domain,
                          strlen(domain)) != 0)
            {
                return -1;
            }
            result.kind = TAZ_RUN_AS_NAME_LOCAL;
            *out = result;
            return 0;
        }

        if (copy_part(result.user, sizeof(result.user), user_part, user_len) !=
                0 ||
            copy_part(result.domain, sizeof(result.domain), computer_part,
                      computer_len) != 0)
        {
            return -1;
        }
        result.kind = TAZ_RUN_AS_NAME_DOMAIN;
        *out = result;
        return 0;
    }

    /* Bare name: no backslash, no '@'. */
    {
        const char *domain = (computer_name != NULL) ? computer_name : "";

        if (copy_part(result.user, sizeof(result.user), input, strlen(input)) !=
                0 ||
            copy_part(result.domain, sizeof(result.domain), domain,
                      strlen(domain)) != 0)
        {
            return -1;
        }
        result.kind = TAZ_RUN_AS_NAME_LOCAL;
        *out = result;
        return 0;
    }
}

char *taz_run_as_bare_user(const char *name, char *buf, size_t bufsize)
{
    const char *start = strrchr(name, '\\');
    const char *at;
    size_t len;

    start = (start != NULL) ? start + 1 : name;
    at = strchr(start, '@');
    len = (at != NULL) ? (size_t)(at - start) : strlen(start);
    if (len >= bufsize)
    {
        len = bufsize - 1U;
    }
    (void)memcpy(buf, start, len);
    buf[len] = '\0';
    return buf;
}
