#include "taz/fsutil.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <uv.h>

/* Mode bits a Windows READONLY attribute cannot represent (DEC-005). */
#define MODE_READ_ALL          0444U  /* all three read bits */
#define MODE_SPECIAL_BITS      07000U /* setuid, setgid, sticky */
#define MODE_GROUP_OTHER_WRITE 0022U
#define MODE_OWNER_WRITE       0200U

/* UTF-8 continuation byte (10xxxxxx): mask selects the top two bits, tag is
 * the value they must have (DEC-009). */
#define UTF8_CONT_MASK 0xC0U
#define UTF8_CONT_TAG  0x80U

#ifdef _WIN32
#define FSUTIL_SEP '\\'
/* The Win32 long-path prefix. */
static const char WIN_LONGPATH_PREFIX[] = "\\\\?\\";
/* Length of the Win32 long-path prefix, excluding the NUL. */
#define WIN_LONGPATH_PREFIX_LEN (sizeof(WIN_LONGPATH_PREFIX) - 1U)
/* Length of "C:" plus one trailing separator. */
#define WIN_DRIVE_SEP_LEN 3U
#else
#define FSUTIL_SEP '/'
#endif

taz_v1_Kind taz_fsutil_kind_from_mode(uint64_t st_mode)
{
    switch (st_mode & (uint64_t)S_IFMT)
    {
        case S_IFREG:
            return taz_v1_Kind_KIND_FILE;
        case S_IFDIR:
            return taz_v1_Kind_KIND_DIR;
        case S_IFLNK:
            return taz_v1_Kind_KIND_SYMLINK;
        default:
            return taz_v1_Kind_KIND_OTHER;
    }
}

int taz_fsutil_is_hidden(const char *name)
{
    return (name != NULL) && (name[0] == '.');
}

char *taz_fsutil_join(const char *dir, const char *name)
{
    size_t dir_len = strlen(dir);
    size_t name_len = strlen(name);
    int need_sep = (dir_len > 0U) && !taz_fsutil_is_sep(dir[dir_len - 1U]);
    size_t sep_len = need_sep ? 1U : 0U;
    char *out = (char *)malloc(dir_len + sep_len + name_len + 1U);
    size_t pos;

    if (out == NULL)
    {
        return NULL;
    }

    (void)memcpy(out, dir, dir_len);
    pos = dir_len;
    if (need_sep)
    {
        out[pos] = FSUTIL_SEP;
        pos += 1U;
    }
    (void)memcpy(out + pos, name, name_len);
    pos += name_len;
    out[pos] = '\0';
    return out;
}

uint32_t taz_fsutil_chmod_unrepresentable(uint32_t mode)
{
    uint32_t result = 0U;

    if ((mode & MODE_READ_ALL) != MODE_READ_ALL)
    {
        /* No single bit identifies a missing read permission; flag it. */
        result |= 1U;
    }
    result |= (mode & MODE_SPECIAL_BITS);
    if (((mode & MODE_GROUP_OTHER_WRITE) != 0U) &&
        ((mode & MODE_OWNER_WRITE) == 0U))
    {
        result |= (mode & MODE_GROUP_OTHER_WRITE);
    }
    return result;
}

int taz_fsutil_is_sep(char c)
{
#ifdef _WIN32
    return (c == '/') || (c == '\\');
#else
    return c == '/';
#endif
}

#ifdef _WIN32

/* "C:" -> 2, "C:\" / "C:/" -> 3, anything else -> 0. */
static size_t win_drive_prefix_len(const char *path, size_t len)
{
    int is_letter;

    if (len < 2U)
    {
        return 0U;
    }
    is_letter = ((path[0] >= 'A') && (path[0] <= 'Z')) ||
                ((path[0] >= 'a') && (path[0] <= 'z'));
    if (!is_letter || (path[1] != ':'))
    {
        return 0U;
    }
    if ((len >= WIN_DRIVE_SEP_LEN) && taz_fsutil_is_sep(path[2]))
    {
        return WIN_DRIVE_SEP_LEN;
    }
    return 2U;
}

/* path[0] and path[1] are already known to be separators. Returns the
 * length through the share name's trailing separator, or 0 when the
 * server/share names are not both separator-terminated. */
static size_t win_unc_prefix_len(const char *path, size_t len)
{
    size_t i = 2U;
    size_t server_sep;
    size_t share_sep;

    while ((i < len) && !taz_fsutil_is_sep(path[i]))
    {
        i++;
    }
    if (i >= len)
    {
        return 0U;
    }
    server_sep = i;

    i = server_sep + 1U;
    while ((i < len) && !taz_fsutil_is_sep(path[i]))
    {
        i++;
    }
    if (i >= len)
    {
        return 0U;
    }
    share_sep = i;

    return share_sep + 1U;
}

