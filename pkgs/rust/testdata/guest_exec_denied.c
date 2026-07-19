#include <stdint.h>

__attribute__((noreturn, used)) void start(uint64_t *stack) {
    const char *path = (const char *)(uintptr_t)stack[2];
    const char *const arguments[] = {path, 0};
#if defined(__aarch64__)
    register uint64_t x0 __asm__("x0") = (uint64_t)path;
    register uint64_t x1 __asm__("x1") = (uint64_t)arguments;
    register uint64_t x2 __asm__("x2") = 0;
    register uint64_t x8 __asm__("x8") = 221;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
    register uint64_t status __asm__("x0") = x0 == (uint64_t)-2 ? 0 : 99;
    register uint64_t exit_call __asm__("x8") = 93;
    __asm__ volatile("svc #0" : : "r"(status), "r"(exit_call) : "memory");
#elif defined(__x86_64__)
    register uint64_t rax __asm__("rax") = 59;
    register uint64_t rdi __asm__("rdi") = (uint64_t)path;
    register uint64_t rsi __asm__("rsi") = (uint64_t)arguments;
    register uint64_t rdx __asm__("rdx") = 0;
    __asm__ volatile("syscall" : "+r"(rax) : "r"(rdi), "r"(rsi), "r"(rdx) : "rcx", "r11", "memory");
    rdi = rax == (uint64_t)-2 ? 0 : 99;
    rax = 60;
    __asm__ volatile("syscall" : : "r"(rax), "r"(rdi) : "rcx", "r11", "memory");
#else
#error unsupported guest architecture
#endif
    __builtin_unreachable();
}

#if defined(__aarch64__)
__asm__(".global _start\n"
        "_start:\n"
        "mov x0, sp\n"
        "b start");
#elif defined(__x86_64__)
__asm__(".global _start\n"
        "_start:\n"
        "mov %rsp, %rdi\n"
        "jmp start");
#endif
