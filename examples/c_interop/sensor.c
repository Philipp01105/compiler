#include "sensor.h"

int sensor_analyze(const double *samples, size_t count, SensorStats *output) {
    if (!samples || !output || count == 0) return -1;
    double first = sensor_calibrate(samples[0]);
    SensorStats result = {first, first, 0.0, count};
    for (size_t i = 0; i < count; ++i) {
        double value = sensor_calibrate(samples[i]);
        if (value < result.minimum) result.minimum = value;
        if (value > result.maximum) result.maximum = value;
        result.mean += value;
    }
    result.mean /= (double) count;
    *output = result;
    return 0;
}
