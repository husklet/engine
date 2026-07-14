#ifndef HL_FAKE_H
#define HL_FAKE_H

#include "hl/host_services.h"

HL_EXTERN_C_BEGIN

typedef struct hl_fake_host {
    uint64_t monotonic_ns;
    uint64_t realtime_ns;
    uint64_t raw_monotonic_ns;
    uint64_t process_cpu_ns;
    uint64_t thread_cpu_ns;
    uint64_t next_handle;
    uint32_t live_mappings;
    uint32_t live_processes;
    uint32_t process_waited;
    uint32_t process_block_wait;
    uint32_t process_exit_kind;
    uint32_t live_mutexes;
    uint32_t code_write_begins;
    uint32_t code_write_ends;
    uint64_t mutex_handles[64];
    uint8_t mutex_locked[64];
    uint64_t counter_handles[64];
    uint8_t counter_objects[64];
    uint64_t counter_values[64];
    uint32_t counter_flags[64];
    uint32_t counter_references[64];
    uint32_t counter_rights[64];
    uint32_t live_counters;
    uint64_t transfer_channels[64];
    uint8_t transfer_endpoints[64];
    uint8_t transfer_peers[64];
    uint8_t transfer_references[64];
    uint8_t transfer_message_pending[64];
    uint16_t transfer_data_sizes[64];
    uint8_t transfer_attachment_counts[64];
    uint8_t transfer_data[64][HL_HOST_TRANSFER_MAX_DATA];
    uint8_t transfer_objects[64][HL_HOST_TRANSFER_MAX_ATTACHMENTS];
    uint32_t transfer_rights[64][HL_HOST_TRANSFER_MAX_ATTACHMENTS];
    uint32_t live_transfer_channels;
    uint64_t directory_handles[16];
    uint8_t directory_objects[16];
    uint32_t directory_references[16];
    uint64_t directory_tokens[16][16];
    uint32_t directory_interests[16][16];
    hl_host_directory_record directory_records[16][64];
    uint8_t directory_record_counts[16];
    uint64_t event_handles[16];
    uint8_t event_directories[16];
    uint64_t event_tokens[16];
    int32_t process_exit_value;
    hl_status next_failure;
} hl_fake_host;

HL_API void hl_fake_host_init(hl_fake_host *fake, hl_host_services *services);
HL_API void hl_fake_host_fail_next(hl_fake_host *fake, hl_status status);
HL_API void hl_fake_host_block_process_wait(hl_fake_host *fake, uint32_t block);
HL_API void hl_fake_host_directory_emit(hl_fake_host *fake, uint64_t token, uint32_t changes);

HL_EXTERN_C_END

#endif
