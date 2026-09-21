#ifndef ANALYSIS_H
#define ANALYSIS_H

typedef struct {
    double temperature_c;
    double relative_humidity_pct;
    int occupied;
} sensor_snapshot;

double comfort_score(const sensor_snapshot *snapshot);

#endif

