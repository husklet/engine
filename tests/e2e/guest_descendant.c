#include <unistd.h>

int main(void) {
    if (fork() < 0) return 2;
    for (;;)
        pause();
}
