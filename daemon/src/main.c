/**
 * @file main.c
 * @brief TAZ reference daemon entry point.
 */

#include <stdio.h>
#include <stdlib.h>

#include <pb.h>
#include <uv.h>

#include "taz/build_info.h"
#include "taz/v1/common.pb.h"

int main(void)
{
    (void)printf("tazd %s (%s) libuv %s %s\n", taz_build_version(),
                 taz_build_id(), uv_version_string(), NANOPB_VERSION);
    (void)printf("protocol opcode range: 0x0001..0x%04x\n",
                 (unsigned)taz_v1_Opcode_OPCODE_PIPELINE);
    return EXIT_SUCCESS;
}
