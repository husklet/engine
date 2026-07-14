#ifndef HL_TEST_TRANSFER_H
#define HL_TEST_TRANSFER_H

#include <string.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static void counter_notification(void *observer, uint64_t token) {
    int descriptor = *(int *)observer;
    ssize_t ignored = write(descriptor, &token, sizeof(token));
    (void)ignored;
}

static int check_transfer_fork(const hl_host_services *services) {
    static const char payload[] = "fork-transfer";
    hl_host_result channels = services->transfer->channel_pair(services->context);
    pid_t child;
    int status;
    char data[sizeof(payload)] = {0};
    hl_host_transfer_attachment attachment = {0};
    hl_host_result received;
    HL_CHECK(channels.status == HL_STATUS_OK);
    child = fork();
    HL_CHECK(child >= 0);
    if (child == 0) {
        hl_host_result counter;
        hl_host_transfer_attachment sent;
        (void)services->transfer->close(services->context, channels.detail);
        counter = services->counter->create(services->context, 23, HL_HOST_COUNTER_NONBLOCK);
        if (counter.status != HL_STATUS_OK) _exit(31);
        sent = (hl_host_transfer_attachment){counter.value, HL_HOST_TRANSFER_KIND_COUNTER, HL_HOST_TRANSFER_READ};
        if (services->transfer
                ->send(services->context, channels.value, (hl_host_const_bytes){payload, sizeof(payload)}, &sent, 1)
                .status != HL_STATUS_OK)
            _exit(32);
        if (services->counter->close(services->context, counter.value).status != HL_STATUS_OK) _exit(33);
        (void)services->transfer->close(services->context, channels.value);
        _exit(0);
    }
    HL_CHECK(services->transfer->close(services->context, channels.value).status == HL_STATUS_OK);
    received =
        services->transfer->receive(services->context, channels.detail, (hl_host_bytes){data, 1}, &attachment, 1);
    HL_CHECK(received.status == HL_STATUS_RESOURCE_LIMIT && received.value == sizeof(payload) && received.detail == 1);
    received =
        services->transfer->receive(services->context, channels.detail, (hl_host_bytes){data, sizeof(data)}, NULL, 0);
    HL_CHECK(received.status == HL_STATUS_RESOURCE_LIMIT && received.value == sizeof(payload) && received.detail == 1);
    received = services->transfer->receive(services->context, channels.detail, (hl_host_bytes){data, sizeof(data)},
                                           &attachment, 1);
    HL_CHECK(received.status == HL_STATUS_OK && received.value == sizeof(payload) && received.detail == 1);
    HL_CHECK(memcmp(data, payload, sizeof(payload)) == 0 && attachment.kind == HL_HOST_TRANSFER_KIND_COUNTER &&
             attachment.rights == HL_HOST_TRANSFER_READ);
    HL_CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    HL_CHECK(services->counter->read(services->context, attachment.object).value == 23);
    HL_CHECK(services->counter->write(services->context, attachment.object, 1).status == HL_STATUS_PERMISSION_DENIED);
    HL_CHECK(services->counter->get_flags(services->context, attachment.object).status == HL_STATUS_PERMISSION_DENIED);
    HL_CHECK(services->counter->close(services->context, attachment.object).status == HL_STATUS_OK);
    HL_CHECK(services->transfer->close(services->context, channels.detail).status == HL_STATUS_OK);

    /* A remote post-fork write must wake a subscription without consuming the counter. */
    {
        int notification[2];
        uint64_t token = 0;
        hl_host_result counter = services->counter->create(services->context, 0, HL_HOST_COUNTER_NONBLOCK);
        hl_host_result subscription;
        struct pollfd ready;
        HL_CHECK(counter.status == HL_STATUS_OK && pipe(notification) == 0);
        subscription =
            services->counter->subscribe(services->context, counter.value, counter_notification, &notification[1], 93);
        HL_CHECK(subscription.status == HL_STATUS_OK);
        child = fork();
        HL_CHECK(child >= 0);
        if (child == 0) {
            close(notification[0]);
            close(notification[1]);
            _exit(services->counter->write(services->context, counter.value, 5).status == HL_STATUS_OK ? 0 : 51);
        }
        ready = (struct pollfd){notification[0], POLLIN, 0};
        HL_CHECK(poll(&ready, 1, 2000) == 1 && read(notification[0], &token, sizeof(token)) == sizeof(token) &&
                 token == 93);
        HL_CHECK(services->counter->readiness(services->context, counter.value, HL_HOST_READY_READ).value ==
                 HL_HOST_READY_READ);
        HL_CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
        HL_CHECK(services->counter->unsubscribe(services->context, subscription.value).status == HL_STATUS_OK);
        HL_CHECK(services->counter->read(services->context, counter.value).value == 5);
        HL_CHECK(services->counter->close(services->context, counter.value).status == HL_STATUS_OK);
        close(notification[0]);
        close(notification[1]);
    }

    /* Endpoint aliases share one queue and remain pollable after the original alias closes. */
    channels = services->transfer->channel_pair(services->context);
    HL_CHECK(channels.status == HL_STATUS_OK);
    {
        hl_host_result alias = services->transfer->duplicate(services->context, channels.detail);
        hl_host_result pollset = services->event->create(services->context);
        hl_host_event_record event;
        char byte = 0;
        HL_CHECK(alias.status == HL_STATUS_OK && pollset.status == HL_STATUS_OK);
        HL_CHECK(services->transfer->close(services->context, channels.detail).status == HL_STATUS_OK);
        HL_CHECK(services->event
                     ->control(services->context, pollset.value, HL_HOST_EVENT_ADD, alias.value, 71, HL_HOST_READY_READ)
                     .status == HL_STATUS_OK);
        HL_CHECK(services->transfer->send(services->context, channels.value, (hl_host_const_bytes){"q", 1}, NULL, 0)
                     .status == HL_STATUS_OK);
        HL_CHECK(services->event->wait(services->context, pollset.value, &event, 1, HL_HOST_DEADLINE_INFINITE).value ==
                     1 &&
                 event.token == 71 && (event.readiness & HL_HOST_READY_READ) != 0);
        HL_CHECK(
            services->transfer->receive(services->context, alias.value, (hl_host_bytes){&byte, 1}, NULL, 0).status ==
                HL_STATUS_OK &&
            byte == 'q');
        HL_CHECK(services->event->close(services->context, pollset.value).status == HL_STATUS_OK);
        HL_CHECK(services->transfer->close(services->context, alias.value).status == HL_STATUS_OK);
        HL_CHECK(services->transfer->close(services->context, channels.value).status == HL_STATUS_OK);
    }

    /* Closing a receiver with an unread attached object must release the queued native descriptors. */
    channels = services->transfer->channel_pair(services->context);
    HL_CHECK(channels.status == HL_STATUS_OK);
    child = fork();
    HL_CHECK(child >= 0);
    if (child == 0) {
        hl_host_result counter = services->counter->create(services->context, 1, HL_HOST_COUNTER_NONBLOCK);
        hl_host_transfer_attachment sent = {counter.value, HL_HOST_TRANSFER_KIND_COUNTER, HL_HOST_TRANSFER_READ};
        (void)services->transfer->close(services->context, channels.detail);
        if (counter.status != HL_STATUS_OK) _exit(41);
        if (services->transfer->send(services->context, channels.value, (hl_host_const_bytes){NULL, 0}, &sent, 1)
                .status != HL_STATUS_OK)
            _exit(42);
        if (services->counter->close(services->context, counter.value).status != HL_STATUS_OK) _exit(43);
        (void)services->transfer->close(services->context, channels.value);
        _exit(0);
    }
    HL_CHECK(services->transfer->close(services->context, channels.value).status == HL_STATUS_OK);
    HL_CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    HL_CHECK(services->transfer->close(services->context, channels.detail).status == HL_STATUS_OK);
    return 0;
}

#endif
