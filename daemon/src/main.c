/**
 * @file main.c
 * @brief TAZ reference daemon entry point.
 */

#include <stdio.h>
#include <stdlib.h>

#include <pb.h>
#include <uv.h>

#include "taz/v1/common.pb.h"

#ifndef TAZ_VERSION
#define TAZ_VERSION "0.0.0"
#endif

#ifndef TAZ_BUILD_ID
#define TAZ_BUILD_ID "unknown"
#endif

int main(void)
{
    (void)printf("tazd %s (%s) libuv %s %s\n", TAZ_VERSION, TAZ_BUILD_ID,
                 uv_version_string(), NANOPB_VERSION);
    (void)printf("protocol opcode range: 0x0001..0x%04x\n",
                 (unsigned)taz_v1_Opcode_OPCODE_PIPELINE);
    return EXIT_SUCCESS;
}
