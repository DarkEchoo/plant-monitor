#ifndef ANALYSIS_H
#define ANALYSIS_H

typedef struct {
    double moisture_raw;
    double light_lux;
    double moisture_change_per_hour;
    double light_exposure_lux_hours;
} plant_snapshot;

typedef enum {
    PLANT_STATUS_INVALID = -1,
    PLANT_STATUS_STABLE = 0,
    PLANT_STATUS_DRYING,
    PLANT_STATUS_WATER_SOON
} plant_status;

plant_status analyze_plant_snapshot(const plant_snapshot *snapshot);
const char *plant_status_name(plant_status status);

#endif

