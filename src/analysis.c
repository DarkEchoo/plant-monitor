#include "analysis.h"

#include <math.h>
#include <stddef.h>

plant_status analyze_plant_snapshot(const plant_snapshot *snapshot)
{
    if (snapshot == NULL ||
        !isfinite(snapshot->moisture_raw) ||
        !isfinite(snapshot->light_lux) ||
        !isfinite(snapshot->light_exposure_lux_hours) ||
        snapshot->moisture_raw < 0.0 ||
        snapshot->light_lux < 0.0 ||
        snapshot->light_exposure_lux_hours < 0.0) {
        return PLANT_STATUS_INVALID;
    }

    /*
     * Temporary thresholds for testing purposes.
     * Replace them after choosing the plant and calibrating the sensors.
     */
    int needs_water = snapshot->moisture_raw < 350.0;
    int needs_more_light = snapshot->light_exposure_lux_hours < 10000.0;

    if (needs_water && needs_more_light) {
        return PLANT_STATUS_WATER_AND_LIGHT;
    }

    if (needs_water) {
        return PLANT_STATUS_WATER_SOON;
    }

    if (needs_more_light) {
        return PLANT_STATUS_NOT_ENOUGH_LIGHT;
    }

    return PLANT_STATUS_STABLE;
}

const char *plant_status_name(plant_status status)
{
    switch (status) {
    case PLANT_STATUS_STABLE:
        return "STABLE";
    case PLANT_STATUS_WATER_SOON:
        return "WATER_SOON";
    case PLANT_STATUS_NOT_ENOUGH_LIGHT:
        return "NOT_ENOUGH_LIGHT";
    case PLANT_STATUS_WATER_AND_LIGHT:
        return "WATER_AND_LIGHT";
    case PLANT_STATUS_INVALID:
    default:
        return "INVALID";
    }
}
