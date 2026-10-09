#ifndef DMM_EXAMPLE_SENSOR_H
#define DMM_EXAMPLE_SENSOR_H
#include <stddef.h>

typedef struct {
    double minimum, maximum, mean;
    size_t count;
} SensorStats;

/* Implemented in DMM and called by C using the native ABI. */
double sensor_calibrate(double celsius);

/* Pointers are borrowed for this call; C neither retains nor frees them. */
int sensor_analyze(const double *samples, size_t count, SensorStats *output);
#endif
