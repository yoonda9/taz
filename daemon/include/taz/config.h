#ifndef TAZ_CONFIG_H
#define TAZ_CONFIG_H

#include "taz/v1/daemon_control.pb.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /* Reset all config keys to their default values. Call once at startup and
     * in tests between cases to ensure a clean state. */
    void taz_config_reset(void);

    /* Populate *out with the current values for the requested keys.
     * If nkeys == 0, all keys are returned.
     * Unknown keys in the request list are silently ignored (they do not appear
     * in *out and are not reported as rejected; rejection only happens on
     * CONFIGURATION_UPDATE). */
    void taz_config_get(const char *const *keys, size_t nkeys,
                        taz_v1_ConfigurationGetResponse *out);

    /* Apply the key/value pairs in req->config to the config store.
     * Valid keys with acceptable values are written and listed in
     * out->applied. Invalid or unknown keys are listed in out->rejected with a
     * human-readable reason. Both lists may be non-empty from a single call. */
    void taz_config_update(const taz_v1_ConfigurationUpdateRequest *req,
                           taz_v1_ConfigurationUpdateResponse *out);

#ifdef __cplusplus
}
#endif

#endif /* TAZ_CONFIG_H */
