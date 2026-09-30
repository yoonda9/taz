#ifndef TAZ_HANDLERS_CONFIG_H
#define TAZ_HANDLERS_CONFIG_H

#include <stdint.h>

#include "taz/dispatch.h"
#include "taz/frame.h"

#ifdef __cplusplus
extern "C"
{
#endif

    void handle_configuration_get(const taz_frame_header_t *header,
                                  const uint8_t *payload,
                                  taz_dispatch_write_fn_t write_fn, void *ctx);

    void handle_configuration_update(const taz_frame_header_t *header,
                                     const uint8_t *payload,
                                     taz_dispatch_write_fn_t write_fn,
                                     void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* TAZ_HANDLERS_CONFIG_H */
