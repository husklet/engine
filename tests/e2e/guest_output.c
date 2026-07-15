#include <stdint.h>

__attribute__((noreturn)) void _start(void) {
    static const char message[] = "activation-stdio\n";
#if defined(__aarch64__)
    register uint64_t fd __asm__("x0") = 1;
    register const char *data __asm__("x1") = message;
    register uint64_t size __asm__("x2") = sizeof(message) - 1;
    register uint64_t number __asm__("x8") = 64;
    __asm__ volatile("svc #0" : : "r"(fd), "r"(data), "r"(size), "r"(number) : "memory");
    fd = 42; number = 93;
    __asm__ volatile("svc #0" : : "r"(fd), "r"(number) : "memory");
#else
#error unsupported guest
#endif
    __builtin_unreachable();
}
