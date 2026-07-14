#ifndef HL_TEST_COUNTER_H
#define HL_TEST_COUNTER_H

static int check_counter(hl_host_services *services) {
    hl_host_result counter;
    hl_host_result duplicate;
    hl_host_result pollset;
    hl_host_event_record event;
    HL_CHECK((services->capabilities & (HL_HOST_CAP_COUNTER | HL_HOST_CAP_EVENT)) ==
             (HL_HOST_CAP_COUNTER | HL_HOST_CAP_EVENT));
    counter = services->counter->create(services->context, 0, HL_HOST_COUNTER_NONBLOCK);
    HL_CHECK(counter.status == HL_STATUS_OK);
    HL_CHECK(services->counter->read(services->context, counter.value).status == HL_STATUS_WOULD_BLOCK);
    duplicate = services->counter->duplicate(services->context, counter.value);
    HL_CHECK(duplicate.status == HL_STATUS_OK);
    pollset = services->event->create(services->context);
    HL_CHECK(pollset.status == HL_STATUS_OK);
    HL_CHECK(services->event
                 ->control(services->context, pollset.value, HL_HOST_EVENT_ADD, duplicate.value, 77, HL_HOST_READY_READ)
                 .status == HL_STATUS_OK);
    {
        hl_host_result empty = services->event->wait(services->context, pollset.value, &event, 1, 0);
        HL_CHECK((empty.status == HL_STATUS_OK && empty.value == 0) || empty.status == HL_STATUS_WOULD_BLOCK);
    }
    HL_CHECK(services->counter->write(services->context, counter.value, 4).status == HL_STATUS_OK);
    HL_CHECK(services->event->wait(services->context, pollset.value, &event, 1, HL_HOST_DEADLINE_INFINITE).status ==
             HL_STATUS_OK);
    HL_CHECK(event.token == 77 && (event.readiness & HL_HOST_READY_READ) != 0);
    HL_CHECK(services->counter->read(services->context, duplicate.value).value == 4);
    HL_CHECK(services->counter->write(services->context, counter.value, UINT64_MAX).status ==
             HL_STATUS_INVALID_ARGUMENT);
    HL_CHECK(services->counter->close(services->context, counter.value).status == HL_STATUS_OK);
    HL_CHECK(services->counter->write(services->context, duplicate.value, 2).status == HL_STATUS_OK);
    HL_CHECK(services->counter->read(services->context, duplicate.value).value == 2);
    HL_CHECK(services->event->close(services->context, pollset.value).status == HL_STATUS_OK);
    HL_CHECK(services->counter->close(services->context, duplicate.value).status == HL_STATUS_OK);

    counter = services->counter->create(services->context, 2, HL_HOST_COUNTER_SEMAPHORE | HL_HOST_COUNTER_NONBLOCK);
    HL_CHECK(counter.status == HL_STATUS_OK);
    HL_CHECK(services->counter->read(services->context, counter.value).value == 1);
    HL_CHECK(services->counter->read(services->context, counter.value).value == 1);
    HL_CHECK(services->counter->read(services->context, counter.value).status == HL_STATUS_WOULD_BLOCK);
    HL_CHECK(services->counter->close(services->context, counter.value).status == HL_STATUS_OK);
    return 0;
}

#endif
