/**
 * @file main.c
 * @brief TAZER reference daemon entry point.
 *
 * Step 1 scaffold: proves that libuv and the nanopb-generated protocol
 * messages link into a single binary. The TCP server arrives in Step 3.
 */

#include <stdio.h>
#include <stdlib.h>

#include <pb.h>
#include <uv.h>

#include "tazer/v1/common.pb.h"

#ifndef TAZER_VERSION
#define TAZER_VERSION "0.0.0"
#endif

#ifndef TAZER_BUILD_ID
#define TAZER_BUILD_ID "unknown"
#endif

int main(void)
{
    (void)printf("tazer %s (%s) libuv %s %s\n", TAZER_VERSION, TAZER_BUILD_ID,
                 uv_version_string(), NANOPB_VERSION);
    (void)printf("protocol opcode range: 0x0001..0x%04x\n",
                 (unsigned)tazer_v1_Opcode_OPCODE_PIPELINE);
    return EXIT_SUCCESS;
}
