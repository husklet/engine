// IPV6_V6ONLY get-after-set: read the default, disable it, enable it, and confirm
// each getsockopt reflects the last setsockopt. Pure option-plumbing check on an
// AF_INET6 socket with no traffic. Deterministic booleans -> oracle.
#include "net_util.h"
#include <netinet/in.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

static int v6only(int fd) {
    int v = -1;
    socklen_t l = sizeof v;
    getsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v, &l);
    return v;
}

int main(void) {
    net_watchdog(20);
    int s = socket(AF_INET6, SOCK_STREAM, 0);
    if (s < 0) { printf("no_ipv6\n"); return 0; }
    int def = v6only(s);
    int off = 0, on = 1;
    setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof off);
    int after_off = v6only(s);
    setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, &on, sizeof on);
    int after_on = v6only(s);
    printf("default_bool=%d after_off=%d after_on=%d\n", def != 0, after_off, after_on != 0);
    close(s);
    return 0;
}
