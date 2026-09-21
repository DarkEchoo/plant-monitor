#include "analysis.h"

#include <assert.h>
#include <stdio.h>

int main(void)
{
    const plant_snapshot stable = {500.0, 200.0, -1.0, 1200.0};
    const plant_snapshot drying = {400.0, 900.0, -4.0, 7000.0};
    const plant_snapshot water_soon = {320.0, 1800.0, -9.5, 12500.0};
    const plant_snapshot invalid = {-1.0, 200.0, 0.0, 100.0};

    assert(analyze_plant_snapshot(NULL) == PLANT_STATUS_INVALID);
    assert(analyze_plant_snapshot(&invalid) == PLANT_STATUS_INVALID);
    assert(analyze_plant_snapshot(&stable) == PLANT_STATUS_STABLE);
    assert(analyze_plant_snapshot(&drying) == PLANT_STATUS_DRYING);
    assert(analyze_plant_snapshot(&water_soon) == PLANT_STATUS_WATER_SOON);
    assert(plant_status_name(PLANT_STATUS_WATER_SOON) != NULL);

    puts("analysis tests passed");
    return 0;
}

