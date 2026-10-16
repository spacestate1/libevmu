/* C library pieces libGimbal expects that ESP-IDF's libc does not provide. */
#include <time.h>

int timespec_get(struct timespec *ts, int base)
{
    if (base != TIME_UTC || clock_gettime(CLOCK_REALTIME, ts) != 0) return 0;
    return base;
}
