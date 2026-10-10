#include "taz/fsutil.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <uv.h>

#ifdef _WIN32
#include <aclapi.h>
#include <windows.h>
#endif

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

char *taz_fsutil_dirname(const char *path)
{
    size_t root_len = taz_fsutil_root_prefix_len(path);
    size_t end = strlen(path);
    size_t out_len;
    char *out;

    while ((end > root_len) && taz_fsutil_is_sep(path[end - 1U]))
    {
        end--;
    }

    if (end > root_len)
    {
        size_t i = end;
        size_t sep_pos = 0U;
        int found_sep = 0;

        while (i > 0U)
        {
            i--;
            if (taz_fsutil_is_sep(path[i]))
            {
                sep_pos = i;
                found_sep = 1;
                break;
            }
        }

        /* A separator that is itself part of the root prefix (POSIX "/",
         * or a UNC root's trailing separator) means the whole path is
         * under the root with no further component boundary: dirname is
         * the root, kept with its own trailing separator. Any other
         * separator is a real component boundary: drop it. */
        if (!found_sep || ((sep_pos + 1U) <= root_len))
        {
            out_len = root_len;
        }
        else
        {
            out_len = sep_pos;
        }
    }
    else
    {
        /* Nothing left beyond the root prefix (e.g. "/" or ""). */
        out_len = root_len;
    }

    if (out_len == 0U)
    {
        out = (char *)malloc(2U);
        if (out == NULL)
        {
            return NULL;
        }
        out[0] = '.';
        out[1] = '\0';
        return out;
    }

    out = (char *)malloc(out_len + 1U);
    if (out == NULL)
    {
        return NULL;
    }
    (void)memcpy(out, path, out_len);
    out[out_len] = '\0';
    return out;
}

/* "<dest>.taz-<stream_id>.tmp": the FILE_PUT temp file name. */
#define TEMP_NAME_PREFIX ".taz-"
#define TEMP_NAME_SUFFIX ".tmp"

