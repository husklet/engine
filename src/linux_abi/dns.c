#define _POSIX_C_SOURCE 200809L
#include "dns.h"

#include <netdb.h>
#include <pthread.h>
#if defined(__APPLE__)
#include <dlfcn.h>
#include <string.h>
#endif

static pthread_once_t dns_preparation = PTHREAD_ONCE_INIT;

#if defined(__APPLE__)
static void prepare_foundation_strings(void) {
    typedef void *(*class_lookup)(const char *);
    typedef void *(*selector_lookup)(const char *);
    typedef void *(*string_create)(void *, void *, const char *);

    void *foundation = dlopen("/System/Library/Frameworks/Foundation.framework/Foundation", RTLD_NOW | RTLD_LOCAL);
    void *runtime = dlopen("/usr/lib/libobjc.A.dylib", RTLD_NOW | RTLD_LOCAL);
    if (foundation == NULL || runtime == NULL) return;

    void *class_symbol = dlsym(runtime, "objc_getClass");
    void *selector_symbol = dlsym(runtime, "sel_registerName");
    void *message_symbol = dlsym(runtime, "objc_msgSend");
    class_lookup get_class = NULL;
    selector_lookup get_selector = NULL;
    string_create create_string = NULL;
    memcpy(&get_class, &class_symbol, sizeof(get_class));
    memcpy(&get_selector, &selector_symbol, sizeof(get_selector));
    memcpy(&create_string, &message_symbol, sizeof(create_string));
    if (get_class == NULL || get_selector == NULL || create_string == NULL) return;

    void *string_class = get_class("NSString");
    void *constructor = get_selector("stringWithUTF8String:");
    if (string_class != NULL && constructor != NULL) (void)create_string(string_class, constructor, "localhost");
}
#endif

static void prepare_host_resolver(void) {
    struct addrinfo hints = {0};
    struct addrinfo *result = NULL;
#if defined(__APPLE__)
    prepare_foundation_strings();
#endif
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo("localhost", NULL, &hints, &result) == 0) freeaddrinfo(result);
}

void hl_linux_dns_prepare(void) {
    (void)pthread_once(&dns_preparation, prepare_host_resolver);
}
