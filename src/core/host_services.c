#include "hl/host_services.h"

#include <stddef.h>

static int hl_has_field(uint32_t size, size_t offset, size_t field_size) {
    return size >= offset && (size_t)size - offset >= field_size;
}

static int hl_valid_group(const void *group, uint32_t abi, size_t size) {
    const uint32_t *header = group;
    return group != NULL && header[0] == abi && header[1] >= size;
}

static int hl_valid_file_group(const hl_host_file_services *file) {
    const size_t abi13_size = offsetof(hl_host_file_services, allocate_range);
    return file != NULL &&
           ((file->abi == HL_HOST_FILE_ABI_13 && file->size >= abi13_size) ||
            ((file->abi == HL_HOST_FILE_ABI_14 || file->abi == HL_HOST_FILE_ABI_15 ||
              file->abi == HL_HOST_FILE_ABI_16) &&
             file->size >= offsetof(hl_host_file_services, set_permissions)) ||
            (file->abi == HL_HOST_FILE_ABI_17 &&
             file->size >= offsetof(hl_host_file_services, read_directory)) ||
            (file->abi == HL_HOST_FILE_ABI_18 &&
             file->size >= offsetof(hl_host_file_services, make_directory)) ||
            (file->abi == HL_HOST_FILE_ABI_19 &&
             file->size >= offsetof(hl_host_file_services, make_fifo)) ||
            (file->abi == HL_HOST_FILE_ABI_20 &&
             file->size >= offsetof(hl_host_file_services, validate_private_regular)) ||
            (file->abi == HL_HOST_FILE_ABI_21 &&
             file->size >= offsetof(hl_host_file_services, validate_private_directory)) ||
            (file->abi == HL_HOST_FILE_ABI && file->size >= sizeof(*file)));
}

