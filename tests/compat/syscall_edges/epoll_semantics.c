// epoll bookkeeping the engine must model itself: duplicate ADD is EEXIST, MOD/DEL of an
// unregistered fd is ENOENT, adding an epoll fd to itself is EINVAL, EPOLLONESHOT disarms
// after one delivery and rearms only via MOD, and edge-triggered reports only on transition.
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

int main(void) {
    int ep = epoll_create1(0);
    int ev = eventfd(0, EFD_NONBLOCK);
    int unreg = eventfd(0, EFD_NONBLOCK); // never registered: MOD/DEL of it must be ENOENT
    struct epoll_event e = {.events = EPOLLIN, .data.u32 = 7};
    int a1 = epoll_ctl(ep, EPOLL_CTL_ADD, ev, &e);
    int a2 = epoll_ctl(ep, EPOLL_CTL_ADD, ev, &e);
    int ea2 = (a2 == -1) ? errno : 0;
    int m = epoll_ctl(ep, EPOLL_CTL_MOD, unreg, &e);
    int em = (m == -1) ? errno : 0;
    int d = epoll_ctl(ep, EPOLL_CTL_DEL, unreg, &e);
    int ed = (d == -1) ? errno : 0;
    int self = epoll_ctl(ep, EPOLL_CTL_ADD, ep, &e);
    int eself = (self == -1) ? errno : 0;

    struct epoll_event out[4];
    uint64_t one = 1;
    (void)!write(ev, &one, 8);
    int n1 = epoll_wait(ep, out, 4, 0);
    int n2 = epoll_wait(ep, out, 4, 0); // still level-triggered readable
    // switch to oneshot
    e.events = EPOLLIN | EPOLLONESHOT;
    epoll_ctl(ep, EPOLL_CTL_MOD, ev, &e);
    int n3 = epoll_wait(ep, out, 4, 0);
    int n4 = epoll_wait(ep, out, 4, 0); // disarmed
    epoll_ctl(ep, EPOLL_CTL_MOD, ev, &e);
    int n5 = epoll_wait(ep, out, 4, 0); // rearmed
    // edge triggered
    e.events = EPOLLIN | EPOLLET;
    epoll_ctl(ep, EPOLL_CTL_MOD, ev, &e);
    int n6 = epoll_wait(ep, out, 4, 0); // MOD rearms ET, reports once
    int n7 = epoll_wait(ep, out, 4, 0); // no new transition
    (void)!write(ev, &one, 8);
    int n8 = epoll_wait(ep, out, 4, 0); // new write is a transition
    printf("a1=%d a2=%d ea2=%d m=%d em=%d d=%d ed=%d self=%d eself=%d n=%d,%d,%d,%d,%d,%d,%d,%d data=%u\n",
           a1, a2, ea2, m, em, d, ed, self, eself, n1, n2, n3, n4, n5, n6, n7, n8, out[0].data.u32);
    return 0;
}
