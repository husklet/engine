// timerfd per-fd state the engine must model: gettime before arming reports a disarmed timer,
// settime returns the previous setting, an absolute deadline in the past fires immediately,
// the read counter accumulates missed expirations, and disarming clears a pending count.
#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

int main(void) {
    int fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    struct itimerspec cur;
    memset(&cur, 0xff, sizeof cur);
    int g0 = timerfd_gettime(fd, &cur);
    int disarmed = (cur.it_value.tv_sec == 0 && cur.it_value.tv_nsec == 0 &&
                    cur.it_interval.tv_sec == 0 && cur.it_interval.tv_nsec == 0);

    uint64_t ticks = 0;
    ssize_t r0 = read(fd, &ticks, 8);
    int e0 = (r0 == -1) ? errno : 0;

    struct itimerspec past = {.it_interval = {0, 0}, .it_value = {1, 0}};
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    past.it_value.tv_sec = now.tv_sec - 5;
    past.it_value.tv_nsec = now.tv_nsec;
    struct itimerspec old;
    memset(&old, 0xff, sizeof old);
    int s1 = timerfd_settime(fd, TFD_TIMER_ABSTIME, &past, &old);
    int oldzero = (old.it_value.tv_sec == 0 && old.it_value.tv_nsec == 0);
    struct timespec pause = {0, 20000000};
    nanosleep(&pause, NULL);
    ssize_t r1 = read(fd, &ticks, 8);

    // periodic 10ms, sleep 55ms, expect several accumulated ticks then disarm clears them
    struct itimerspec per = {.it_interval = {0, 10000000}, .it_value = {0, 10000000}};
    struct itimerspec old2;
    memset(&old2, 0, sizeof old2);
    int s2 = timerfd_settime(fd, 0, &per, &old2);
    struct timespec p2 = {0, 55000000};
    nanosleep(&p2, NULL);
    uint64_t many = 0;
    ssize_t r2 = read(fd, &many, 8);
    struct itimerspec off = {{0, 0}, {0, 0}};
    struct itimerspec prev;
    memset(&prev, 0, sizeof prev);
    int s3 = timerfd_settime(fd, 0, &off, &prev);
    int prevint = (prev.it_interval.tv_nsec == 10000000);
    nanosleep(&p2, NULL);
    uint64_t after = 0;
    ssize_t r3 = read(fd, &after, 8);
    int e3 = (r3 == -1) ? errno : 0;
    printf("g0=%d disarmed=%d r0=%zd e0=%d s1=%d oldzero=%d r1=%zd ticks=%llu s2=%d r2=%zd many=%d s3=%d prevint=%d r3=%zd e3=%d\n",
           g0, disarmed, r0, e0, s1, oldzero, r1, (unsigned long long)ticks, s2, r2, many >= 3, s3, prevint, r3, e3);
    return 0;
}
