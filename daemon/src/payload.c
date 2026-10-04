#include "taz/payload.h"

#include <string.h>

#include <pb_encode.h>

#include "taz/build_info.h"
#include "taz/v1/common.pb.h"
#include "taz/v1/daemon_control.pb.h"
#include "taz/version.h"

#if defined(_WIN32) || defined(_WIN64)
#define TAZ_PLATFORM_STR "windows/amd64"
#elif defined(__linux__)
#define TAZ_PLATFORM_STR "linux/amd64"
#elif defined(__APPLE__)
#define TAZ_PLATFORM_STR "darwin/amd64"
#else
#define TAZ_PLATFORM_STR "unknown"
#endif

size_t taz_payload_capability(uint8_t *buf, size_t bufsize)
{
    taz_v1_CapabilityPayload msg = taz_v1_CapabilityPayload_init_zero;
    msg.protocol_major = (uint32_t)TAZ_PROTOCOL_MAJOR;
    msg.protocol_minor = (uint32_t)TAZ_PROTOCOL_MINOR;
    /* PING is answered at the frame layer (api.md §1.1) but is still an
     * operation this daemon implements (protocol §9). */
    {
        pb_size_t op = 0;
        msg.operations[op++] = (uint32_t)taz_v1_Opcode_OPCODE_PING;
        msg.operations[op++] = (uint32_t)taz_v1_Opcode_OPCODE_VERSION;
        msg.operations[op++] = (uint32_t)taz_v1_Opcode_OPCODE_CONFIGURATION_GET;
        msg.operations[op++] =
            (uint32_t)taz_v1_Opcode_OPCODE_CONFIGURATION_UPDATE;
        msg.operations[op++] = (uint32_t)taz_v1_Opcode_OPCODE_COMMAND_EXEC;
        msg.operations[op++] = (uint32_t)taz_v1_Opcode_OPCODE_FILE_STAT;
        msg.operations[op++] = (uint32_t)taz_v1_Opcode_OPCODE_FILE_CREATE;
        msg.operations[op++] = (uint32_t)taz_v1_Opcode_OPCODE_FILE_DELETE;
        msg.operations[op++] = (uint32_t)taz_v1_Opcode_OPCODE_FILE_CHMOD;
        msg.operations[op++] = (uint32_t)taz_v1_Opcode_OPCODE_DIR_MAKE;
        msg.operations[op++] = (uint32_t)taz_v1_Opcode_OPCODE_DIR_LIST;
        msg.operations[op++] = (uint32_t)taz_v1_Opcode_OPCODE_DIR_REMOVE;
        msg.operations_count = op;
    }
    msg.compression_count = 1U;
    (void)strncpy(msg.compression[0], "NONE", sizeof(msg.compression[0]) - 1U);

    pb_ostream_t stream = pb_ostream_from_buffer(buf, bufsize);
    if (!pb_encode(&stream, taz_v1_CapabilityPayload_fields, &msg))
    {
        return 0U;
    }
    return stream.bytes_written;
}

size_t taz_payload_version_response(uint8_t *buf, size_t bufsize)
{
    taz_v1_VersionResponse msg = taz_v1_VersionResponse_init_zero;
    (void)strncpy(msg.version, taz_build_version(), sizeof(msg.version) - 1U);
    (void)strncpy(msg.build, taz_build_id(), sizeof(msg.build) - 1U);
    (void)strncpy(msg.platform, TAZ_PLATFORM_STR, sizeof(msg.platform) - 1U);

    pb_ostream_t stream = pb_ostream_from_buffer(buf, bufsize);
    if (!pb_encode(&stream, taz_v1_VersionResponse_fields, &msg))
    {
        return 0U;
    }
    return stream.bytes_written;
}
