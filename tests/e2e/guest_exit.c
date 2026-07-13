#include <stdint.h>

__attribute__((noreturn)) void _start(void) {
#if defined(__aarch64__)
    register uint64_t status __asm__("x0") = 42;
    register uint64_t syscall_number __asm__("x8") = 93;
    __asm__ volatile("svc #0" : : "r"(status), "r"(syscall_number) : "memory");
#elif defined(__x86_64__)
    register uint64_t status __asm__("rdi") = 42;
    register uint64_t syscall_number __asm__("rax") = 60;
    __asm__ volatile("syscall" : : "r"(status), "r"(syscall_number) : "rcx", "r11", "memory");
#else
#error unsupported guest architecture
#endif
    __builtin_unreachable();
}
