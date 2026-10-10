#include "taz/process.h"

/* Percentage scale (100%) for the interval CPU-percentage calculation. */
#define TAZ_PROCESS_INTERVAL_CPU_PERCENT_SCALE 100.0

float taz_process_interval_cpu_percent(uint64_t cpu_delta_ns,
                                       uint64_t wall_delta_ns)
{
    double ratio;

    if (wall_delta_ns == 0U)
    {
        return 0.0F;
    }
    ratio = TAZ_PROCESS_INTERVAL_CPU_PERCENT_SCALE * (double)cpu_delta_ns /
            (double)wall_delta_ns;
    return (float)ratio;
}
