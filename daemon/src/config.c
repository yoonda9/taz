#include "taz/config.h"

#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "taz/v1/daemon_control.pb.h"

/* Maximum length of a config value string (must fit in KeyValue.value[]) */
#define CONFIG_VAL_MAX 128U
/* strtoul base for decimal strings */
#define DECIMAL_BASE 10

/* Opaque storage for one config entry */
typedef struct
{
    const char *key;
    char value[CONFIG_VAL_MAX];
    const char *default_value;
} config_entry_t;

/* log.level valid tokens */
static const char *const LOG_LEVEL_VALUES[] = {"DEBUG", "INFO", "WARN", "ERROR",
                                               NULL};

/* compression valid tokens (only values advertised in CAPABILITY) */
static const char *const COMPRESSION_VALUES[] = {"NONE", NULL};

static config_entry_t g_config[] = {
    {"log.level", "INFO", "INFO"},
    {"compression", "NONE", "NONE"},
    {"exec.max_output_bytes", "1048576", "1048576"},
};

#define CONFIG_COUNT (sizeof(g_config) / sizeof(g_config[0]))

void taz_config_reset(void)
{
    for (size_t i = 0; i < CONFIG_COUNT; i++)
    {
        (void)strncpy(g_config[i].value, g_config[i].default_value,
                      CONFIG_VAL_MAX - 1U);
        g_config[i].value[CONFIG_VAL_MAX - 1U] = '\0';
    }
}

static config_entry_t *find_entry(const char *key)
{
    for (size_t i = 0; i < CONFIG_COUNT; i++)
    {
        if (strcmp(g_config[i].key, key) == 0)
        {
            return &g_config[i];
        }
    }
    return NULL;
}

static void append_kv(taz_v1_ConfigurationGetResponse *out, const char *key,
                      const char *value)
{
    if ((size_t)out->config_count >=
        sizeof(out->config) / sizeof(out->config[0]))
    {
        return;
    }
    taz_v1_KeyValue *kv = &out->config[out->config_count];
    (void)strncpy(kv->key, key, sizeof(kv->key) - 1U);
    kv->key[sizeof(kv->key) - 1U] = '\0';
    (void)strncpy(kv->value, value, sizeof(kv->value) - 1U);
    kv->value[sizeof(kv->value) - 1U] = '\0';
    out->config_count++;
}

void taz_config_get(const char *const *keys, size_t nkeys,
                    taz_v1_ConfigurationGetResponse *out)
{
    taz_v1_ConfigurationGetResponse zero =
        taz_v1_ConfigurationGetResponse_init_zero;
    *out = zero;

    if (nkeys == 0)
    {
        /* Return all keys */
        for (size_t i = 0; i < CONFIG_COUNT; i++)
        {
            append_kv(out, g_config[i].key, g_config[i].value);
        }
        return;
    }

    for (size_t i = 0; i < nkeys; i++)
    {
        const config_entry_t *e = find_entry(keys[i]);
        if (e != NULL)
        {
            append_kv(out, e->key, e->value);
        }
    }
}

/* Validate a string token against a NULL-terminated list of allowed values.
 * Returns 1 if valid, 0 if not. */
static int validate_enum(const char *value, const char *const *allowed)
{
    for (size_t i = 0; allowed[i] != NULL; i++)
    {
        if (strcmp(value, allowed[i]) == 0)
        {
            return 1;
        }
    }
    return 0;
}

/* Validate exec.max_output_bytes: must be a decimal unsigned int > 0. */
static int validate_uint(const char *value)
{
    if (value == NULL || value[0] == '\0')
    {
        return 0;
    }
    /* Reject leading sign or whitespace */
    if (value[0] == '-' || value[0] == '+' || value[0] == ' ')
    {
        return 0;
    }
    char *end = NULL;
    errno = 0;
    unsigned long ul = strtoul(value, &end, DECIMAL_BASE);
    /* Must consume entire string and not overflow */
    if (end == NULL || *end != '\0' || errno == ERANGE || ul == 0UL)
    {
        return 0;
    }
    /* Must fit in a uint32 */
    if (ul > (unsigned long)UINT32_MAX)
    {
        return 0;
    }
    return 1;
}

static void append_applied(taz_v1_ConfigurationUpdateResponse *out,
                           const char *key)
{
    if ((size_t)out->applied_count >=
        sizeof(out->applied) / sizeof(out->applied[0]))
    {
        return;
    }
    char *dst = out->applied[out->applied_count];
    (void)strncpy(dst, key, sizeof(out->applied[0]) - 1U);
    dst[sizeof(out->applied[0]) - 1U] = '\0';
    out->applied_count++;
}

static void append_rejected(taz_v1_ConfigurationUpdateResponse *out,
                            const char *key, const char *reason)
{
    if ((size_t)out->rejected_count >=
        sizeof(out->rejected) / sizeof(out->rejected[0]))
    {
        return;
    }
    taz_v1_RejectedKey *rk = &out->rejected[out->rejected_count];
    (void)strncpy(rk->key, key, sizeof(rk->key) - 1U);
    rk->key[sizeof(rk->key) - 1U] = '\0';
    (void)strncpy(rk->reason, reason, sizeof(rk->reason) - 1U);
    rk->reason[sizeof(rk->reason) - 1U] = '\0';
    out->rejected_count++;
}

void taz_config_update(const taz_v1_ConfigurationUpdateRequest *req,
                       taz_v1_ConfigurationUpdateResponse *out)
{
    taz_v1_ConfigurationUpdateResponse zero =
        taz_v1_ConfigurationUpdateResponse_init_zero;
    *out = zero;

    for (pb_size_t i = 0; i < req->config_count; i++)
    {
        const taz_v1_KeyValue *kv = &req->config[i];
        config_entry_t *e = find_entry(kv->key);

        if (e == NULL)
        {
            append_rejected(out, kv->key, "unknown key");
            continue;
        }

        /* Validate per-key constraints */
        if (strcmp(e->key, "log.level") == 0)
        {
            if (!validate_enum(kv->value, LOG_LEVEL_VALUES))
            {
                char reason[256];
                (void)snprintf(reason, sizeof(reason),
                               "invalid log.level '%.64s': must be DEBUG, "
                               "INFO, WARN or ERROR",
                               kv->value);
                append_rejected(out, kv->key, reason);
                continue;
            }
        }
        else if (strcmp(e->key, "compression") == 0)
        {
            if (!validate_enum(kv->value, COMPRESSION_VALUES))
            {
                char reason[256];
                (void)snprintf(reason, sizeof(reason),
                               "invalid compression '%.64s': must be NONE",
                               kv->value);
                append_rejected(out, kv->key, reason);
                continue;
            }
        }
        else if (strcmp(e->key, "exec.max_output_bytes") == 0)
        {
            if (!validate_uint(kv->value))
            {
                char reason[256];
                (void)snprintf(reason, sizeof(reason),
                               "invalid exec.max_output_bytes '%.64s': must "
                               "be a positive decimal integer",
                               kv->value);
                append_rejected(out, kv->key, reason);
                continue;
            }
        }

        (void)strncpy(e->value, kv->value, CONFIG_VAL_MAX - 1U);
        e->value[CONFIG_VAL_MAX - 1U] = '\0';
        append_applied(out, e->key);
    }
}
