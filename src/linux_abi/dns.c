#define _POSIX_C_SOURCE 200809L
#include "dns.h"

#include <netdb.h>
#include <pthread.h>

static pthread_once_t dns_preparation = PTHREAD_ONCE_INIT;

static void prepare_host_resolver(void) {
    struct addrinfo hints = {0};
    struct addrinfo *result = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo("localhost", NULL, &hints, &result) == 0) freeaddrinfo(result);
}

void hl_linux_dns_prepare(void) {
    (void)pthread_once(&dns_preparation, prepare_host_resolver);
}
