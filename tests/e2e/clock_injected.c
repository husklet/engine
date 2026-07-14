#define _GNU_SOURCE
#include <time.h>

int main(void) {
    struct timespec value;
    return clock_gettime(CLOCK_REALTIME, &value) == 0 && value.tv_sec == 123456789 ? 0 : 1;
}