hl_status hl_host_services_validate(const hl_host_services *services, uint64_t required_capabilities) {
    if (services == NULL) return HL_STATUS_INVALID_ARGUMENT;
    if (services->abi != HL_HOST_SERVICES_ABI) return HL_STATUS_ABI_MISMATCH;
    if (!hl_has_field(services->size, offsetof(hl_host_services, log), sizeof(services->log)))
        return HL_STATUS_ABI_MISMATCH;
    if ((services->capabilities & required_capabilities) != required_capabilities) return HL_STATUS_NOT_SUPPORTED;
    if ((services->capabilities & HL_HOST_CAP_MEMORY) != 0) {
        const hl_host_memory_services *memory = services->memory;
        if (memory == NULL || memory->abi != HL_HOST_MEMORY_ABI || memory->size < sizeof(*memory) ||
            memory->reserve == NULL || memory->protect == NULL || memory->release == NULL ||
            memory->publish_code == NULL)
            return HL_STATUS_ABI_MISMATCH;
    }
    if ((services->capabilities & HL_HOST_CAP_CODE_MAPPING) != 0 &&
        (services->memory == NULL || services->memory->size < sizeof(*services->memory) ||
         services->memory->reserve_code == NULL || services->memory->repair_code_after_fork == NULL))
        return HL_STATUS_ABI_MISMATCH;
    if ((services->capabilities & HL_HOST_CAP_CODE_MAPPING) != 0 &&
        (services->memory->begin_code_write == NULL || services->memory->end_code_write == NULL))
        return HL_STATUS_ABI_MISMATCH;
    if ((services->capabilities & HL_HOST_CAP_CLOCK) != 0) {
        const hl_host_clock_services *clock = services->clock;
        if (clock == NULL || clock->abi != HL_HOST_CLOCK_ABI || clock->size < sizeof(*clock) ||
            clock->monotonic_ns == NULL || clock->realtime_ns == NULL || clock->raw_monotonic_ns == NULL ||
            clock->process_cpu_ns == NULL || clock->thread_cpu_ns == NULL || clock->sleep_until == NULL)
            return HL_STATUS_ABI_MISMATCH;
    }
    if ((services->capabilities & HL_HOST_CAP_LOG) != 0 &&
        (!hl_valid_group(services->log, HL_HOST_LOG_ABI, sizeof(*services->log)) || services->log->emit == NULL))
        return HL_STATUS_ABI_MISMATCH;
    if ((services->capabilities & HL_HOST_CAP_POSIX_ATTACHMENT) != 0 &&
        (!hl_valid_group(services->posix_attachment, HL_HOST_POSIX_ATTACHMENT_ABI,
                         sizeof(*services->posix_attachment)) ||
         services->posix_attachment->borrow_file == NULL || services->posix_attachment->release == NULL))
        return HL_STATUS_ABI_MISMATCH;
    if ((services->capabilities & HL_HOST_CAP_FILE) != 0 &&
        (!hl_valid_file_group(services->file) ||
         services->file->open_relative == NULL || services->file->read_at == NULL || services->file->write_at == NULL ||
         services->file->append == NULL || services->file->metadata == NULL || services->file->close == NULL ||
         services->file->read == NULL || services->file->write == NULL || services->file->clone_for_fork == NULL ||
         services->file->seek == NULL || services->file->rename_relative == NULL ||
         services->file->unlink_relative == NULL || services->file->path == NULL ||
         services->file->standard_stream == NULL || services->file->readlink == NULL ||
         services->file->set_owner == NULL || services->file->resolve_beneath == NULL ||
         services->file->sync_range == NULL || services->file->sync_filesystem == NULL ||
         services->file->open_beneath == NULL ||
         (services->file->abi == HL_HOST_FILE_ABI &&
          (services->file->allocate_range == NULL || services->file->filesystem_metadata == NULL ||
           services->file->set_permissions == NULL || services->file->set_times == NULL ||
           services->file->read_directory == NULL || services->file->make_directory == NULL ||
           services->file->make_symlink == NULL || services->file->make_link == NULL ||
           services->file->make_fifo == NULL || services->file->validate_private_regular == NULL ||
           services->file->store_private_atomic == NULL || services->file->validate_private_directory == NULL))))
        return HL_STATUS_ABI_MISMATCH;
    if ((services->capabilities & HL_HOST_CAP_PROCESS) != 0 &&
        (!hl_valid_group(services->process, HL_HOST_PROCESS_ABI, sizeof(*services->process)) ||
         services->process->spawn_cloned == NULL || services->process->wait == NULL ||
         services->process->terminate == NULL || services->process->close == NULL ||
         services->process->spawn_prepared == NULL))
        return HL_STATUS_ABI_MISMATCH;
    if ((services->capabilities & HL_HOST_CAP_EVENT) != 0 &&
        (!hl_valid_group(services->event, HL_HOST_EVENT_ABI, sizeof(*services->event)) ||
         services->event->create == NULL || services->event->control == NULL || services->event->wait == NULL ||
         services->event->wake == NULL || services->event->close == NULL))
        return HL_STATUS_ABI_MISMATCH;
    if ((services->capabilities & HL_HOST_CAP_EVENT_TIMER) != 0 &&
        (!hl_valid_group(services->event, HL_HOST_EVENT_ABI, sizeof(*services->event)) ||
         services->event->create == NULL || services->event->wait == NULL || services->event->wake == NULL ||
         services->event->close == NULL || services->event->arm_timer == NULL || services->event->disarm_timer == NULL))
        return HL_STATUS_ABI_MISMATCH;
    if ((services->capabilities & HL_HOST_CAP_NETWORK) != 0 &&
        (!hl_valid_group(services->network, HL_HOST_NETWORK_ABI, sizeof(*services->network)) ||
         services->network->socket == NULL || services->network->bind == NULL || services->network->connect == NULL ||
         services->network->send == NULL || services->network->receive == NULL || services->network->close == NULL))
        return HL_STATUS_ABI_MISMATCH;
    if ((services->capabilities & HL_HOST_CAP_SHARED_MEMORY) != 0 &&
        (!hl_valid_group(services->shared_memory, HL_HOST_SHARED_MEMORY_ABI, sizeof(*services->shared_memory)) ||
         services->shared_memory->create == NULL || services->shared_memory->open == NULL ||
         services->shared_memory->resize == NULL || services->shared_memory->close == NULL))
        return HL_STATUS_ABI_MISMATCH;
    if ((services->capabilities & HL_HOST_CAP_SYNC) != 0 &&
        (!hl_has_field(services->size, offsetof(hl_host_services, sync), sizeof(services->sync)) ||
         !hl_valid_group(services->sync, HL_HOST_SYNC_ABI, sizeof(*services->sync)) ||
         services->sync->mutex_create == NULL || services->sync->mutex_lock == NULL ||
         services->sync->mutex_unlock == NULL || services->sync->mutex_close == NULL ||
         services->sync->fork_prepare == NULL || services->sync->fork_parent == NULL ||
         services->sync->fork_child == NULL))
        return HL_STATUS_ABI_MISMATCH;
    if ((services->capabilities & HL_HOST_CAP_COUNTER) != 0 &&
        (!hl_has_field(services->size, offsetof(hl_host_services, counter), sizeof(services->counter)) ||
         !hl_valid_group(services->counter, HL_HOST_COUNTER_ABI, sizeof(*services->counter)) ||
         services->counter->create == NULL || services->counter->read == NULL || services->counter->write == NULL ||
         services->counter->get_flags == NULL || services->counter->set_flags == NULL ||
         services->counter->duplicate == NULL || services->counter->readiness == NULL ||
         services->counter->subscribe == NULL || services->counter->unsubscribe == NULL ||
         services->counter->close == NULL))
        return HL_STATUS_ABI_MISMATCH;
    if ((services->capabilities & HL_HOST_CAP_TRANSFER) != 0 &&
        (!hl_has_field(services->size, offsetof(hl_host_services, transfer), sizeof(services->transfer)) ||
         !hl_valid_group(services->transfer, HL_HOST_TRANSFER_ABI, sizeof(*services->transfer)) ||
         services->transfer->channel_pair == NULL || services->transfer->send == NULL ||
         services->transfer->receive == NULL || services->transfer->duplicate == NULL ||
         services->transfer->close == NULL))
        return HL_STATUS_ABI_MISMATCH;
    if ((services->capabilities & HL_HOST_CAP_DIRECTORY) != 0 &&
        (!hl_has_field(services->size, offsetof(hl_host_services, directory), sizeof(services->directory)) ||
         !hl_valid_group(services->directory, HL_HOST_DIRECTORY_ABI, sizeof(*services->directory)) ||
         services->directory->create == NULL || services->directory->add == NULL ||
         services->directory->modify == NULL || services->directory->remove == NULL ||
         services->directory->read == NULL || services->directory->duplicate == NULL ||
         services->directory->close == NULL))
        return HL_STATUS_ABI_MISMATCH;
    if ((services->capabilities & HL_HOST_CAP_WATCH) != 0 &&
        (!hl_has_field(services->size, offsetof(hl_host_services, watch), sizeof(services->watch)) ||
         !hl_valid_group(services->watch, HL_HOST_WATCH_ABI, sizeof(*services->watch)) ||
         services->watch->open == NULL || services->watch->query == NULL || services->watch->drain == NULL ||
         services->watch->close == NULL))
        return HL_STATUS_ABI_MISMATCH;
    if ((services->capabilities & HL_HOST_CAP_STREAM) != 0 &&
        (!hl_has_field(services->size, offsetof(hl_host_services, stream), sizeof(services->stream)) ||
         !hl_valid_group(services->stream, HL_HOST_STREAM_ABI, sizeof(*services->stream)) ||
         services->stream->pipe_pair == NULL || services->stream->read == NULL || services->stream->write == NULL ||
         services->stream->duplicate == NULL || services->stream->close == NULL ||
         services->stream->set_status_flags == NULL || services->stream->readiness == NULL ||
         services->stream->move == NULL))
        return HL_STATUS_ABI_MISMATCH;
    return HL_STATUS_OK;
}
