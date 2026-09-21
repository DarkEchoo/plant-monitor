#include "analysis.h"

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    const plant_snapshot example = {320.0, 1800.0, -9.5, 12500.0};
    plant_status status;

    if (argc != 2 || strcmp(argv[1], "--simulate") != 0) {
        fprintf(stderr, "usage: %s --simulate\n", argv[0]);
        fprintf(stderr, "live I2C sensor capture is not implemented yet\n");
        return 2;
    }

    status = analyze_plant_snapshot(&example);
    printf("SIMULATED moisture_raw=%.1f light_lux=%.1f "
           "moisture_change_per_hour=%.1f light_exposure_lux_hours=%.1f "
           "status=%s\n",
           example.moisture_raw,
           example.light_lux,
           example.moisture_change_per_hour,
           example.light_exposure_lux_hours,
           plant_status_name(status));
    return 0;
}

