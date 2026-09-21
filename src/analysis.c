#include "analysis.h"

#include <stddef.h>

plant_status analyze_plant_snapshot(const plant_snapshot *snapshot)
{
    if (snapshot == NULL || snapshot->moisture_raw < 0.0 ||
        snapshot->light_lux < 0.0 ||
        snapshot->light_exposure_lux_hours < 0.0) {
        return PLANT_STATUS_INVALID;
    }

    /*
     * These development-only boundaries make the scaffold testable. They are
     * not calibrated plant-care claims and must be replaced using real data.
     * Both moisture trend and accumulated light influence the result.
     */
    if (snapshot->moisture_raw < 350.0 &&
        snapshot->moisture_change_per_hour < -8.0 &&
        snapshot->light_exposure_lux_hours > 10000.0) {
        return PLANT_STATUS_WATER_SOON;
    }

    if (snapshot->moisture_change_per_hour < -2.0) {
        return PLANT_STATUS_DRYING;
    }

    return PLANT_STATUS_STABLE;
}

const char *plant_status_name(plant_status status)
{
    switch (status) {
    case PLANT_STATUS_STABLE:
        return "STABLE";
    case PLANT_STATUS_DRYING:
        return "DRYING";
    case PLANT_STATUS_WATER_SOON:
        return "WATER_SOON";
    case PLANT_STATUS_INVALID:
    default:
        return "INVALID";
    }
}

