#include "analysis.h"

#include <stddef.h>

double comfort_score(const sensor_snapshot *snapshot)
{
    double humidity_distance;
    double temperature_distance;
    double score;

    if (snapshot == NULL) {
        return -1.0;
    }

    humidity_distance = snapshot->relative_humidity_pct - 45.0;
    if (humidity_distance < 0.0) {
        humidity_distance = -humidity_distance;
    }

    temperature_distance = snapshot->temperature_c - 22.0;
    if (temperature_distance < 0.0) {
        temperature_distance = -temperature_distance;
    }

    score = 100.0 - (humidity_distance * 1.5) - (temperature_distance * 4.0);

    /* Occupancy affects whether current conditions need immediate attention. */
    if (snapshot->occupied == 0) {
        score += 5.0;
    }

    if (score < 0.0) {
        return 0.0;
    }
    if (score > 100.0) {
        return 100.0;
    }
    return score;
}

