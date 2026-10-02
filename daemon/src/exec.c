#include "taz/exec.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Initial allocation for a stream buffer on its first append. */
#define TAZ_EXEC_CAPTURE_INITIAL_CAP 64U

/* Ensure *buf has room for at least `needed` bytes, growing by doubling
 * and never past `max_bytes` (the combined cap, which `needed` can never
 * exceed). Returns 0 on allocation failure, leaving *buf and *buf_cap
 * unchanged. */
static int grow(uint8_t **buf, size_t *buf_cap, size_t needed, size_t max_bytes)
{
    if (needed <= *buf_cap)
    {
        return 1;
    }

    size_t new_cap = (*buf_cap != 0U) ? *buf_cap : TAZ_EXEC_CAPTURE_INITIAL_CAP;
    while (new_cap < needed)
    {
        if (new_cap > SIZE_MAX / 2U)
        {
            new_cap = needed;
            break;
        }
        new_cap *= 2U;
    }
    if (new_cap > max_bytes)
    {
        new_cap = max_bytes;
    }

    uint8_t *grown = (uint8_t *)realloc(*buf, new_cap);
    if (grown == NULL)
    {
        return 0;
    }
    *buf = grown;
    *buf_cap = new_cap;
    return 1;
}

void taz_exec_capture_init(taz_exec_capture_t *cap, size_t max_bytes)
{
    cap->out = NULL;
    cap->out_len = 0U;
    cap->out_cap = 0U;
    cap->err = NULL;
    cap->err_len = 0U;
    cap->err_cap = 0U;
    cap->max_bytes = max_bytes;
    cap->truncated = 0;
}

void taz_exec_capture_append(taz_exec_capture_t *cap, taz_exec_stream_t which,
                             const void *data, size_t len)
{
    if (cap->truncated || len == 0U)
    {
        return;
    }

    size_t combined = cap->out_len + cap->err_len;
    size_t remaining =
        (combined < cap->max_bytes) ? cap->max_bytes - combined : 0U;
    if (remaining == 0U)
    {
        cap->truncated = 1;
        return;
    }

    size_t take = (len <= remaining) ? len : remaining;

    uint8_t **buf;
    size_t *buf_len;
    size_t *buf_cap;
    if (which == TAZ_EXEC_STREAM_OUT)
    {
        buf = &cap->out;
        buf_len = &cap->out_len;
        buf_cap = &cap->out_cap;
    }
    else
    {
        buf = &cap->err;
        buf_len = &cap->err_len;
        buf_cap = &cap->err_cap;
    }

    if (!grow(buf, buf_cap, *buf_len + take, cap->max_bytes))
    {
        cap->truncated = 1;
        return;
    }

    (void)memcpy(*buf + *buf_len, data, take);
    *buf_len += take;

    if (take < len)
    {
        cap->truncated = 1;
    }
}

void taz_exec_capture_free(taz_exec_capture_t *cap)
{
    free(cap->out);
    free(cap->err);
    cap->out = NULL;
    cap->out_len = 0U;
    cap->out_cap = 0U;
    cap->err = NULL;
    cap->err_len = 0U;
    cap->err_cap = 0U;
}
