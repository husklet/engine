#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int prepare_output(const char *release) {
    char path[1024];
    if (snprintf(path, sizeof path, "%s.output", release) >= (int)sizeof path) return -1;
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0 || dup2(fd, STDOUT_FILENO) < 0) return -1;
    if (fd > STDERR_FILENO) close(fd);
    if (snprintf(path, sizeof path, "%s.error", release) >= (int)sizeof path) return -1;
    fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0 || dup2(fd, STDERR_FILENO) < 0) return -1;
    if (fd > STDERR_FILENO) close(fd);
    fd = open("/dev/null", O_RDONLY);
    if (fd < 0 || dup2(fd, STDIN_FILENO) < 0) return -1;
    if (fd > STDERR_FILENO) close(fd);
    return 0;
}

int main(int argc, char **argv) {
    char socket_path[1024];
    if (argc != 2 || prepare_output(argv[1]) != 0 ||
        snprintf(socket_path, sizeof socket_path, "%s.listener", argv[1]) >= (int)sizeof socket_path)
        return 2;
    int listener = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    struct sockaddr_un unix_address;
    memset(&unix_address, 0, sizeof unix_address);
    unix_address.sun_family = AF_UNIX;
    snprintf(unix_address.sun_path, sizeof unix_address.sun_path, "%s", socket_path);
    unlink(socket_path);
    if (listener < 0 || bind(listener, (struct sockaddr *)&unix_address, sizeof unix_address) != 0 ||
        listen(listener, 7) != 0)
        return 3;
    int listener_alias = dup(listener);
    int datagram = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    struct sockaddr_in datagram_address = {.sin_family = AF_INET, .sin_port = 0};
    datagram_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (listener_alias < 0 || datagram < 0 ||
        bind(datagram, (struct sockaddr *)&datagram_address, sizeof datagram_address) != 0)
        return 4;
    socklen_t datagram_size = sizeof datagram_address;
    if (getsockname(datagram, (struct sockaddr *)&datagram_address, &datagram_size) != 0 ||
        datagram_address.sin_port == 0)
        return 4;
    int spare = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    int broadcast = 1;
    int expected_broadcast;
    int receive_buffer = 131072;
    if (spare < 0 || setsockopt(spare, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof broadcast) != 0 ||
        setsockopt(spare, SOL_SOCKET, SO_RCVBUF, &receive_buffer, sizeof receive_buffer) != 0)
        return 5;
    socklen_t expected_size = sizeof expected_broadcast;
    if (getsockopt(spare, SOL_SOCKET, SO_BROADCAST, &expected_broadcast, &expected_size) != 0) return 5;
    dprintf(STDOUT_FILENO, "READY 1 port=%u\n", (unsigned)ntohs(datagram_address.sin_port));
    while (access(argv[1], F_OK) != 0)
        if (errno != ENOENT) return 6;
    if (!(fcntl(listener, F_GETFD) & FD_CLOEXEC) || (fcntl(listener_alias, F_GETFD) & FD_CLOEXEC) ||
        !(fcntl(datagram, F_GETFD) & FD_CLOEXEC))
        return 7;
    struct sockaddr_in restored_address;
    socklen_t restored_size = sizeof restored_address;
    if (getsockname(datagram, (struct sockaddr *)&restored_address, &restored_size) != 0 ||
        restored_address.sin_port != datagram_address.sin_port)
        return 8;
    int client = socket(AF_UNIX, SOCK_STREAM, 0);
    if (client < 0 || connect(client, (struct sockaddr *)&unix_address, sizeof unix_address) != 0) return 9;
    int accepted = -1;
    for (int attempt = 0; attempt < 500 && accepted < 0; ++attempt) {
        accepted = accept(listener_alias, NULL, NULL);
        if (accepted < 0 && (errno == EAGAIN || errno == EINTR)) usleep(10000);
    }
    if (accepted < 0 || send(client, "unix", 4, 0) != 4) return 9;
    char buffer[16];
    if (recv(accepted, buffer, sizeof buffer, 0) != 4 || memcmp(buffer, "unix", 4) != 0) return 9;
    int udp_client = socket(AF_INET, SOCK_DGRAM, 0);
    ssize_t sent = udp_client < 0 ? -1 : sendto(udp_client, "udp", 3, 0, (struct sockaddr *)&restored_address,
                                                sizeof restored_address);
    if (udp_client < 0 || sent != 3) {
        dprintf(STDERR_FILENO, "socket-state udp send fd=%d sent=%lld errno=%d\n", udp_client,
                (long long)sent, errno);
        return 10;
    }
    ssize_t received = -1;
    for (int attempt = 0; attempt < 500 && received < 0; ++attempt) {
        received = recv(datagram, buffer, sizeof buffer, 0);
        if (received < 0 && (errno == EAGAIN || errno == EINTR)) usleep(10000);
    }
    socklen_t option_size = sizeof broadcast;
    int option_result = getsockopt(spare, SOL_SOCKET, SO_BROADCAST, &broadcast, &option_size);
    if (received != 3 || memcmp(buffer, "udp", 3) != 0 || option_result != 0 ||
        broadcast != expected_broadcast) {
        dprintf(STDERR_FILENO, "socket-state udp recv=%lld errno=%d option=%d broadcast=%d\n",
                (long long)received, errno, option_result, broadcast);
        return 10;
    }
    close(accepted);
    close(client);
    close(udp_client);
    close(listener);
    close(listener_alias);
    close(datagram);
    close(spare);
    unlink(socket_path);
    dprintf(STDOUT_FILENO, "SOCKET-STATE-RESTORED\n");
    return 0;
}
