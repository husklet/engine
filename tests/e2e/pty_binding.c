#include <stdint.h>
#include <sys/ioctl.h>
#include <unistd.h>

enum {
    LINUX_TCGETS2 = 0x802c542a,
    LINUX_TCSETS2 = 0x402c542b,
};

struct linux_termios2 {
    uint32_t input;
    uint32_t output;
    uint32_t control;
    uint32_t local;
    uint8_t line;
    uint8_t characters[19];
    uint32_t input_speed;
    uint32_t output_speed;
};

int main(void) {
    struct linux_termios2 termios;
    struct winsize size = {.ws_row = 40, .ws_col = 100};
    struct winsize observed = {0};
    if (ioctl(STDIN_FILENO, LINUX_TCGETS2, &termios) != 0) return 10;
    termios.local &= ~UINT32_C(8);
    if (ioctl(STDIN_FILENO, LINUX_TCSETS2, &termios) != 0) return 11;
    termios.local = UINT32_MAX;
    if (ioctl(STDIN_FILENO, LINUX_TCGETS2, &termios) != 0 || (termios.local & UINT32_C(8)) != 0) return 12;
    if (ioctl(STDIN_FILENO, TIOCSWINSZ, &size) != 0) return 13;
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &observed) != 0) return 14;
    return observed.ws_row == 40 && observed.ws_col == 100 ? 0 : 15;
}
