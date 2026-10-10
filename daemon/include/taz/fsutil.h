#ifndef TAZ_FSUTIL_H
#define TAZ_FSUTIL_H

#include <stddef.h>
#include <stdint.h>

#include "taz/v1/common.pb.h"

#ifndef _WIN32
#define TAZ_PASSWD_PATH "/etc/passwd"
#endif

#ifdef __cplusplus
extern "C"
{
#endif

    /* Map a uv_stat_t.st_mode value to a Kind via S_IFMT: REG->FILE,
     * DIR->DIR, LNK->SYMLINK, anything else->OTHER. */
    taz_v1_Kind taz_fsutil_kind_from_mode(uint64_t st_mode);

    /* True when name starts with '.' (and is not NULL/empty). */
    int taz_fsutil_is_hidden(const char *name);

    /* Heap-allocate dir + separator + name (no doubled separator when dir
     * already ends in one). Separator is '/' on POSIX, '\' on Windows.
     * Caller frees the result with free(). Returns NULL on OOM. */
    char *taz_fsutil_join(const char *dir, const char *name);

    /* Non-zero when a POSIX mode cannot be faithfully represented by
     * Windows' single owner-write (READONLY) attribute: missing a read
     * bit, a setuid/setgid/sticky bit, or a write bit for group/other
     * without owner-write. 0 when Windows can represent the mode exactly
     * enough (DEC-005). */
    uint32_t taz_fsutil_chmod_unrepresentable(uint32_t mode);

    /* True for a path separator: '/' everywhere, additionally '\' on
     * Windows. */
    int taz_fsutil_is_sep(char c);

    /* Length of the leading prefix of path that mkdir -p must never try to
     * create: "/" -> 1, "a/b" -> 0; on Windows additionally a drive
     * ("C:\" / "C:/" -> 3, "C:" -> 2), a long-path-prefixed drive
     * ("\\?\C:\" -> 7), or a UNC root (through the share's trailing
     * separator). 0 when path has no such prefix. */
    size_t taz_fsutil_root_prefix_len(const char *path);

    /* Heap-allocate the parent directory of path, the way POSIX dirname(3)
     * would, but without ever mutating or returning a pointer into path.
     * Trailing separators are ignored before splitting off the last
     * component. A root (POSIX "/", a Windows drive root, or a UNC root)
     * dirnames to itself, including its trailing separator. No separator at
     * all (after stripping trailing ones) dirnames to the root prefix if
     * there is one (e.g. "C:x" -> "C:"), otherwise to ".". Caller frees the
     * result with free(). Returns NULL on OOM. */
    char *taz_fsutil_dirname(const char *path);

    /* Heap-allocate "<dest>.taz-<stream_id>.tmp" (stream_id in decimal), the
     * FILE_PUT temp file name in dest's own directory. Caller frees the
     * result with free(). Returns NULL on OOM. */
    char *taz_fsutil_temp_name(const char *dest, uint32_t stream_id);

    /* Copies name into buf (NUL-terminated, at most bufsize - 1 bytes of
     * name). When name does not fit, the copy is trimmed at the last
     * complete UTF-8 codepoint boundary rather than mid-codepoint (DEC-009):
     * a long non-ASCII NTFS name can need up to 765 UTF-8 bytes for 255
     * UTF-16 units, well past DirEntry.name's 256-byte field. No-op when
     * bufsize is 0. */
    void taz_fsutil_truncate_utf8(const char *name, char *buf, size_t bufsize);

    /* Copy in[0..in_len) into buf as valid, NUL-terminated UTF-8 (at most
     * bufsize - 1 bytes): every maximal invalid subsequence (stray
     * continuation byte, invalid lead byte, truncated multi-byte sequence,
     * overlong encoding, UTF-16 surrogate, codepoint > U+10FFFF) becomes one
     * U+FFFD, following the Unicode "maximal subpart" practice. The copy is
     * cut at a codepoint boundary: a unit (valid codepoint or replacement)
     * that would not fit is dropped rather than split. No-op when
     * bufsize == 0; empty input copies to "". Pure, no allocation. */
    void taz_fsutil_sanitize_utf8(const char *in, size_t in_len, char *buf,
                                  size_t bufsize);

    /* Parse passwd_path (an /etc/passwd-style "name:x:uid:..." file) for
     * the line whose third field equals uid. On a match, copies the name
     * into buf (truncated, NUL-terminated, to bufsize) and returns 1.
     * Returns 0 when the file is missing or no line matches; malformed or
     * short lines are skipped. */
    int taz_passwd_name_from_uid(const char *passwd_path, unsigned long uid,
                                 char *buf, size_t bufsize);

/* Largest uid/gid taz_passwd_lookup_by_name accepts: anything above it
 * would wrap when narrowed to a 32-bit uid_t/gid_t (2^32 becomes 0, root),
 * and (uid_t)-1 means "unchanged" to the set*id calls. */
#define TAZ_PASSWD_ID_MAX 4294967294UL

    /* Parse passwd_path for the line whose first field equals name. On a
     * match, fills *uid and *gid from the uid (field 3) and primary gid
     * (field 4), copies the home directory (field 6, "" when absent) into
     * home unless home is NULL, and returns 1. Returns 0 when the file is
     * missing or no line matches. Malformed or short lines are skipped, as
     * are ids above TAZ_PASSWD_ID_MAX and lines whose home does not fit in
     * home_size (it is never truncated). */
    int taz_passwd_lookup_by_name(const char *passwd_path, const char *name,
                                  unsigned long *uid, unsigned long *gid,
                                  char *home, size_t home_size);

#ifndef _WIN32
    /* Wrapper for taz_passwd_name_from_uid that looks up uid in passwd_path,
     * falling back to decimal string representation of uid if not found or file
     * is missing. Always NUL-terminates buf when bufsize > 0. */
    void taz_user_name_from_uid(const char *passwd_path, unsigned long uid,
                                char *buf, size_t bufsize);
#else
/* Convert a Windows SID to "DOMAIN\name" format (sanitized UTF-8, truncated
 * at codepoint boundary). On any failure, leaves buf untouched. */
void taz_win32_account_from_sid(const void *sid, char *buf, size_t bufsize);
#endif

#ifdef __cplusplus
}
#endif

#endif /* TAZ_FSUTIL_H */
