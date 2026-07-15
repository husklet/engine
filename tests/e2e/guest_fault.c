#include <stdint.h>

__attribute__((noreturn)) void _start(void) {
#if defined(__aarch64__)
    register uintptr_t address __asm__("x0") = UINTPTR_MAX;
    register uint32_t value __asm__("w1") = UINT32_C(1);
    __asm__ volatile("str %w1, [%0]" : : "r"(address), "r"(value) : "memory");
#elif defined(__x86_64__)
    register uintptr_t address __asm__("rax") = UINTPTR_MAX;
    __asm__ volatile("movl $1, (%0)" : : "r"(address) : "memory");
#else
#error unsupported guest architecture
#endif
    __builtin_unreachable();
}
