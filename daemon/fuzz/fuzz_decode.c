/* libFuzzer harness for nanopb decoding of every request message type.
 * Feeds arbitrary bytes to pb_decode and verifies no crash or sanitizer
 * error occurs. */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include <pb_decode.h>

#include "taz/v1/advanced.pb.h"
#include "taz/v1/command.pb.h"
#include "taz/v1/daemon_control.pb.h"
#include "taz/v1/file.pb.h"
#include "taz/v1/process.pb.h"

/* Every REQUEST payload the daemon may decode. The first input byte selects
 * the message type; the rest is the encoded message. Seeds in
 * corpus/fuzz_decode/ follow this table's order. */
typedef struct
{
    const pb_msgdesc_t *fields;
    size_t size;
} request_t;

static const request_t REQUESTS[] = {
    {taz_v1_VersionRequest_fields, sizeof(taz_v1_VersionRequest)},
    {taz_v1_ConfigurationGetRequest_fields,
     sizeof(taz_v1_ConfigurationGetRequest)},
    {taz_v1_ConfigurationUpdateRequest_fields,
     sizeof(taz_v1_ConfigurationUpdateRequest)},
    {taz_v1_RestartRequest_fields, sizeof(taz_v1_RestartRequest)},
    {taz_v1_CommandExecRequest_fields, sizeof(taz_v1_CommandExecRequest)},
    {taz_v1_ProcessListRequest_fields, sizeof(taz_v1_ProcessListRequest)},
    {taz_v1_ProcessKillRequest_fields, sizeof(taz_v1_ProcessKillRequest)},
    {taz_v1_ProcessInfoRequest_fields, sizeof(taz_v1_ProcessInfoRequest)},
    {taz_v1_ProcessMonitorRequest_fields, sizeof(taz_v1_ProcessMonitorRequest)},
    {taz_v1_FilePutRequest_fields, sizeof(taz_v1_FilePutRequest)},
    {taz_v1_FileGetRequest_fields, sizeof(taz_v1_FileGetRequest)},
    {taz_v1_FileCreateRequest_fields, sizeof(taz_v1_FileCreateRequest)},
    {taz_v1_FileDeleteRequest_fields, sizeof(taz_v1_FileDeleteRequest)},
    {taz_v1_FileStatRequest_fields, sizeof(taz_v1_FileStatRequest)},
    {taz_v1_FileChmodRequest_fields, sizeof(taz_v1_FileChmodRequest)},
    {taz_v1_DirMakeRequest_fields, sizeof(taz_v1_DirMakeRequest)},
    {taz_v1_DirListRequest_fields, sizeof(taz_v1_DirListRequest)},
    {taz_v1_DirRemoveRequest_fields, sizeof(taz_v1_DirRemoveRequest)},
    {taz_v1_RunAsRequest_fields, sizeof(taz_v1_RunAsRequest)},
    {taz_v1_TimeoutSetRequest_fields, sizeof(taz_v1_TimeoutSetRequest)},
    {taz_v1_LogRequest_fields, sizeof(taz_v1_LogRequest)},
    {taz_v1_DetachRequest_fields, sizeof(taz_v1_DetachRequest)},
    {taz_v1_TaskStatusRequest_fields, sizeof(taz_v1_TaskStatusRequest)},
    {taz_v1_TaskCancelRequest_fields, sizeof(taz_v1_TaskCancelRequest)},
    {taz_v1_CancelRequest_fields, sizeof(taz_v1_CancelRequest)},
};

#define REQUEST_COUNT (sizeof(REQUESTS) / sizeof(REQUESTS[0]))

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    const request_t *request;
    pb_istream_t stream;
    void *msg;

    if (size < 1U)
    {
        return 0;
    }
    request = &REQUESTS[data[0] % REQUEST_COUNT];

    /* Heap, zeroed like *_init_zero: some request structs are tens of KiB,
     * and heap allocation lets ASan see an overrun past the struct's end.
     * Decode errors are expected; only crashes and sanitizer reports count. */
    msg = calloc(1U, request->size);
    if (msg == NULL)
    {
        return 0;
    }
    stream = pb_istream_from_buffer(&data[1], size - 1U);
    (void)pb_decode(&stream, request->fields, msg);
    free(msg);
    return 0;
}