#endif /* _WIN32 */

size_t taz_fsutil_root_prefix_len(const char *path)
{
    size_t len = strlen(path);

#ifdef _WIN32
    if ((len >= WIN_LONGPATH_PREFIX_LEN) &&
        (memcmp(path, WIN_LONGPATH_PREFIX, WIN_LONGPATH_PREFIX_LEN) == 0))
    {
        size_t drive_len = win_drive_prefix_len(path + WIN_LONGPATH_PREFIX_LEN,
                                                len - WIN_LONGPATH_PREFIX_LEN);
        return WIN_LONGPATH_PREFIX_LEN + drive_len;
    }

    {
        size_t drive_len = win_drive_prefix_len(path, len);
        if (drive_len > 0U)
        {
            return drive_len;
        }
    }

    if ((len >= 2U) && taz_fsutil_is_sep(path[0]) && taz_fsutil_is_sep(path[1]))
    {
        return win_unc_prefix_len(path, len);
    }
#endif

    if ((len >= 1U) && taz_fsutil_is_sep(path[0]))
    {
        return 1U;
    }
    return 0U;
}

void taz_fsutil_truncate_utf8(const char *name, char *buf, size_t bufsize)
{
    size_t len;

    if (bufsize == 0U)
    {
        return;
    }

    len = strlen(name);
    if (len >= bufsize)
    {
        len = bufsize - 1U;
        /* name[len] is the first byte that would be dropped; back up while
         * it is a UTF-8 continuation byte (10xxxxxx) so a multi-byte
         * codepoint is never split between the kept and dropped halves.
         * Already-invalid byte sequences (arbitrary bytes are a valid
         * POSIX filename) are trimmed the same way, which is harmless. */
        while ((len > 0U) &&
               (((unsigned char)name[len] & UTF8_CONT_MASK) == UTF8_CONT_TAG))
        {
            len--;
        }
    }
    (void)memcpy(buf, name, len);
    buf[len] = '\0';
}

/* strtoul base for the decimal uid field. */
#define PASSWD_UID_BASE 10U
/* Generous line length for a passwd entry; longer lines are skipped (they
 * cannot hold a plausible name:x:uid:... line that matters here). */
#define PASSWD_LINE_MAX 512U

int taz_passwd_name_from_uid(const char *passwd_path, unsigned long uid,
                             char *buf, size_t bufsize)
{
    FILE *f;
    char line[PASSWD_LINE_MAX];

    if (bufsize == 0U)
    {
        return 0;
    }

    f = fopen(passwd_path, "r");
    if (f == NULL)
    {
        return 0;
    }

    while (fgets(line, (int)sizeof(line), f) != NULL)
    {
        size_t line_len = strlen(line);
        int ends_in_newline = (line_len > 0U) && (line[line_len - 1U] == '\n');
        char *name_end;
        char *uid_start;
        char *uid_end;
        char *end;
        unsigned long line_uid;
        size_t name_len;

        if (!ends_in_newline && (feof(f) == 0))
        {
            /* The buffer filled before the real line ended; drain the rest
             * of it so its tail is never parsed as a fresh entry. */
            int c;
            int found_newline = 0;

            while ((c = fgetc(f)) != EOF)
            {
                if (c == '\n')
                {
                    found_newline = 1;
                    break;
                }
            }
            if (!found_newline)
            {
                /* The overlong line ran to EOF; nothing left to read. */
                break;
            }
            continue;
        }

        name_end = strchr(line, ':');
        if ((name_end == NULL) || (name_end == line))
        {
            continue;
        }
        uid_start = strchr(name_end + 1, ':');
        if (uid_start == NULL)
        {
            continue;
        }
        uid_start += 1;
        uid_end = strchr(uid_start, ':');
        if ((uid_end == NULL) || (uid_end == uid_start))
        {
            continue;
        }
        if ((uid_start[0] < '0') || (uid_start[0] > '9'))
        {
            continue; /* reject leading '-', '+', whitespace, etc. */
        }

        *uid_end = '\0';
        line_uid = strtoul(uid_start, &end, PASSWD_UID_BASE);
        if (end != uid_end)
        {
            continue; /* non-numeric uid field */
        }
        if (line_uid != uid)
        {
            continue;
        }

        name_len = (size_t)(name_end - line);
        if (name_len >= bufsize)
        {
            name_len = bufsize - 1U;
        }
        (void)memcpy(buf, line, name_len);
        buf[name_len] = '\0';
        (void)fclose(f);
        return 1;
    }

    (void)fclose(f);
    return 0;
}
