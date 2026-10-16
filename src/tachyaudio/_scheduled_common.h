#ifndef TACHY_SCHEDULED_COMMON_H
#define TACHY_SCHEDULED_COMMON_H
#include <stdint.h>
#include <math.h>

enum {
    TACHY_SCHEDULE_IDLE,
    TACHY_SCHEDULE_PREPARING,
    TACHY_SCHEDULE_PENDING,
    TACHY_SCHEDULE_PLAYING,
    TACHY_SCHEDULE_RENDERED,
    TACHY_SCHEDULE_CANCELLED,
    TACHY_SCHEDULE_MISSED,
    TACHY_SCHEDULE_CLOCK_ERROR,
    TACHY_SCHEDULE_DISCONTINUITY,
    TACHY_SCHEDULE_CALLBACK_LATE,
    TACHY_SCHEDULE_FORMAT_ERROR
};

static long long tachy_schedule_offset(uint64_t target, uint64_t beginning, double rate)
{
    if (target < beginning) {
        return (double)(beginning - target) < 1000000000.0 / rate ? 0 : -1;
    }
    double offset = ceil(((double)(target - beginning) / 1000000000.0) * rate);
    if (offset > (double)INT32_MAX) {
        return INT32_MAX;
    }
    return (long long)offset;
}


#endif
