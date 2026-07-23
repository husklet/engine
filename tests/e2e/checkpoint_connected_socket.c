#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/tcp.h>
#include <unistd.h>

static int output_open(const char *release) {
    char path[1024];
    snprintf(path, sizeof path, "%s.output", release);
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0 || dup2(fd, STDOUT_FILENO) < 0) return -1;
    if (fd > STDERR_FILENO) close(fd);
    snprintf(path, sizeof path, "%s.error", release);
    fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0 || dup2(fd, STDERR_FILENO) < 0) return -1;
    if (fd > STDERR_FILENO) close(fd);
    fd = open("/dev/null", O_RDONLY);
    if (fd < 0 || dup2(fd, STDIN_FILENO) < 0) return -1;
    if (fd > STDERR_FILENO) close(fd);
    return 0;
}

static int receive_exact(int fd, const char *expected) {
    char data[64];
    size_t wanted = strlen(expected), offset = 0;
    while (offset < wanted) {
        ssize_t count = recv(fd, data + offset, wanted - offset, 0);
        if (count > 0) offset += (size_t)count;
        else if (count < 0 && errno == EINTR) continue;
        else return -1;
    }
    return memcmp(data, expected, wanted) == 0 ? 0 : -1;
}

int main(int argc, char **argv) {
    if (argc != 2 || output_open(argv[1]) != 0) return 2;
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = 0};
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (listener < 0 || bind(listener, (struct sockaddr *)&address, sizeof address) != 0 ||
        listen(listener, 8) != 0)
        return 3;
    socklen_t size = sizeof address;
    if (getsockname(listener, (struct sockaddr *)&address, &size) != 0) return 4;
    pid_t child = fork();
    if (child < 0) return 5;
    if (child == 0) {
        close(listener);
        int client = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (client < 0 || connect(client, (struct sockaddr *)&address, sizeof address) != 0 ||
            send(client, "queued-server", 13, 0) != 13)
            return 6;
        dprintf(STDOUT_FILENO, "READY 2\n");
        while (access(argv[1], F_OK) != 0)
            if (errno != ENOENT) return 7;
        if (!(fcntl(client, F_GETFD) & FD_CLOEXEC) || receive_exact(client, "queued-client") != 0) return 8;
        close(client);
        return 0;
    }
    int server = accept(listener, NULL, NULL);
    int receive_buffer = 196608;
    int one = 1, ttl = 37;
    if (server < 0 || setsockopt(server, SOL_SOCKET, SO_RCVBUF, &receive_buffer, sizeof receive_buffer) != 0 ||
        setsockopt(server, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one) != 0 ||
        setsockopt(server, IPPROTO_IP, IP_TTL, &ttl, sizeof ttl) != 0 ||
        send(server, "queued-client", 13, 0) != 13)
        return 9;
    socklen_t option_size = sizeof receive_buffer;
    if (getsockopt(server, SOL_SOCKET, SO_RCVBUF, &receive_buffer, &option_size) != 0) return 9;
    int expected_receive_buffer = receive_buffer;
    int alias = dup(server);
    if (alias < 0) return 10;
    dprintf(STDOUT_FILENO, "READY 1\n");
    while (access(argv[1], F_OK) != 0)
        if (errno != ENOENT) return 11;
    close(server);
    option_size = sizeof receive_buffer;
    int restored_nodelay = 0, restored_ttl = 0;
    socklen_t int_size = sizeof restored_nodelay;
    int data_result = receive_exact(alias, "queued-server");
    int buffer_result = getsockopt(alias, SOL_SOCKET, SO_RCVBUF, &receive_buffer, &option_size);
    int nodelay_result = getsockopt(alias, IPPROTO_TCP, TCP_NODELAY, &restored_nodelay, &int_size);
    int_size = sizeof restored_ttl;
    int ttl_result = getsockopt(alias, IPPROTO_IP, IP_TTL, &restored_ttl, &int_size);
    if (data_result != 0 || buffer_result != 0 || receive_buffer != expected_receive_buffer ||
        nodelay_result != 0 || restored_nodelay != one || ttl_result != 0 || restored_ttl != ttl) {
        dprintf(STDERR_FILENO, "connected restore data=%d buffer=%d/%d expected=%d nodelay=%d/%d ttl=%d/%d\n",
                data_result, buffer_result, receive_buffer, expected_receive_buffer, nodelay_result,
                restored_nodelay, ttl_result, restored_ttl);
        return 12;
    }
    int status;
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0) return 13;
    close(alias);
    close(listener);
    dprintf(STDOUT_FILENO, "CONNECTED-SOCKET-RESTORED\n");
    return 0;
}
