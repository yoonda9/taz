/* libFuzzer harness for nanopb decoding of every request message type.
 * Feeds arbitrary bytes to pb_decode and verifies no crash or sanitizer
 * error occurs. */

#include <stddef.h>
#include <stdint.h>

#include <pb_decode.h>

#include "taz/v1/common.pb.h"
#include "taz/v1/daemon_control.pb.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    pb_istream_t stream;

    /* Try each request message type.  Decode errors are expected and ignored;
     * only crashes or sanitizer reports indicate a real defect. */
    stream = pb_istream_from_buffer(data, size);
    {
        taz_v1_VersionRequest msg = taz_v1_VersionRequest_init_zero;
        (void)pb_decode(&stream, taz_v1_VersionRequest_fields, &msg);
    }

    stream = pb_istream_from_buffer(data, size);
    {
        taz_v1_CapabilityPayload msg = taz_v1_CapabilityPayload_init_zero;
        (void)pb_decode(&stream, taz_v1_CapabilityPayload_fields, &msg);
    }

    stream = pb_istream_from_buffer(data, size);
    {
        taz_v1_ErrorInfo msg = taz_v1_ErrorInfo_init_zero;
        (void)pb_decode(&stream, taz_v1_ErrorInfo_fields, &msg);
    }

    stream = pb_istream_from_buffer(data, size);
    {
        taz_v1_ConfigurationGetRequest msg =
            taz_v1_ConfigurationGetRequest_init_zero;
        (void)pb_decode(&stream, taz_v1_ConfigurationGetRequest_fields, &msg);
    }

    stream = pb_istream_from_buffer(data, size);
    {
        taz_v1_ConfigurationUpdateRequest msg =
            taz_v1_ConfigurationUpdateRequest_init_zero;
        (void)pb_decode(&stream, taz_v1_ConfigurationUpdateRequest_fields,
                        &msg);
    }

    return 0;
}
