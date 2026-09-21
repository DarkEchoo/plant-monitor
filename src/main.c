#include "analysis.h"

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    const sensor_snapshot example = {23.0, 52.0, 1};

    if (argc != 2 || strcmp(argv[1], "--simulate") != 0) {
        fprintf(stderr, "usage: %s --simulate\n", argv[0]);
        fprintf(stderr, "live sensor capture is not implemented yet\n");
        return 2;
    }

    printf("SIMULATED temperature_c=%.1f humidity_pct=%.1f occupied=%d "
           "comfort_score=%.1f\n",
           example.temperature_c,
           example.relative_humidity_pct,
           example.occupied,
           comfort_score(&example));
    return 0;
}

