#ifndef TAZ_CMDLINE_H
#define TAZ_CMDLINE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /* Join argv (NULL-terminated, argv[0] included) into a single Windows
     * command line, quoting each element exactly as CommandLineToArgvW's
     * documented algorithm expects: an element is left bare unless it is
     * empty or contains a space, tab or quote, in which case it is wrapped
     * in quotes and every quote is escaped, doubling any backslashes that
     * immediately precede a quote or the closing wrapper. Pure: no I/O, no
     * platform headers, same answer on every OS for the same inputs - used
     * by taz_platform_reexec on Windows and unit-tested on every platform.
     *
     * Elements are joined by a single space. Writes a NUL-terminated
     * result into out (capacity out_size) and returns 0 on success.
     * Returns -1 without writing past out_size when the result (including
     * the terminating NUL) would not fit; out's contents are then
     * unspecified up to out_size, and untouched beyond it. */
    int taz_cmdline_build(char *const argv[], char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* TAZ_CMDLINE_H */
