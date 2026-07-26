#include "hl/activation.h"
#include "hl/config.h"

#import <Foundation/Foundation.h>

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int write_full(int descriptor, const void *data, size_t size) {
    const unsigned char *bytes = data;
    size_t offset = 0;
    while (offset < size) {
        ssize_t count = write(descriptor, bytes + offset, size - offset);
        if (count <= 0) return -1;
        offset += (size_t)count;
    }
    return 0;
}

static int config(const char *guest, uint64_t sequence, char path[64]) {
    hl_launch_config launch = {0};
    char pool[4096] = {0};
    size_t guest_size = strlen(guest) + 1;
    int descriptor;
    if (guest_size + 2 > sizeof(pool)) return -1;
    strcpy(path, "/tmp/hl-activation-objc.XXXXXX");
    descriptor = mkstemp(path);
    if (descriptor < 0) return -1;
    memcpy(pool + 1, guest, guest_size);
    launch.magic = HL_CONFIG_MAGIC;
    launch.abi = HL_CONFIG_ABI;
    launch.header_size = sizeof(launch);
    launch.pool_size = (uint32_t)(guest_size + 2);
    launch.uid = -1;
    launch.gid = -1;
    launch.process_domain[0] = (uint64_t)getpid();
    launch.process_domain[1] = sequence;
    launch.arguments_offset = 1;
    launch.executable_host_offset = 1;
    if (write_full(descriptor, &launch, sizeof(launch)) != 0 ||
        write_full(descriptor, pool, launch.pool_size) != 0 || close(descriptor) != 0) {
        close(descriptor);
        unlink(path);
        return -1;
    }
    return 0;
}

// Every failure path names itself: this consumer's only channel to CI is a log
// tail, where a bare exit status says nothing about which of the 50 spawns broke.
static int fail(int code, const char *what) {
    fprintf(stderr, "activation-objc-package: %s (exit %d)\n", what, code);
    return code;
}

int main(int argc, char **argv) {
    if (argc != 2) return fail(64, "usage: activation-objc-package <aarch64-guest>");
    @autoreleasepool {
        NSString *consumer = [NSString stringWithUTF8String:"activation-package"];
        if (consumer.length != 18) return fail(65, "Foundation is not initialised: NSString length is wrong");
        for (uint64_t attempt = 1; attempt <= 50; ++attempt) {
            hl_engine_exit result = {.abi = HL_ENGINE_ABI, .size = sizeof(result)};
            char config_path[64];
            if (config(argv[1], attempt, config_path) != 0)
                return fail(66, "could not write the launch config to /tmp");
            hl_status status = hl_activation_spawn(argv[0], HL_GUEST_ISA_AARCH64, config_path, &result);
            if (status != HL_STATUS_OK || result.kind != HL_ENGINE_EXIT_CODE || result.guest_status != 42) {
                fprintf(stderr, "activation-objc-package: attempt=%llu of 50 status=%d kind=%u guest=%d\n",
                        (unsigned long long)attempt, status, result.kind, result.guest_status);
                return fail(67, "a post-Foundation-init guest spawn did not exit 42");
            }
        }
    }
    return 0;
}
