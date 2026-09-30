#include "config.h"

#include <stdlib.h>

#include <pb_decode.h>

#include "taz/config.h"
#include "taz/error.h"
#include "taz/response.h"
#include "taz/v1/common.pb.h"
#include "taz/v1/daemon_control.pb.h"

void handle_configuration_get(const taz_frame_header_t *header,
                              const uint8_t *payload,
                              taz_dispatch_write_fn_t write_fn, void *ctx)
{
    taz_v1_ConfigurationGetRequest req =
        taz_v1_ConfigurationGetRequest_init_zero;
    const char *keys[32];
    size_t nkeys = 0U;
    taz_v1_ConfigurationGetResponse *resp;

    if (payload != NULL && header->length > 0U)
    {
        pb_istream_t istream =
            pb_istream_from_buffer(payload, (size_t)header->length);
        if (!pb_decode(&istream, taz_v1_ConfigurationGetRequest_fields, &req))
        {
            taz_error_send(write_fn, ctx, header->stream_id, header->opcode,
                           taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST,
                           "decode ConfigurationGetRequest failed", NULL);
            return;
        }
        if (req.keys_count > 0)
        {
            pb_size_t i;
            for (i = 0; i < req.keys_count; i++)
            {
                keys[i] = req.keys[i];
            }
            nkeys = (size_t)req.keys_count;
        }
    }

    resp = (taz_v1_ConfigurationGetResponse *)malloc(
        sizeof(taz_v1_ConfigurationGetResponse));
    if (resp == NULL)
    {
        taz_error_send(write_fn, ctx, header->stream_id, header->opcode,
                       taz_v1_ErrorCode_ERROR_CODE_INTERNAL, "out of memory",
                       NULL);
        return;
    }

    taz_config_get(nkeys > 0U ? keys : NULL, nkeys, resp);
    taz_response_send(write_fn, ctx, header->stream_id, header->opcode,
                      taz_v1_ConfigurationGetResponse_fields, resp);
    free(resp);
}

void handle_configuration_update(const taz_frame_header_t *header,
                                 const uint8_t *payload,
                                 taz_dispatch_write_fn_t write_fn, void *ctx)
{
    taz_v1_ConfigurationUpdateRequest *req;
    taz_v1_ConfigurationUpdateResponse *resp;

    if (payload == NULL || header->length == 0U)
    {
        taz_error_send(write_fn, ctx, header->stream_id, header->opcode,
                       taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST,
                       "empty CONFIGURATION_UPDATE payload", NULL);
        return;
    }

    /* Both are heap-allocated: the request alone holds 32 key/value pairs
     * (about 20 KiB). */
    req = (taz_v1_ConfigurationUpdateRequest *)malloc(
        sizeof(taz_v1_ConfigurationUpdateRequest));
    resp = (taz_v1_ConfigurationUpdateResponse *)malloc(
        sizeof(taz_v1_ConfigurationUpdateResponse));
    if (req == NULL || resp == NULL)
    {
        free(req);
        free(resp);
        taz_error_send(write_fn, ctx, header->stream_id, header->opcode,
                       taz_v1_ErrorCode_ERROR_CODE_INTERNAL, "out of memory",
                       NULL);
        return;
    }

    {
        pb_istream_t istream =
            pb_istream_from_buffer(payload, (size_t)header->length);
        if (!pb_decode(&istream, taz_v1_ConfigurationUpdateRequest_fields, req))
        {
            free(req);
            free(resp);
            taz_error_send(write_fn, ctx, header->stream_id, header->opcode,
                           taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST,
                           "decode ConfigurationUpdateRequest failed", NULL);
            return;
        }
    }

    taz_config_update(req, resp);
    taz_response_send(write_fn, ctx, header->stream_id, header->opcode,
                      taz_v1_ConfigurationUpdateResponse_fields, resp);
    free(req);
    free(resp);
}
