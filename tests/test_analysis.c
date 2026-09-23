#include "analysis.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    const plant_snapshot stable =
        {500.0, 1800.0, -1.0, 12500.0};

    const plant_snapshot not_enough_light =
        {500.0, 200.0, -1.0, 7000.0};

    const plant_snapshot water_soon =
        {320.0, 1800.0, -9.5, 12500.0};

    const plant_snapshot water_and_light =
        {320.0, 200.0, -9.5, 7000.0};

    const plant_snapshot invalid =
        {-1.0, 200.0, 0.0, 100.0};

    assert(analyze_plant_snapshot(NULL) ==
           PLANT_STATUS_INVALID);

    assert(analyze_plant_snapshot(&invalid) ==
           PLANT_STATUS_INVALID);

    assert(analyze_plant_snapshot(&stable) ==
           PLANT_STATUS_STABLE);

    assert(analyze_plant_snapshot(&not_enough_light) ==
           PLANT_STATUS_NOT_ENOUGH_LIGHT);

    assert(analyze_plant_snapshot(&water_soon) ==
           PLANT_STATUS_WATER_SOON);

    assert(analyze_plant_snapshot(&water_and_light) ==
           PLANT_STATUS_WATER_AND_LIGHT);

    assert(strcmp(
        plant_status_name(PLANT_STATUS_STABLE),
        "STABLE") == 0);

    assert(strcmp(
        plant_status_name(PLANT_STATUS_NOT_ENOUGH_LIGHT),
        "NOT_ENOUGH_LIGHT") == 0);

    assert(strcmp(
        plant_status_name(PLANT_STATUS_WATER_SOON),
        "WATER_SOON") == 0);

    assert(strcmp(
        plant_status_name(PLANT_STATUS_WATER_AND_LIGHT),
        "WATER_AND_LIGHT") == 0);

    assert(strcmp(
        plant_status_name(PLANT_STATUS_INVALID),
        "INVALID") == 0);

    puts("analysis tests passed");
    return 0;
}