char *taz_fsutil_temp_name(const char *dest, uint32_t stream_id)
{
    char id_buf[sizeof("4294967295")]; /* decimal digits of UINT32_MAX + NUL */
    size_t dest_len = strlen(dest);
    size_t prefix_len = sizeof(TEMP_NAME_PREFIX) - 1U;
    size_t suffix_len = sizeof(TEMP_NAME_SUFFIX) - 1U;
    size_t id_len;
    size_t pos;
    char *out;

    (void)snprintf(id_buf, sizeof(id_buf), "%lu", (unsigned long)stream_id);
    id_len = strlen(id_buf);

    out = (char *)malloc(dest_len + prefix_len + id_len + suffix_len + 1U);
    if (out == NULL)
    {
        return NULL;
    }

    pos = 0U;
    (void)memcpy(out + pos, dest, dest_len);
    pos += dest_len;
    (void)memcpy(out + pos, TEMP_NAME_PREFIX, prefix_len);
    pos += prefix_len;
    (void)memcpy(out + pos, id_buf, id_len);
    pos += id_len;
    (void)memcpy(out + pos, TEMP_NAME_SUFFIX, suffix_len);
    pos += suffix_len;
    out[pos] = '\0';
    return out;
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

/* UTF-8 well-formedness table (Unicode Table 3-7): generic continuation
 * range, the three lead-byte lengths, and the two lead bytes (E0, ED, F0,
 * F4) whose second byte is narrower than the generic range. */
#define SANITIZE_ASCII_MAX     0x7FU
#define SANITIZE_CONT_MIN      0x80U
#define SANITIZE_CONT_MAX      0xBFU
#define SANITIZE_LEAD_2_MIN    0xC2U
#define SANITIZE_LEAD_2_MAX    0xDFU
#define SANITIZE_LEAD_3_MIN    0xE0U
#define SANITIZE_LEAD_3_MAX    0xEFU
#define SANITIZE_LEAD_E0       0xE0U
#define SANITIZE_LEAD_ED       0xEDU
#define SANITIZE_E0_SECOND_MIN 0xA0U
#define SANITIZE_ED_SECOND_MAX 0x9FU
#define SANITIZE_LEAD_4_MIN    0xF0U
#define SANITIZE_LEAD_4_MAX    0xF4U
#define SANITIZE_LEAD_F0       0xF0U
#define SANITIZE_LEAD_F4       0xF4U
#define SANITIZE_F0_SECOND_MIN 0x90U
#define SANITIZE_F4_SECOND_MAX 0x8FU
#define SANITIZE_SEQ_LEN_2     2U
#define SANITIZE_SEQ_LEN_3     3U
#define SANITIZE_SEQ_LEN_4     4U

/* U+FFFD REPLACEMENT CHARACTER, UTF-8 encoded. */
#define UTF8_REPLACEMENT_B0  0xEFU
#define UTF8_REPLACEMENT_B1  0xBFU
#define UTF8_REPLACEMENT_B2  0xBDU
#define UTF8_REPLACEMENT_LEN 3U

/* Classifies the unit starting at in[i] (0 < i + 1 <= len). Returns the
 * number of bytes it consumes and sets *valid. A well-formed codepoint
 * consumes its full encoded length; an ill-formed one consumes only its
 * "maximal subpart" per the Unicode standard: a lead byte together with
 * whatever leading continuation bytes already matched the expected range,
 * stopping at (and not consuming) the first byte that breaks the sequence
 * or at the end of input. */
static size_t sanitize_utf8_next(const unsigned char *in, size_t len, size_t i,
                                 int *valid)
{
    unsigned char b0 = in[i];
    size_t need;
    unsigned char second_min = SANITIZE_CONT_MIN;
    unsigned char second_max = SANITIZE_CONT_MAX;
    size_t have;
    unsigned char lo;
    unsigned char hi;

    if (b0 <= SANITIZE_ASCII_MAX)
    {
        *valid = 1;
        return 1U;
    }

    if ((b0 >= SANITIZE_LEAD_2_MIN) && (b0 <= SANITIZE_LEAD_2_MAX))
    {
        need = SANITIZE_SEQ_LEN_2;
    }
    else if ((b0 >= SANITIZE_LEAD_3_MIN) && (b0 <= SANITIZE_LEAD_3_MAX))
    {
        need = SANITIZE_SEQ_LEN_3;
        if (b0 == SANITIZE_LEAD_E0)
        {
            second_min = SANITIZE_E0_SECOND_MIN;
        }
        else if (b0 == SANITIZE_LEAD_ED)
        {
            second_max = SANITIZE_ED_SECOND_MAX;
        }
    }
    else if ((b0 >= SANITIZE_LEAD_4_MIN) && (b0 <= SANITIZE_LEAD_4_MAX))
    {
        need = SANITIZE_SEQ_LEN_4;
        if (b0 == SANITIZE_LEAD_F0)
        {
            second_min = SANITIZE_F0_SECOND_MIN;
        }
        else if (b0 == SANITIZE_LEAD_F4)
        {
            second_max = SANITIZE_F4_SECOND_MAX;
        }
    }
    else
    {
        /* A stray continuation byte (80-BF) or a lead byte that is never
         * valid (C0, C1, F5-FF). */
        *valid = 0;
        return 1U;
    }

    have = 1U;
    lo = second_min;
    hi = second_max;
    while (have < need)
    {
        if (((i + have) >= len) || (in[i + have] < lo) || (in[i + have] > hi))
        {
            *valid = 0;
            return have;
        }
        have++;
        lo = SANITIZE_CONT_MIN;
        hi = SANITIZE_CONT_MAX;
    }

    *valid = 1;
    return need;
}

void taz_fsutil_sanitize_utf8(const char *in, size_t in_len, char *buf,
                              size_t bufsize)
{
    const unsigned char *src = (const unsigned char *)in;
    size_t capacity;
    size_t i = 0U;
    size_t out = 0U;

    if (bufsize == 0U)
    {
        return;
    }
    capacity = bufsize - 1U;

    while (i < in_len)
    {
        int valid;
        size_t consumed = sanitize_utf8_next(src, in_len, i, &valid);
        const unsigned char *unit = valid ? (src + i) : NULL;
        unsigned char replacement[UTF8_REPLACEMENT_LEN];
        size_t unit_len = valid ? consumed : UTF8_REPLACEMENT_LEN;

        if (!valid)
        {
            replacement[0] = (unsigned char)UTF8_REPLACEMENT_B0;
            replacement[1] = (unsigned char)UTF8_REPLACEMENT_B1;
            replacement[2] = (unsigned char)UTF8_REPLACEMENT_B2;
            unit = replacement;
        }

        if ((out + unit_len) > capacity)
        {
            break;
        }
        (void)memcpy(buf + out, unit, unit_len);
        out += unit_len;
        i += consumed;
    }

    buf[out] = '\0';
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

int taz_passwd_lookup_by_name(const char *passwd_path, const char *name,
                              unsigned long *uid, unsigned long *gid,
                              char *home, size_t home_size)
{
    FILE *f;
    char line[PASSWD_LINE_MAX];
    size_t name_len = strlen(name);

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
        char *gid_start;
        char *gid_end;
        char *gecos_end;
        const char *home_start = "";
        size_t home_len = 0U;
        char *end;
        unsigned long line_uid;
        unsigned long line_gid;
        size_t field_name_len;

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
        field_name_len = (size_t)(name_end - line);
        if ((field_name_len != name_len) || (memcmp(line, name, name_len) != 0))
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

        gid_start = uid_end + 1;
        gid_end = strchr(gid_start, ':');
        if ((gid_end == NULL) || (gid_end == gid_start))
        {
            continue;
        }
        if ((gid_start[0] < '0') || (gid_start[0] > '9'))
        {
            continue;
        }

        gecos_end = strchr(gid_end + 1, ':');
        if (gecos_end != NULL)
        {
            const char *home_end;

            home_start = gecos_end + 1;
            home_end = home_start;
            while ((*home_end != '\0') && (*home_end != ':') &&
                   (*home_end != '\n'))
            {
                home_end++;
            }
            home_len = (size_t)(home_end - home_start);
        }
        if ((home != NULL) && (home_len >= home_size))
        {
            continue;
        }

        *uid_end = '\0';
        line_uid = strtoul(uid_start, &end, PASSWD_UID_BASE);
        if ((end != uid_end) || (line_uid > TAZ_PASSWD_ID_MAX))
        {
            continue; /* non-numeric or out-of-range uid field */
        }

        *gid_end = '\0';
        line_gid = strtoul(gid_start, &end, PASSWD_UID_BASE);
        if ((end != gid_end) || (line_gid > TAZ_PASSWD_ID_MAX))
        {
            continue; /* non-numeric or out-of-range gid field */
        }

        *uid = line_uid;
        *gid = line_gid;
        if (home != NULL)
        {
            (void)memcpy(home, home_start, home_len);
            home[home_len] = '\0';
        }
        (void)fclose(f);
        return 1;
    }

    (void)fclose(f);
    return 0;
}

#ifndef _WIN32
void taz_user_name_from_uid(const char *passwd_path, unsigned long uid,
                            char *buf, size_t bufsize)
{
    if (bufsize == 0U)
    {
        return;
    }

    if (!taz_passwd_name_from_uid(passwd_path, uid, buf, bufsize))
    {
        (void)snprintf(buf, bufsize, "%lu", uid);
    }
}
#else
void taz_win32_account_from_sid(const void *sid, char *buf, size_t bufsize)
{
    PSID psid = (PSID)sid;
    WCHAR name[256];
    WCHAR domain[256];
    DWORD name_len = (DWORD)(sizeof(name) / sizeof(name[0]));
    DWORD domain_len = (DWORD)(sizeof(domain) / sizeof(domain[0]));
    SID_NAME_USE use;
    char name_utf8[256];
    char domain_utf8[256];
    int name_mbc;
    int domain_mbc;

    if ((bufsize == 0U) || (sid == NULL))
    {
        return;
    }

    if (!LookupAccountSidW(NULL, psid, name, &name_len, domain, &domain_len,
                           &use))
    {
        return;
    }

    name_mbc = WideCharToMultiByte(CP_UTF8, 0, name, -1, name_utf8,
                                   (int)sizeof(name_utf8), NULL, NULL);
    domain_mbc = WideCharToMultiByte(CP_UTF8, 0, domain, -1, domain_utf8,
                                     (int)sizeof(domain_utf8), NULL, NULL);

    if ((name_mbc > 0) && (domain_mbc > 0))
    {
        char account[256 + 256 + 1]; /* domain\name + NUL */
        (void)snprintf(account, sizeof(account), "%s\\%s", domain_utf8,
                       name_utf8);
        taz_fsutil_sanitize_utf8(account, strlen(account), buf, bufsize);
    }
}
#endif /* _WIN32 */
