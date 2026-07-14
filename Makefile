CC ?= cc
AR ?= ar
CLANG_FORMAT ?= clang-format
BUILD ?= build
HOST ?= linux
DEBUG ?= 0
MAC ?= mac
AARCH64_LINUX_CC ?= aarch64-linux-gnu-gcc
X86_64_LINUX_CC ?= x86_64-linux-gnu-gcc
AARCH64_DYNAMIC_LOADER ?= /usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1
AARCH64_DYNAMIC_LIBC ?= /usr/lib/aarch64-linux-gnu/libc.so.6
X86_64_DYNAMIC_LOADER ?= /usr/x86_64-linux-gnu/lib/ld-linux-x86-64.so.2
X86_64_DYNAMIC_LIBC ?= /usr/x86_64-linux-gnu/lib/libc.so.6

CPPFLAGS := -Iinclude -DHL_ENABLE_LOGGING=$(DEBUG)
CFLAGS ?= -O2 -g
WARNINGS := -std=c11 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wstrict-prototypes -Wmissing-prototypes
ENGINE_CFLAGS := $(CFLAGS) $(WARNINGS) -fvisibility=hidden
DEPFLAGS := -MMD -MP
PRIVATE_HEADERS := src/core/cli.h src/host/sync.h src/linux_abi/encode.h src/linux_abi/seccomp_vm.h src/translator/guest/x86_64/decoder.h \
	src/translator/host/aarch64/aarch64_codegen.h src/translator/host/x86_64/x86_64_codegen.h

# Production engines are unity translation units: their target .c files textually include the engine,
# translator, and Linux-personality implementation. Make cannot discover those nested includes from the
# compiler command, so conservatively track the complete production C/header tree plus public HL headers.
# This GNU Make-only recursive wildcard keeps newly added included files in the dependency closure without
# a generated list or a clean build.
rwildcard = $(foreach entry,$(wildcard $1*),$(call rwildcard,$(entry)/,$2) $(filter $(subst *,%,$2),$(entry)))
PRODUCTION_UNITY_DEPS := $(sort $(call rwildcard,src/core/,*.c) $(call rwildcard,src/core/,*.h) \
	$(call rwildcard,src/host/,*.c) $(call rwildcard,src/host/,*.h) \
	$(call rwildcard,src/linux_abi/,*.c) $(call rwildcard,src/linux_abi/,*.h) \
	$(call rwildcard,src/translator/,*.c) $(call rwildcard,src/translator/,*.h) \
	$(call rwildcard,include/hl/,*.h))

CORE_SOURCES := src/core/cli.c src/core/config.c src/core/engine.c src/core/host_services.c src/core/launch.c src/core/log.c \
	src/core/options.c
IR_SOURCES := src/translator/arena.c src/translator/codegen.c src/translator/digest.c src/translator/identity.c src/translator/reloc.c \
	src/translator/window.c src/translator/guest/x86_64/decode.c src/translator/host/aarch64/codegen.c \
	src/translator/host/x86_64/codegen.c src/translator/ir/interpreter.c \
	src/translator/ir/ir.c
LINUX_ABI_SOURCES := src/linux_abi/affinity.c src/linux_abi/container/vfs/gmap.c src/linux_abi/device.c \
	src/linux_abi/encode.c src/linux_abi/fdcache.c \
	src/linux_abi/errno.c src/linux_abi/limits.c src/linux_abi/linux_abi.c src/linux_abi/number.c \
	src/linux_abi/parse.c src/linux_abi/readonly.c src/linux_abi/seccomp_vm.c src/linux_abi/stat.c src/linux_abi/xattr.c
FAKE_HOST_SOURCES := src/host/fake/host.c
MACOS_HOST_SOURCES := src/host/macos/host.c
COMMON_HOST_SOURCES := src/host/sync.c
MAC_LINUX_ABI_SOURCES := $(LINUX_ABI_SOURCES)
MAC_HOST_SOURCES := $(MACOS_HOST_SOURCES) $(COMMON_HOST_SOURCES) src/host/clock.c src/host/file.c
MAC_CORE_OBJECTS := $(CORE_SOURCES:%.c=$(BUILD)/mac/%.o)
MAC_TRANSLATOR_OBJECTS := $(IR_SOURCES:%.c=$(BUILD)/mac/%.o)
MAC_LINUX_ABI_OBJECTS := $(MAC_LINUX_ABI_SOURCES:%.c=$(BUILD)/mac/%.o)
MAC_HOST_OBJECTS := $(MAC_HOST_SOURCES:%.c=$(BUILD)/mac/%.o)
MAC_LIBS := $(BUILD)/mac/lib/libhl-engine.a $(BUILD)/mac/lib/libhl-translator.a \
	$(BUILD)/mac/lib/libhl-linux-abi.a $(BUILD)/mac/lib/libhl-host-macos.a
PORTABLE_SOURCES := $(CORE_SOURCES) $(IR_SOURCES) $(LINUX_ABI_SOURCES) $(FAKE_HOST_SOURCES) $(COMMON_HOST_SOURCES)
CORE_OBJECTS := $(CORE_SOURCES:%.c=$(BUILD)/%.o)
TRANSLATOR_OBJECTS := $(IR_SOURCES:%.c=$(BUILD)/%.o)
LINUX_ABI_OBJECTS := $(LINUX_ABI_SOURCES:%.c=$(BUILD)/%.o)
FAKE_HOST_OBJECTS := $(FAKE_HOST_SOURCES:%.c=$(BUILD)/%.o)

ifeq ($(HOST),linux)
LINUX_HOST_SOURCES := src/host/linux/host.c $(COMMON_HOST_SOURCES)
LINUX_HOST_OBJECTS := $(LINUX_HOST_SOURCES:%.c=$(BUILD)/%.o)
LINUX_HOST_PRODUCTS := $(BUILD)/lib/libhl-host-linux.a
LINUX_HOST_TEST := run-unit-linux
endif

NATIVE_OBJECTS := $(CORE_OBJECTS) $(TRANSLATOR_OBJECTS) $(LINUX_ABI_OBJECTS) $(FAKE_HOST_OBJECTS) \
	$(LINUX_HOST_OBJECTS)
MAC_OBJECTS := $(MAC_CORE_OBJECTS) $(MAC_TRANSLATOR_OBJECTS) $(MAC_LINUX_ABI_OBJECTS) $(MAC_HOST_OBJECTS)
MAC_AUX_OBJECTS := $(BUILD)/mac/target/aarch64.o $(BUILD)/mac/target/x86_64.o \
	$(BUILD)/mac/lifecycle/aarch64-target.o $(BUILD)/mac/lifecycle/x86_64-target.o \
	$(BUILD)/mac/lifecycle/aarch64-core.o $(BUILD)/mac/lifecycle/x86_64-core.o \
	$(BUILD)/mac/lifecycle/aarch64-runner.o $(BUILD)/mac/lifecycle/x86_64-runner.o

BINDING_AUX_OBJECTS := $(BUILD)/mac/binding/aarch64-runner.o $(BUILD)/mac/binding/x86_64-runner.o \
	$(BUILD)/mac/stdio/aarch64-runner.o $(BUILD)/mac/stdio/x86_64-runner.o \
	$(BUILD)/mac/dir/aarch64-runner.o $(BUILD)/mac/dir/x86_64-runner.o
DEPENDENCY_FILES := $(NATIVE_OBJECTS:.o=.d) $(MAC_OBJECTS:.o=.d) $(MAC_AUX_OBJECTS:.o=.d) \
	$(BINDING_AUX_OBJECTS:.o=.d)

UNIT_NAMES := affinity arena cli clock codegen config decoder device digest emit fdcache file gmap host_services identity ir launch linux_abi linux_fork seccomp_vm stat engine errno limits log namespace number options parse profile readonly reloc window xattr_cache
UNIT_BINS := $(UNIT_NAMES:%=$(BUILD)/tests/test_%)
UNIT_RUN_TARGETS := $(UNIT_NAMES:%=run-unit-%)

FIXTURE_SOURCES := $(sort $(wildcard tests/compat/fixtures/*.c))
FIXTURE_BINS := $(FIXTURE_SOURCES:tests/compat/fixtures/%.c=$(BUILD)/fixtures/%)
NATIVE_SMOKE := atomics clockelapsed epoll epoll_edge eventfd eventfd_sema forkwait mmapanon mmapshared seccomp statx_agree sysv_ipc timerfd
NATIVE_SMOKE_BINS := $(NATIVE_SMOKE:%=$(BUILD)/fixtures/%)
E2E_CASES := atomics clockelapsed epoll epoll_dup_lifetime epoll_edge epoll_et epoll_fork_inherit epoll_highfd \
	epoll_mod epoll_oneshot epoll_reblock_inf eventfd eventfd_nonblock eventfd_sema fd_reuse_guard forkwait \
	futex futex_shared_key futex_xproc inotify inotify_moves mmapanon mmapshared procreap seccomp signalfd \
	signalfd_multi signalfd_rt signals stat_layout statx_agree sysv_ipc timerfd timerfd_interval
E2E_CASE_BINS := $(E2E_CASES:%=$(BUILD)/e2e/%-aarch64) $(E2E_CASES:%=$(BUILD)/e2e/%-x86_64)
E2E_CASE_RUNS := $(E2E_CASES:%=run-e2e-compat-%)
ABI_CASE_SOURCES := $(sort $(wildcard tests/compat/abi/*.c))
ABI_CASE_NAMES := $(basename $(notdir $(ABI_CASE_SOURCES)))
ABI_CASE_BINS := $(ABI_CASE_NAMES:%=$(BUILD)/compat/abi/aarch64/%) \
	$(ABI_CASE_NAMES:%=$(BUILD)/compat/abi/x86_64/%)
LIBC_CASE_SOURCES := $(sort $(wildcard tests/compat/libc/*.c))
LIBC_CASE_NAMES := $(basename $(notdir $(LIBC_CASE_SOURCES)))
LIBC_CASE_BINS := $(LIBC_CASE_NAMES:%=$(BUILD)/compat/libc/aarch64/%) \
	$(LIBC_CASE_NAMES:%=$(BUILD)/compat/libc/x86_64/%)
COMPLETENESS_AARCH64_SOURCES := $(sort $(wildcard tests/compat/completeness/aarch64/*.c))
COMPLETENESS_X86_64_SOURCES := $(sort $(wildcard tests/compat/completeness/x86_64/*.c))
COMPLETENESS_SYSCALL_SOURCES := $(sort $(wildcard tests/compat/completeness/syscall/*.c))
COMPLETENESS_AARCH64_BINS := $(COMPLETENESS_AARCH64_SOURCES:tests/compat/completeness/%.c=$(BUILD)/compat/completeness/aarch64/%) \
	$(COMPLETENESS_SYSCALL_SOURCES:tests/compat/completeness/%.c=$(BUILD)/compat/completeness/aarch64/%)
COMPLETENESS_X86_64_BINS := $(COMPLETENESS_X86_64_SOURCES:tests/compat/completeness/%.c=$(BUILD)/compat/completeness/x86_64/%) \
	$(COMPLETENESS_SYSCALL_SOURCES:tests/compat/completeness/%.c=$(BUILD)/compat/completeness/x86_64/%)
COMPLETENESS_BINS := $(COMPLETENESS_AARCH64_BINS) $(COMPLETENESS_X86_64_BINS)
POSIX_CASE_SOURCES := $(sort $(wildcard tests/compat/posix/*.c))
POSIX_CASE_NAMES := $(basename $(notdir $(POSIX_CASE_SOURCES)))
POSIX_CASE_BINS := $(POSIX_CASE_NAMES:%=$(BUILD)/compat/posix/aarch64/%) \
	$(POSIX_CASE_NAMES:%=$(BUILD)/compat/posix/x86_64/%)
SYSCALL_CASE_SOURCES := $(sort $(wildcard tests/compat/syscall/*.c))
SYSCALL_CASE_NAMES := $(basename $(notdir $(SYSCALL_CASE_SOURCES)))
SYSCALL_CASE_BINS := $(SYSCALL_CASE_NAMES:%=$(BUILD)/compat/syscall/aarch64/%) \
	$(SYSCALL_CASE_NAMES:%=$(BUILD)/compat/syscall/x86_64/%)
NETWORK_CASE_SOURCES := $(sort $(wildcard tests/compat/network/*.c))
NETWORK_CASE_NAMES := $(basename $(notdir $(NETWORK_CASE_SOURCES)))
NETWORK_CASE_BINS := $(NETWORK_CASE_NAMES:%=$(BUILD)/compat/network/aarch64/%) \
	$(NETWORK_CASE_NAMES:%=$(BUILD)/compat/network/x86_64/%)
PROCFS_CASE_SOURCES := $(sort $(wildcard tests/compat/procfs/*.c))
PROCFS_CASE_NAMES := $(basename $(notdir $(PROCFS_CASE_SOURCES)))
PROCFS_CASE_BINS := $(PROCFS_CASE_NAMES:%=$(BUILD)/compat/procfs/aarch64/%) \
	$(PROCFS_CASE_NAMES:%=$(BUILD)/compat/procfs/x86_64/%)
MEMORY_CASE_SOURCES := $(sort $(wildcard tests/compat/memory/*.c))
MEMORY_CASE_NAMES := $(basename $(notdir $(MEMORY_CASE_SOURCES)))
MEMORY_CASE_BINS := $(MEMORY_CASE_NAMES:%=$(BUILD)/compat/memory/aarch64/%) \
	$(MEMORY_CASE_NAMES:%=$(BUILD)/compat/memory/x86_64/%)
FILESYSTEM_CASE_SOURCES := $(sort $(wildcard tests/compat/filesystem/*.c tests/compat/filesystem/*/*.c))
FILESYSTEM_CASE_NAMES := $(patsubst tests/compat/filesystem/%.c,%,$(FILESYSTEM_CASE_SOURCES))
FILESYSTEM_CASE_BINS := $(FILESYSTEM_CASE_NAMES:%=$(BUILD)/compat/filesystem/aarch64/%) \
	$(FILESYSTEM_CASE_NAMES:%=$(BUILD)/compat/filesystem/x86_64/%)
SIGNALS_CASE_SOURCES := $(sort $(wildcard tests/compat/signals/*.c))
SIGNALS_CASE_NAMES := $(basename $(notdir $(SIGNALS_CASE_SOURCES)))
SIGNALS_CASE_BINS := $(SIGNALS_CASE_NAMES:%=$(BUILD)/compat/signals/aarch64/%) \
	$(SIGNALS_CASE_NAMES:%=$(BUILD)/compat/signals/x86_64/%)
PROCESS_CASE_SOURCES := $(sort $(wildcard tests/compat/process/*.c tests/compat/process/*/*.c))
PROCESS_CASE_NAMES := $(patsubst tests/compat/process/%.c,%,$(PROCESS_CASE_SOURCES))
PROCESS_CASE_BINS := $(PROCESS_CASE_NAMES:%=$(BUILD)/compat/process/aarch64/%) \
	$(PROCESS_CASE_NAMES:%=$(BUILD)/compat/process/x86_64/%)
TIME_CASE_SOURCES := $(sort $(wildcard tests/compat/time/*.c))
TIME_CASE_NAMES := $(basename $(notdir $(TIME_CASE_SOURCES)))
TIME_CASE_BINS := $(TIME_CASE_NAMES:%=$(BUILD)/compat/time/aarch64/%) \
	$(TIME_CASE_NAMES:%=$(BUILD)/compat/time/x86_64/%)
ISA_X86_64_FIXTURES := ctest_x64 g_x64 go_goro_x86 go_heapgc_x86 gw hello_x86 hx
ISA_X86_64_BINS := $(ISA_X86_64_FIXTURES:%=$(BUILD)/compat/isa/x86_64/%)
# These are committed binary inputs, not native host build targets; suppress built-in .c rules.
$(ISA_X86_64_FIXTURES:%=tests/compat/isa/x86_64/%): ;
CORE_ABI_BOTH := hello math strings bitops varargs longjmp recursion fnptr jumptable ibtc_dispatch floatmath \
	heap qsort files statfile pipe mmapanon munmap_partial regex globmatch strtod timefmt environ atexit \
	sigaction2 sigjmp sortbig
CORE_ABI_AARCH64 := $(CORE_ABI_BOTH) stolen_regs
CORE_ABI_X86_64 := $(CORE_ABI_BOTH) moffs fpedge fpdnan repmovsdf x87m80 shldflags
CORE_ABI_BINS := $(CORE_ABI_AARCH64:%=$(BUILD)/compat/core/abi/aarch64/%) \
	$(CORE_ABI_X86_64:%=$(BUILD)/compat/core/abi/x86_64/%)
CORE_WORKLOAD_BOTH := busyloop ibtc_dispatch bigmem bigarr soak_codecache soak_indirect soak_threadchurn \
	soak_forkchurn soak_allocchurn smc_mprotect
CORE_WORKLOAD_AARCH64 := $(CORE_WORKLOAD_BOTH) dbserver soak_smc smc_threads smc_selfflush sqlite
CORE_WORKLOAD_X86_64 := $(CORE_WORKLOAD_BOTH) smc_remap_reuse smc_mremap smc_table_overflow
CORE_WORKLOAD_BINS := $(CORE_WORKLOAD_AARCH64:%=$(BUILD)/compat/core/workload/aarch64/%) \
	$(CORE_WORKLOAD_X86_64:%=$(BUILD)/compat/core/workload/x86_64/%)
CORE_SYSCALL_SOURCES := $(sort $(wildcard tests/compat/core/syscall/*.c))
CORE_SYSCALL_NAMES := $(basename $(notdir $(CORE_SYSCALL_SOURCES)))
CORE_SYSCALL_BINS := $(CORE_SYSCALL_NAMES:%=$(BUILD)/compat/core/syscall/aarch64/%) \
	$(CORE_SYSCALL_NAMES:%=$(BUILD)/compat/core/syscall/x86_64/%)
CORE_REGRESS_BOTH := lseek_read offset_track sha512_kat ccmp_test getaffinity_tid stackoverflow stackoverflow_catch
CORE_REGRESS_AARCH64 := $(CORE_REGRESS_BOTH) nonpie_ldapr nonpie_pairatomics ldrsw_literal go_cgo_stackgrow_arm
CORE_REGRESS_X86_64 := $(CORE_REGRESS_BOTH) nonpie_vec repcmps_nopie nonpie_v8blob
CORE_REGRESS_BINS := $(CORE_REGRESS_AARCH64:%=$(BUILD)/compat/core/regress/aarch64/%) \
	$(CORE_REGRESS_X86_64:%=$(BUILD)/compat/core/regress/x86_64/%)
tests/compat/core/regress/go_cgo_stackgrow_arm: ;
ISOLATION_CASE_SOURCES := $(sort $(wildcard tests/compat/isolation/*.c))
ISOLATION_CASE_NAMES := $(basename $(notdir $(ISOLATION_CASE_SOURCES)))
ISOLATION_CASE_BINS := $(ISOLATION_CASE_NAMES:%=$(BUILD)/compat/isolation/aarch64/%) \
	$(ISOLATION_CASE_NAMES:%=$(BUILD)/compat/isolation/x86_64/%)
SYSCALL_EDGE_CASE_SOURCES := $(sort $(wildcard tests/compat/syscall_edges/*.c))
SYSCALL_EDGE_CASE_NAMES := $(basename $(notdir $(SYSCALL_EDGE_CASE_SOURCES)))
SYSCALL_EDGE_CASE_BINS := $(SYSCALL_EDGE_CASE_NAMES:%=$(BUILD)/compat/syscall_edges/aarch64/%) \
	$(SYSCALL_EDGE_CASE_NAMES:%=$(BUILD)/compat/syscall_edges/x86_64/%)
SOAK_CASE_SOURCES := $(sort $(wildcard tests/soak/*.c))
SOAK_CASE_NAMES := $(basename $(notdir $(SOAK_CASE_SOURCES)))
SOAK_CASE_BINS := $(SOAK_CASE_NAMES:%=$(BUILD)/soak/aarch64/%) \
	$(SOAK_CASE_NAMES:%=$(BUILD)/soak/x86_64/%)

.PHONY: all clean test unit $(UNIT_RUN_TARGETS) test-debug-log test-macos compat-build compat-native compat-engines dynamic-e2e e2e-compat \
	compat-abi compat-core compat-core-abi compat-core-regress compat-core-syscall compat-core-workload compat-filesystem compat-isa-x86-64 compat-isolation compat-libc compat-completeness compat-memory compat-network compat-posix compat-process compat-procfs compat-signals compat-soak compat-syscall compat-syscall-edges compat-time $(E2E_CASE_RUNS) perf-compat check-domains format format-check help

all: $(BUILD)/lib/libhl-engine.a $(BUILD)/lib/libhl-translator.a $(BUILD)/lib/libhl-linux-abi.a \
	$(BUILD)/lib/libhl-host-fake.a $(LINUX_HOST_PRODUCTS) $(BUILD)/bin/hl-engine-runner

$(BUILD)/src/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(ENGINE_CFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD)/mac/%.o: %.c
	@mkdir -p $(@D)
	$(MAC) clang $(CPPFLAGS) $(ENGINE_CFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD)/mac/lib/libhl-engine.a: $(MAC_CORE_OBJECTS)
	@mkdir -p $(@D)
	$(MAC) ar rcs $@ $^

$(BUILD)/mac/lib/libhl-translator.a: $(MAC_TRANSLATOR_OBJECTS)
	@mkdir -p $(@D)
	$(MAC) ar rcs $@ $^

$(BUILD)/mac/lib/libhl-linux-abi.a: $(MAC_LINUX_ABI_OBJECTS)
	@mkdir -p $(@D)
	$(MAC) ar rcs $@ $^

$(BUILD)/mac/lib/libhl-host-macos.a: $(MAC_HOST_OBJECTS)
	@mkdir -p $(@D)
	$(MAC) ar rcs $@ $^

$(BUILD)/lib/libhl-engine.a: $(CORE_OBJECTS)
	@mkdir -p $(@D)
	$(AR) rcs $@ $^

$(BUILD)/lib/libhl-translator.a: $(TRANSLATOR_OBJECTS)
	@mkdir -p $(@D)
	$(AR) rcs $@ $^

$(BUILD)/lib/libhl-linux-abi.a: $(LINUX_ABI_OBJECTS)
	@mkdir -p $(@D)
	$(AR) rcs $@ $^

$(BUILD)/lib/libhl-host-fake.a: $(FAKE_HOST_OBJECTS)
	@mkdir -p $(@D)
	$(AR) rcs $@ $^

$(BUILD)/lib/libhl-host-linux.a: $(LINUX_HOST_OBJECTS)
	@mkdir -p $(@D)
	$(AR) rcs $@ $^

$(BUILD)/bin/hl-engine-runner: src/runner/main.c $(BUILD)/lib/libhl-engine.a $(BUILD)/lib/libhl-translator.a \
	$(BUILD)/lib/libhl-linux-abi.a
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(ENGINE_CFLAGS) $< $(BUILD)/lib/libhl-engine.a $(BUILD)/lib/libhl-translator.a \
		$(BUILD)/lib/libhl-linux-abi.a -o $@

$(BUILD)/tests/test_%: tests/unit/test_%.c $(BUILD)/lib/libhl-engine.a $(BUILD)/lib/libhl-translator.a \
	$(BUILD)/lib/libhl-linux-abi.a $(BUILD)/lib/libhl-host-fake.a
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -Itests/unit $(ENGINE_CFLAGS) $< $(BUILD)/lib/libhl-engine.a \
		$(BUILD)/lib/libhl-translator.a $(BUILD)/lib/libhl-linux-abi.a $(BUILD)/lib/libhl-host-fake.a -o $@

$(BUILD)/tests/test_linux_abi: tests/unit/test_linux_abi.c $(BUILD)/lib/libhl-engine.a \
	$(BUILD)/lib/libhl-translator.a $(BUILD)/lib/libhl-linux-abi.a $(BUILD)/lib/libhl-host-fake.a
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -Itests/unit $(ENGINE_CFLAGS) -pthread $< $(BUILD)/lib/libhl-engine.a \
		$(BUILD)/lib/libhl-translator.a $(BUILD)/lib/libhl-linux-abi.a $(BUILD)/lib/libhl-host-fake.a -o $@

$(BUILD)/tests/test_fdcache: tests/unit/test_fdcache.c $(BUILD)/lib/libhl-linux-abi.a $(BUILD)/lib/libhl-host-fake.a
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -Isrc/linux_abi -Itests/unit $(ENGINE_CFLAGS) $< $(BUILD)/lib/libhl-linux-abi.a \
		$(BUILD)/lib/libhl-host-fake.a -o $@

$(BUILD)/tests/test_linux_fork: tests/unit/test_linux_fork.c $(BUILD)/lib/libhl-linux-abi.a \
	$(BUILD)/lib/libhl-host-linux.a
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -Itests/unit $(ENGINE_CFLAGS) $< $(BUILD)/lib/libhl-linux-abi.a \
		$(BUILD)/lib/libhl-host-linux.a -pthread -o $@

$(BUILD)/tests/test_seccomp_vm: tests/unit/test_seccomp_vm.c $(BUILD)/lib/libhl-linux-abi.a
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -Isrc/linux_abi -Itests/unit $(ENGINE_CFLAGS) $< $(BUILD)/lib/libhl-linux-abi.a -o $@

$(BUILD)/tests/test_limits: tests/unit/test_limits.c $(BUILD)/lib/libhl-linux-abi.a
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -Itests/unit $(ENGINE_CFLAGS) $< $(BUILD)/lib/libhl-linux-abi.a -pthread -o $@

$(BUILD)/tests/test_reloc: tests/unit/test_reloc.c src/translator/reloc.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -Itests/unit $(ENGINE_CFLAGS) $^ -o $@

$(BUILD)/tests/test_digest: tests/unit/test_digest.c src/translator/digest.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -Itests/unit $(ENGINE_CFLAGS) $^ -o $@

$(BUILD)/tests/test_window: tests/unit/test_window.c src/translator/window.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -Itests/unit $(ENGINE_CFLAGS) $^ -o $@

$(BUILD)/tests/test_identity: tests/unit/test_identity.c src/translator/identity.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -Itests/unit $(ENGINE_CFLAGS) $^ -o $@

$(BUILD)/tests/test_clock: tests/unit/test_clock.c src/host/clock.c src/host/fake/host.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -Itests/unit $(ENGINE_CFLAGS) $^ -o $@

$(BUILD)/tests/test_file: tests/unit/test_file.c src/host/file.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -Itests/unit $(ENGINE_CFLAGS) $^ -o $@

$(BUILD)/fixtures/%: tests/compat/fixtures/%.c
	@mkdir -p $(@D)
	$(CC) -O2 -g -std=gnu11 -Wall -Wextra $< -pthread -o $@

$(BUILD)/e2e/guest-exit-aarch64: tests/e2e/guest_exit.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_CC) -nostdlib -static -fno-stack-protector -Wl,-e,_start $< -o $@

$(BUILD)/e2e/guest-exit-x86_64: tests/e2e/guest_exit.c
	@mkdir -p $(@D)
	$(X86_64_LINUX_CC) -nostdlib -static -fno-stack-protector -Wl,-e,_start $< -o $@

$(BUILD)/e2e/guest-spin-aarch64: tests/e2e/guest_spin.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_CC) -O0 -nostdlib -static -fno-stack-protector -Wl,-e,_start $< -o $@

$(BUILD)/e2e/guest-spin-x86_64: tests/e2e/guest_spin.c
	@mkdir -p $(@D)
	$(X86_64_LINUX_CC) -O0 -nostdlib -static -fno-stack-protector -Wl,-e,_start $< -o $@

$(BUILD)/e2e/clock-injected-aarch64: tests/e2e/clock_injected.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_CC) -O2 -static $< -o $@

$(BUILD)/e2e/clock-injected-x86_64: tests/e2e/clock_injected.c
	@mkdir -p $(@D)
	$(X86_64_LINUX_CC) -O2 -static $< -o $@

$(BUILD)/e2e/dynamic-aarch64: tests/e2e/dynamic_guest.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_CC) -O2 -fPIE -pie -Wl,--dynamic-linker,/lib/ld-linux-aarch64.so.1 \
		-Wl,-rpath,/lib $< -o $@

$(BUILD)/e2e/dynamic-x86_64: tests/e2e/dynamic_guest.c
	@mkdir -p $(@D)
	$(X86_64_LINUX_CC) -O2 -fPIE -pie -Wl,--dynamic-linker,/lib64/ld-linux-x86-64.so.2 \
		-Wl,-rpath,/lib $< -o $@

$(BUILD)/e2e/%-aarch64: tests/compat/fixtures/%.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_CC) -O2 -static -pthread $< -o $@

$(BUILD)/e2e/%-x86_64: tests/compat/fixtures/%.c
	@mkdir -p $(@D)
	$(X86_64_LINUX_CC) -O2 -static -pthread $< -o $@

$(BUILD)/compat/abi/aarch64/%: tests/compat/abi/%.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_CC) -O2 -static $< -lm -o $@

$(BUILD)/compat/abi/x86_64/%: tests/compat/abi/%.c
	@mkdir -p $(@D)
	$(X86_64_LINUX_CC) -O2 -static $< -lm -o $@

$(BUILD)/compat/libc/aarch64/%: tests/compat/libc/%.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_CC) -O2 -static -std=gnu11 $< -lm -o $@

$(BUILD)/compat/libc/x86_64/%: tests/compat/libc/%.c
	@mkdir -p $(@D)
	$(X86_64_LINUX_CC) -O2 -static -std=gnu11 $< -lm -o $@

$(BUILD)/compat/completeness/aarch64/%: tests/compat/completeness/%.c tests/compat/completeness/compat.h
	@mkdir -p $(@D)
	$(AARCH64_LINUX_CC) -O2 -static -std=gnu11 -Itests/compat/completeness $< -o $@

$(BUILD)/compat/completeness/x86_64/%: tests/compat/completeness/%.c tests/compat/completeness/compat.h
	@mkdir -p $(@D)
	$(X86_64_LINUX_CC) -O2 -static -std=gnu11 -Itests/compat/completeness $< -o $@

$(BUILD)/compat/posix/aarch64/%: tests/compat/posix/%.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_CC) -O2 -static -std=gnu11 $< -pthread -lutil -o $@

$(BUILD)/compat/posix/x86_64/%: tests/compat/posix/%.c
	@mkdir -p $(@D)
	$(X86_64_LINUX_CC) -O2 -static -std=gnu11 $< -pthread -lutil -o $@

$(BUILD)/compat/syscall/aarch64/%: tests/compat/syscall/%.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_CC) -O2 -static -std=gnu11 $< -pthread -o $@

$(BUILD)/compat/syscall/x86_64/%: tests/compat/syscall/%.c
	@mkdir -p $(@D)
	$(X86_64_LINUX_CC) -O2 -static -std=gnu11 $< -pthread -o $@

$(BUILD)/compat/network/aarch64/%: tests/compat/network/%.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_CC) -O2 -static -std=gnu11 $< -pthread -o $@

$(BUILD)/compat/network/x86_64/%: tests/compat/network/%.c
	@mkdir -p $(@D)
	$(X86_64_LINUX_CC) -O2 -static -std=gnu11 $< -pthread -o $@

$(BUILD)/compat/procfs/aarch64/%: tests/compat/procfs/%.c tests/compat/procfs/pf.h
	@mkdir -p $(@D)
	$(AARCH64_LINUX_CC) -O2 -static -std=gnu11 -Itests/compat/procfs $< -pthread -o $@

$(BUILD)/compat/procfs/x86_64/%: tests/compat/procfs/%.c tests/compat/procfs/pf.h
	@mkdir -p $(@D)
	$(X86_64_LINUX_CC) -O2 -static -std=gnu11 -Itests/compat/procfs $< -pthread -o $@

$(BUILD)/compat/memory/aarch64/%: tests/compat/memory/%.c tests/compat/memory/memrss.h
	@mkdir -p $(@D)
	$(AARCH64_LINUX_CC) -O2 -static -std=gnu11 -Itests/compat/memory $< -pthread -o $@

$(BUILD)/compat/memory/x86_64/%: tests/compat/memory/%.c tests/compat/memory/memrss.h
	@mkdir -p $(@D)
	$(X86_64_LINUX_CC) -O2 -static -std=gnu11 -Itests/compat/memory $< -pthread -o $@

$(BUILD)/compat/signals/aarch64/%: tests/compat/signals/%.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_CC) -O2 -static-pie -std=gnu11 $< -pthread -o $@

$(BUILD)/compat/signals/x86_64/%: tests/compat/signals/%.c
	@mkdir -p $(@D)
	$(X86_64_LINUX_CC) -O2 -static-pie -std=gnu11 $< -pthread -o $@

$(BUILD)/compat/filesystem/aarch64/dentry/%: tests/compat/filesystem/dentry/%.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_CC) -O2 -static-pie -std=gnu11 $< -pthread -o $@

$(BUILD)/compat/filesystem/x86_64/dentry/%: tests/compat/filesystem/dentry/%.c
	@mkdir -p $(@D)
	$(X86_64_LINUX_CC) -O2 -static-pie -std=gnu11 $< -pthread -o $@

$(BUILD)/compat/filesystem/aarch64/pcachex/%: tests/compat/filesystem/pcachex/%.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_CC) -O2 -static-pie -std=gnu11 $< -pthread -o $@

$(BUILD)/compat/filesystem/x86_64/pcachex/%: tests/compat/filesystem/pcachex/%.c
	@mkdir -p $(@D)
	$(X86_64_LINUX_CC) -O2 -static-pie -std=gnu11 $< -pthread -o $@

$(BUILD)/compat/filesystem/aarch64/%: tests/compat/filesystem/%.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_CC) -O2 -static -std=gnu11 $< -pthread -lrt -o $@

$(BUILD)/compat/filesystem/x86_64/%: tests/compat/filesystem/%.c
	@mkdir -p $(@D)
	$(X86_64_LINUX_CC) -O2 -static -std=gnu11 $< -pthread -lrt -o $@

$(BUILD)/compat/process/aarch64/%: tests/compat/process/%.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_CC) -O2 -static -std=gnu11 $< -pthread -o $@

$(BUILD)/compat/process/x86_64/%: tests/compat/process/%.c
	@mkdir -p $(@D)
	$(X86_64_LINUX_CC) -O2 -static -std=gnu11 $< -pthread -o $@

$(BUILD)/compat/process/aarch64/procexe/%: tests/compat/process/procexe/%.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_CC) -O2 -static-pie -std=gnu11 $< -pthread -o $@

$(BUILD)/compat/process/x86_64/procexe/%: tests/compat/process/procexe/%.c
	@mkdir -p $(@D)
	$(X86_64_LINUX_CC) -O2 -static-pie -std=gnu11 $< -pthread -o $@

$(BUILD)/compat/process/aarch64/nonpie_ptrargs: tests/compat/process/nonpie_ptrargs.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_CC) -O2 -static -no-pie -std=gnu11 $< -pthread -o $@

$(BUILD)/compat/process/x86_64/nonpie_ptrargs: tests/compat/process/nonpie_ptrargs.c
	@mkdir -p $(@D)
	$(X86_64_LINUX_CC) -O2 -static -no-pie -std=gnu11 $< -pthread -o $@

$(BUILD)/compat/time/aarch64/%: tests/compat/time/%.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_CC) -O2 -static -std=gnu11 $< -pthread -lrt -o $@

$(BUILD)/compat/time/x86_64/%: tests/compat/time/%.c
	@mkdir -p $(@D)
	$(X86_64_LINUX_CC) -O2 -static -std=gnu11 $< -pthread -lrt -o $@

$(BUILD)/compat/isa/x86_64/%: tests/compat/isa/x86_64/%
	@mkdir -p $(@D)
	cp -p $< $@

$(BUILD)/compat/core/abi/aarch64/%: tests/compat/core/abi/%.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_CC) -O2 -static-pie -pthread $< -lm -o $@

$(BUILD)/compat/core/abi/x86_64/%: tests/compat/core/abi/%.c
	@mkdir -p $(@D)
	$(X86_64_LINUX_CC) -O2 -static-pie -pthread $< -lm -o $@

$(BUILD)/compat/core/workload/aarch64/%: tests/compat/core/workload/%.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_CC) -O2 -static-pie -pthread $< -lm -o $@

$(BUILD)/compat/core/workload/x86_64/%: tests/compat/core/workload/%.c
	@mkdir -p $(@D)
	$(X86_64_LINUX_CC) -O2 -static-pie -pthread $< -lm -o $@

$(BUILD)/compat/core/workload/aarch64/dbserver $(BUILD)/compat/core/workload/aarch64/sqlite: \
	$(BUILD)/compat/core/workload/aarch64/%: tests/compat/core/workload/%.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_CC) -O2 -static-pie -pthread $< -lsqlite3 -lm -ldl -o $@

$(BUILD)/compat/core/workload/aarch64/ibtc_dispatch: tests/compat/core/abi/ibtc_dispatch.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_CC) -O2 -static-pie -pthread $< -lm -o $@

$(BUILD)/compat/core/workload/x86_64/ibtc_dispatch: tests/compat/core/abi/ibtc_dispatch.c
	@mkdir -p $(@D)
	$(X86_64_LINUX_CC) -O2 -static-pie -pthread $< -lm -o $@

$(BUILD)/compat/core/syscall/aarch64/%: tests/compat/core/syscall/%.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_CC) -O2 -static-pie -pthread $< -lm -o $@

$(BUILD)/compat/core/syscall/x86_64/%: tests/compat/core/syscall/%.c
	@mkdir -p $(@D)
	$(X86_64_LINUX_CC) -O2 -static-pie -pthread $< -lm -o $@

$(BUILD)/compat/core/regress/aarch64/%: tests/compat/core/regress/%.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_CC) -O2 -static-pie -pthread $< -lm -o $@

$(BUILD)/compat/core/regress/x86_64/%: tests/compat/core/regress/%.c
	@mkdir -p $(@D)
	$(X86_64_LINUX_CC) -O2 -static-pie -pthread $< -lm -o $@

$(BUILD)/compat/core/regress/aarch64/nonpie_ldapr $(BUILD)/compat/core/regress/aarch64/nonpie_pairatomics: \
	$(BUILD)/compat/core/regress/aarch64/%: tests/compat/core/regress/%.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_CC) -O2 -static -no-pie -pthread $< -lm -o $@

$(BUILD)/compat/core/regress/x86_64/nonpie_vec $(BUILD)/compat/core/regress/x86_64/repcmps_nopie \
	$(BUILD)/compat/core/regress/x86_64/nonpie_v8blob: \
	$(BUILD)/compat/core/regress/x86_64/%: tests/compat/core/regress/%.c
	@mkdir -p $(@D)
	$(X86_64_LINUX_CC) -O2 -static -no-pie -pthread $< -lm -o $@

$(BUILD)/compat/core/regress/aarch64/go_cgo_stackgrow_arm: tests/compat/core/regress/go_cgo_stackgrow_arm
	@mkdir -p $(@D)
	cp -p $< $@

$(BUILD)/compat/isolation/aarch64/%: tests/compat/isolation/%.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_CC) -O2 -static-pie -std=gnu11 $< -pthread -o $@

$(BUILD)/compat/isolation/x86_64/%: tests/compat/isolation/%.c
	@mkdir -p $(@D)
	$(X86_64_LINUX_CC) -O2 -static-pie -std=gnu11 $< -pthread -o $@

$(BUILD)/compat/syscall_edges/aarch64/%: tests/compat/syscall_edges/%.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_CC) -O2 -static -std=gnu11 $< -pthread -lrt -o $@

$(BUILD)/compat/syscall_edges/x86_64/%: tests/compat/syscall_edges/%.c
	@mkdir -p $(@D)
	$(X86_64_LINUX_CC) -O2 -static -std=gnu11 $< -pthread -lrt -o $@

$(BUILD)/soak/aarch64/%: tests/soak/%.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_CC) -O2 -static-pie -std=gnu11 $< -pthread -lm -o $@

$(BUILD)/soak/x86_64/%: tests/soak/%.c
	@mkdir -p $(@D)
	$(X86_64_LINUX_CC) -O2 -static-pie -std=gnu11 $< -pthread -lm -o $@

$(BUILD)/e2e/fd-binding-aarch64: tests/e2e/fd_binding.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_CC) -O2 -static $< -o $@

$(BUILD)/e2e/fd-binding-x86_64: tests/e2e/fd_binding.c
	@mkdir -p $(@D)
	$(X86_64_LINUX_CC) -O2 -static $< -o $@

$(BUILD)/e2e/stdio-binding-aarch64: tests/e2e/stdio_binding.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_CC) -O2 -static $< -o $@

$(BUILD)/e2e/stdio-binding-x86_64: tests/e2e/stdio_binding.c
	@mkdir -p $(@D)
	$(X86_64_LINUX_CC) -O2 -static $< -o $@

$(BUILD)/e2e/dir-binding-aarch64: tests/e2e/dir_binding.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_CC) -O2 -static $< -o $@

$(BUILD)/e2e/dir-binding-x86_64: tests/e2e/dir_binding.c
	@mkdir -p $(@D)
	$(X86_64_LINUX_CC) -O2 -static $< -o $@

$(BUILD)/mac/target/aarch64.o: src/core/target/aarch64.c $(PRODUCTION_UNITY_DEPS)
	@mkdir -p $(@D)
	$(MAC) clang $(CPPFLAGS) -O2 $(DEPFLAGS) -c $< -o $@

$(BUILD)/mac/target/x86_64.o: src/core/target/x86_64.c $(PRODUCTION_UNITY_DEPS)
	@mkdir -p $(@D)
	$(MAC) clang $(CPPFLAGS) -O2 $(DEPFLAGS) -c $< -o $@

$(BUILD)/production/hl-engine-linux-aarch64: $(BUILD)/mac/target/aarch64.o $(MAC_LIBS) \
	packaging/macos/jit.entitlements
	@mkdir -p $(@D)
	$(MAC) clang -o $@ $< $(MAC_LIBS)
	$(MAC) codesign -s - --entitlements packaging/macos/jit.entitlements -f $@

$(BUILD)/production/hl-engine-linux-x86_64: $(BUILD)/mac/target/x86_64.o $(MAC_LIBS) \
	packaging/macos/jit.entitlements
	@mkdir -p $(@D)
	$(MAC) clang -o $@ $< $(MAC_LIBS)
	$(MAC) codesign -s - --entitlements packaging/macos/jit.entitlements -f $@

compat-engines: $(BUILD)/production/hl-engine-linux-aarch64 $(BUILD)/production/hl-engine-linux-x86_64

$(BUILD)/mac/lifecycle/aarch64-target.o: src/core/target/aarch64.c $(PRODUCTION_UNITY_DEPS)
	@mkdir -p $(@D)
	$(MAC) clang $(CPPFLAGS) -DHL_ENGINE_NO_MAIN=1 -O2 $(DEPFLAGS) -c $< -o $@

$(BUILD)/mac/lifecycle/x86_64-target.o: src/core/target/x86_64.c $(PRODUCTION_UNITY_DEPS)
	@mkdir -p $(@D)
	$(MAC) clang $(CPPFLAGS) -DHL_ENGINE_NO_MAIN=1 -O2 $(DEPFLAGS) -c $< -o $@

$(BUILD)/mac/lifecycle/aarch64-core.o: src/core/lifecycle.c
	@mkdir -p $(@D)
	$(MAC) clang $(CPPFLAGS) -DHL_PRODUCTION_GUEST_ISA=HL_GUEST_ISA_AARCH64 -O2 $(DEPFLAGS) -c $< -o $@

$(BUILD)/mac/lifecycle/x86_64-core.o: src/core/lifecycle.c
	@mkdir -p $(@D)
	$(MAC) clang $(CPPFLAGS) -DHL_PRODUCTION_GUEST_ISA=HL_GUEST_ISA_X86_64 -O2 $(DEPFLAGS) -c $< -o $@

$(BUILD)/mac/lifecycle/aarch64-runner.o: tools/lifecycle_e2e_runner.c
	@mkdir -p $(@D)
	$(MAC) clang $(CPPFLAGS) -DHL_TEST_GUEST_ISA=HL_GUEST_ISA_AARCH64 -O2 $(DEPFLAGS) -c $< -o $@

$(BUILD)/mac/lifecycle/x86_64-runner.o: tools/lifecycle_e2e_runner.c
	@mkdir -p $(@D)
	$(MAC) clang $(CPPFLAGS) -DHL_TEST_GUEST_ISA=HL_GUEST_ISA_X86_64 -O2 $(DEPFLAGS) -c $< -o $@

$(BUILD)/mac/binding/aarch64-runner.o: tools/binding_e2e_runner.c
	@mkdir -p $(@D)
	$(MAC) clang $(CPPFLAGS) -DHL_TEST_GUEST_ISA=HL_GUEST_ISA_AARCH64 -O2 $(DEPFLAGS) -c $< -o $@

$(BUILD)/mac/binding/x86_64-runner.o: tools/binding_e2e_runner.c
	@mkdir -p $(@D)
	$(MAC) clang $(CPPFLAGS) -DHL_TEST_GUEST_ISA=HL_GUEST_ISA_X86_64 -O2 $(DEPFLAGS) -c $< -o $@

$(BUILD)/mac/stdio/aarch64-runner.o: tools/stdio_e2e_runner.c
	@mkdir -p $(@D)
	$(MAC) clang $(CPPFLAGS) -DHL_TEST_GUEST_ISA=HL_GUEST_ISA_AARCH64 -O2 $(DEPFLAGS) -c $< -o $@

$(BUILD)/mac/stdio/x86_64-runner.o: tools/stdio_e2e_runner.c
	@mkdir -p $(@D)
	$(MAC) clang $(CPPFLAGS) -DHL_TEST_GUEST_ISA=HL_GUEST_ISA_X86_64 -O2 $(DEPFLAGS) -c $< -o $@

$(BUILD)/mac/dir/aarch64-runner.o: tools/dir_e2e_runner.c
	@mkdir -p $(@D)
	$(MAC) clang $(CPPFLAGS) -DHL_TEST_GUEST_ISA=HL_GUEST_ISA_AARCH64 -O2 $(DEPFLAGS) -c $< -o $@

$(BUILD)/mac/dir/x86_64-runner.o: tools/dir_e2e_runner.c
	@mkdir -p $(@D)
	$(MAC) clang $(CPPFLAGS) -DHL_TEST_GUEST_ISA=HL_GUEST_ISA_X86_64 -O2 $(DEPFLAGS) -c $< -o $@

$(BUILD)/tools/lifecycle-aarch64: $(BUILD)/mac/lifecycle/aarch64-runner.o \
	$(BUILD)/mac/lifecycle/aarch64-target.o $(BUILD)/mac/lifecycle/aarch64-core.o $(MAC_LIBS) \
	packaging/macos/jit.entitlements
	@mkdir -p $(@D)
	$(MAC) clang -o $@ $(filter %.o %.a,$^)
	$(MAC) codesign -s - --entitlements packaging/macos/jit.entitlements -f $@

$(BUILD)/tools/lifecycle-x86_64: $(BUILD)/mac/lifecycle/x86_64-runner.o \
	$(BUILD)/mac/lifecycle/x86_64-target.o $(BUILD)/mac/lifecycle/x86_64-core.o $(MAC_LIBS) \
	packaging/macos/jit.entitlements
	@mkdir -p $(@D)
	$(MAC) clang -o $@ $(filter %.o %.a,$^)
	$(MAC) codesign -s - --entitlements packaging/macos/jit.entitlements -f $@

$(BUILD)/tools/binding-aarch64: $(BUILD)/mac/binding/aarch64-runner.o \
	$(BUILD)/mac/lifecycle/aarch64-target.o $(BUILD)/mac/lifecycle/aarch64-core.o $(MAC_LIBS) \
	packaging/macos/jit.entitlements
	@mkdir -p $(@D)
	$(MAC) clang -o $@ $(filter %.o %.a,$^)
	$(MAC) codesign -s - --entitlements packaging/macos/jit.entitlements -f $@

$(BUILD)/tools/binding-x86_64: $(BUILD)/mac/binding/x86_64-runner.o \
	$(BUILD)/mac/lifecycle/x86_64-target.o $(BUILD)/mac/lifecycle/x86_64-core.o $(MAC_LIBS) \
	packaging/macos/jit.entitlements
	@mkdir -p $(@D)
	$(MAC) clang -o $@ $(filter %.o %.a,$^)
	$(MAC) codesign -s - --entitlements packaging/macos/jit.entitlements -f $@

$(BUILD)/tools/dir-aarch64: $(BUILD)/mac/dir/aarch64-runner.o \
	$(BUILD)/mac/lifecycle/aarch64-target.o $(BUILD)/mac/lifecycle/aarch64-core.o $(MAC_LIBS) \
	packaging/macos/jit.entitlements
	@mkdir -p $(@D)
	$(MAC) clang -o $@ $(filter %.o %.a,$^)
	$(MAC) codesign -s - --entitlements packaging/macos/jit.entitlements -f $@

$(BUILD)/tools/dir-x86_64: $(BUILD)/mac/dir/x86_64-runner.o \
	$(BUILD)/mac/lifecycle/x86_64-target.o $(BUILD)/mac/lifecycle/x86_64-core.o $(MAC_LIBS) \
	packaging/macos/jit.entitlements
	@mkdir -p $(@D)
	$(MAC) clang -o $@ $(filter %.o %.a,$^)
	$(MAC) codesign -s - --entitlements packaging/macos/jit.entitlements -f $@

$(BUILD)/tools/stdio-aarch64: $(BUILD)/mac/stdio/aarch64-runner.o \
	$(BUILD)/mac/lifecycle/aarch64-target.o $(BUILD)/mac/lifecycle/aarch64-core.o $(MAC_LIBS) \
	packaging/macos/jit.entitlements
	@mkdir -p $(@D)
	$(MAC) clang -o $@ $(filter %.o %.a,$^)
	$(MAC) codesign -s - --entitlements packaging/macos/jit.entitlements -f $@

$(BUILD)/tools/stdio-x86_64: $(BUILD)/mac/stdio/x86_64-runner.o \
	$(BUILD)/mac/lifecycle/x86_64-target.o $(BUILD)/mac/lifecycle/x86_64-core.o $(MAC_LIBS) \
	packaging/macos/jit.entitlements
	@mkdir -p $(@D)
	$(MAC) clang -o $@ $(filter %.o %.a,$^)
	$(MAC) codesign -s - --entitlements packaging/macos/jit.entitlements -f $@

e2e-compat: test-macos compat-engines compat-abi compat-core compat-filesystem compat-isa-x86-64 compat-isolation compat-libc compat-completeness compat-memory compat-network compat-posix compat-process compat-procfs compat-signals compat-soak compat-syscall compat-syscall-edges compat-time $(BUILD)/tools/lifecycle-aarch64 $(BUILD)/tools/lifecycle-x86_64 \
	$(BUILD)/tools/binding-aarch64 $(BUILD)/tools/binding-x86_64 \
	$(BUILD)/e2e/fd-binding-aarch64 $(BUILD)/e2e/fd-binding-x86_64 \
	$(BUILD)/tools/stdio-aarch64 $(BUILD)/tools/stdio-x86_64 \
	$(BUILD)/e2e/stdio-binding-aarch64 $(BUILD)/e2e/stdio-binding-x86_64 \
	$(BUILD)/tools/dir-aarch64 $(BUILD)/tools/dir-x86_64 \
	$(BUILD)/e2e/dir-binding-aarch64 $(BUILD)/e2e/dir-binding-x86_64 \
	$(BUILD)/e2e/guest-exit-aarch64 $(BUILD)/e2e/guest-exit-x86_64 \
	$(BUILD)/e2e/guest-spin-aarch64 $(BUILD)/e2e/guest-spin-x86_64 \
	$(BUILD)/e2e/clock-injected-aarch64 $(BUILD)/e2e/clock-injected-x86_64 \
	$(BUILD)/tools/e2e-runner $(BUILD)/tools/config-e2e-runner $(E2E_CASE_RUNS)
	$(BUILD)/tools/e2e-runner $(MAC) $(abspath $(BUILD)/production/hl-engine-linux-aarch64) \
		$(abspath $(BUILD)/e2e/guest-exit-aarch64) 42
	$(BUILD)/tools/e2e-runner $(MAC) $(abspath $(BUILD)/production/hl-engine-linux-x86_64) \
		$(abspath $(BUILD)/e2e/guest-exit-x86_64) 42
	$(BUILD)/tools/config-e2e-runner $(MAC) $(abspath $(BUILD)/production/hl-engine-linux-aarch64) \
		$(abspath $(BUILD)/e2e/guest-exit-aarch64) 42
	$(BUILD)/tools/config-e2e-runner $(MAC) $(abspath $(BUILD)/production/hl-engine-linux-x86_64) \
		$(abspath $(BUILD)/e2e/guest-exit-x86_64) 42
	$(BUILD)/tools/e2e-runner $(MAC) $(abspath $(BUILD)/tools/lifecycle-aarch64) \
		$(abspath $(BUILD)/e2e/guest-exit-aarch64) 42
	$(BUILD)/tools/e2e-runner $(MAC) $(abspath $(BUILD)/tools/lifecycle-x86_64) \
		$(abspath $(BUILD)/e2e/guest-exit-x86_64) 42
	$(MAC) $(abspath $(BUILD)/tools/lifecycle-aarch64) --clock-spy \
		$(abspath $(BUILD)/e2e/clock-injected-aarch64)
	$(MAC) $(abspath $(BUILD)/tools/lifecycle-x86_64) --clock-spy \
		$(abspath $(BUILD)/e2e/clock-injected-x86_64)
	$(MAC) $(abspath $(BUILD)/tools/lifecycle-aarch64) --force-stop \
		$(abspath $(BUILD)/e2e/guest-spin-aarch64)
	$(MAC) $(abspath $(BUILD)/tools/lifecycle-x86_64) --force-stop \
		$(abspath $(BUILD)/e2e/guest-spin-x86_64)
	$(MAC) $(abspath $(BUILD)/tools/binding-aarch64) $(abspath $(BUILD)/e2e/fd-binding-aarch64)
	$(MAC) $(abspath $(BUILD)/tools/binding-x86_64) $(abspath $(BUILD)/e2e/fd-binding-x86_64)
	$(MAC) $(abspath $(BUILD)/tools/stdio-aarch64) $(abspath $(BUILD)/e2e/stdio-binding-aarch64)
	$(MAC) $(abspath $(BUILD)/tools/stdio-x86_64) $(abspath $(BUILD)/e2e/stdio-binding-x86_64)
	$(MAC) $(abspath $(BUILD)/tools/dir-aarch64) $(abspath $(BUILD)/e2e/dir-binding-aarch64)
	$(MAC) $(abspath $(BUILD)/tools/dir-x86_64) $(abspath $(BUILD)/e2e/dir-binding-x86_64)

define HL_E2E_CASE_RULE
run-e2e-compat-$(1): $(BUILD)/e2e/$(1)-aarch64 $(BUILD)/e2e/$(1)-x86_64 $(BUILD)/tools/e2e-runner \
	$(BUILD)/fixtures/$(1) compat-engines
	$(BUILD)/tools/e2e-runner $(MAC) $(abspath $(BUILD)/production/hl-engine-linux-aarch64) \
		$(abspath $(BUILD)/e2e/$(1)-aarch64) 0 $(abspath $(BUILD)/fixtures/$(1))
	$(BUILD)/tools/e2e-runner $(MAC) $(abspath $(BUILD)/production/hl-engine-linux-x86_64) \
		$(abspath $(BUILD)/e2e/$(1)-x86_64) 0 $(abspath $(BUILD)/fixtures/$(1))
endef

$(foreach test,$(E2E_CASES),$(eval $(call HL_E2E_CASE_RULE,$(test))))

$(BUILD)/tools/check-domains: tools/check_domains.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(WARNINGS) $< -o $@

$(BUILD)/tools/compat-runner: tools/compat_runner.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(WARNINGS) $< -o $@

$(BUILD)/tools/e2e-runner: tools/e2e_runner.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(WARNINGS) $< -o $@

$(BUILD)/tools/matrix-runner: tools/matrix_runner.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $< -o $@

compat-abi: compat-engines $(BUILD)/tools/matrix-runner $(ABI_CASE_BINS)
	$(BUILD)/tools/matrix-runner $(MAC) $(abspath $(BUILD)/production/hl-engine-linux-aarch64) \
		$(abspath $(BUILD)/compat/abi/aarch64) $(abspath $(BUILD)/production/hl-engine-linux-x86_64) \
		$(abspath $(BUILD)/compat/abi/x86_64) $(abspath tests/compat/abi)

compat-libc: compat-engines $(BUILD)/tools/matrix-runner $(LIBC_CASE_BINS)
	$(BUILD)/tools/matrix-runner $(MAC) $(abspath $(BUILD)/production/hl-engine-linux-aarch64) \
		$(abspath $(BUILD)/compat/libc/aarch64) $(abspath $(BUILD)/production/hl-engine-linux-x86_64) \
		$(abspath $(BUILD)/compat/libc/x86_64) $(abspath tests/compat/libc)

compat-completeness: compat-engines $(BUILD)/tools/matrix-runner $(COMPLETENESS_BINS)
	$(BUILD)/tools/matrix-runner $(MAC) $(abspath $(BUILD)/production/hl-engine-linux-aarch64) \
		$(abspath $(BUILD)/compat/completeness/aarch64) $(abspath $(BUILD)/production/hl-engine-linux-x86_64) \
		$(abspath $(BUILD)/compat/completeness/x86_64) $(abspath tests/compat/completeness)

compat-posix: compat-engines $(BUILD)/tools/matrix-runner $(POSIX_CASE_BINS)
	$(BUILD)/tools/matrix-runner $(MAC) $(abspath $(BUILD)/production/hl-engine-linux-aarch64) \
		$(abspath $(BUILD)/compat/posix/aarch64) $(abspath $(BUILD)/production/hl-engine-linux-x86_64) \
		$(abspath $(BUILD)/compat/posix/x86_64) $(abspath tests/compat/posix)

compat-syscall: compat-engines $(BUILD)/tools/matrix-runner $(SYSCALL_CASE_BINS)
	$(BUILD)/tools/matrix-runner $(MAC) $(abspath $(BUILD)/production/hl-engine-linux-aarch64) \
		$(abspath $(BUILD)/compat/syscall/aarch64) $(abspath $(BUILD)/production/hl-engine-linux-x86_64) \
		$(abspath $(BUILD)/compat/syscall/x86_64) $(abspath tests/compat/syscall)

compat-network: compat-engines $(BUILD)/tools/matrix-runner $(NETWORK_CASE_BINS)
	$(BUILD)/tools/matrix-runner $(MAC) $(abspath $(BUILD)/production/hl-engine-linux-aarch64) \
		$(abspath $(BUILD)/compat/network/aarch64) $(abspath $(BUILD)/production/hl-engine-linux-x86_64) \
		$(abspath $(BUILD)/compat/network/x86_64) $(abspath tests/compat/network)

compat-memory: compat-engines $(BUILD)/tools/matrix-runner $(MEMORY_CASE_BINS)
	$(BUILD)/tools/matrix-runner $(MAC) $(abspath $(BUILD)/production/hl-engine-linux-aarch64) \
		$(abspath $(BUILD)/compat/memory/aarch64) $(abspath $(BUILD)/production/hl-engine-linux-x86_64) \
		$(abspath $(BUILD)/compat/memory/x86_64) $(abspath tests/compat/memory)

compat-filesystem: compat-engines $(BUILD)/tools/matrix-runner $(FILESYSTEM_CASE_BINS)
	$(BUILD)/tools/matrix-runner $(MAC) $(abspath $(BUILD)/production/hl-engine-linux-aarch64) \
		$(abspath $(BUILD)/compat/filesystem/aarch64) $(abspath $(BUILD)/production/hl-engine-linux-x86_64) \
		$(abspath $(BUILD)/compat/filesystem/x86_64) $(abspath tests/compat/filesystem)

compat-signals: compat-engines $(BUILD)/tools/matrix-runner $(SIGNALS_CASE_BINS)
	$(BUILD)/tools/matrix-runner $(MAC) $(abspath $(BUILD)/production/hl-engine-linux-aarch64) \
		$(abspath $(BUILD)/compat/signals/aarch64) $(abspath $(BUILD)/production/hl-engine-linux-x86_64) \
		$(abspath $(BUILD)/compat/signals/x86_64) $(abspath tests/compat/signals)

compat-process: compat-engines $(BUILD)/tools/matrix-runner $(PROCESS_CASE_BINS)
	$(BUILD)/tools/matrix-runner $(MAC) $(abspath $(BUILD)/production/hl-engine-linux-aarch64) \
		$(abspath $(BUILD)/compat/process/aarch64) $(abspath $(BUILD)/production/hl-engine-linux-x86_64) \
		$(abspath $(BUILD)/compat/process/x86_64) $(abspath tests/compat/process)

compat-time: compat-engines $(BUILD)/tools/matrix-runner $(TIME_CASE_BINS)
	$(BUILD)/tools/matrix-runner $(MAC) $(abspath $(BUILD)/production/hl-engine-linux-aarch64) \
		$(abspath $(BUILD)/compat/time/aarch64) $(abspath $(BUILD)/production/hl-engine-linux-x86_64) \
		$(abspath $(BUILD)/compat/time/x86_64) $(abspath tests/compat/time)

compat-isa-x86-64: compat-engines $(BUILD)/tools/matrix-runner $(ISA_X86_64_BINS)
	$(BUILD)/tools/matrix-runner $(MAC) $(abspath $(BUILD)/production/hl-engine-linux-aarch64) \
		$(abspath $(BUILD)/compat/isa/aarch64) $(abspath $(BUILD)/production/hl-engine-linux-x86_64) \
		$(abspath $(BUILD)/compat/isa/x86_64) $(abspath tests/compat/isa/x86_64)

compat-core: compat-core-abi compat-core-syscall compat-core-regress compat-core-workload

compat-core-abi: compat-engines $(BUILD)/tools/matrix-runner $(CORE_ABI_BINS)
	$(BUILD)/tools/matrix-runner $(MAC) $(abspath $(BUILD)/production/hl-engine-linux-aarch64) \
		$(abspath $(BUILD)/compat/core/abi/aarch64) $(abspath $(BUILD)/production/hl-engine-linux-x86_64) \
		$(abspath $(BUILD)/compat/core/abi/x86_64) $(abspath tests/compat/core/abi)

compat-core-workload: compat-engines $(BUILD)/tools/matrix-runner $(CORE_WORKLOAD_BINS)
	$(BUILD)/tools/matrix-runner $(MAC) $(abspath $(BUILD)/production/hl-engine-linux-aarch64) \
		$(abspath $(BUILD)/compat/core/workload/aarch64) $(abspath $(BUILD)/production/hl-engine-linux-x86_64) \
		$(abspath $(BUILD)/compat/core/workload/x86_64) $(abspath tests/compat/core/workload)

compat-core-syscall: compat-engines $(BUILD)/tools/matrix-runner $(CORE_SYSCALL_BINS)
	$(BUILD)/tools/matrix-runner $(MAC) $(abspath $(BUILD)/production/hl-engine-linux-aarch64) \
		$(abspath $(BUILD)/compat/core/syscall/aarch64) $(abspath $(BUILD)/production/hl-engine-linux-x86_64) \
		$(abspath $(BUILD)/compat/core/syscall/x86_64) $(abspath tests/compat/core/syscall)

compat-core-regress: compat-engines $(BUILD)/tools/matrix-runner $(CORE_REGRESS_BINS)
	$(BUILD)/tools/matrix-runner $(MAC) $(abspath $(BUILD)/production/hl-engine-linux-aarch64) \
		$(abspath $(BUILD)/compat/core/regress/aarch64) $(abspath $(BUILD)/production/hl-engine-linux-x86_64) \
		$(abspath $(BUILD)/compat/core/regress/x86_64) $(abspath tests/compat/core/regress)

compat-isolation: compat-engines $(BUILD)/tools/matrix-runner $(ISOLATION_CASE_BINS)
	$(BUILD)/tools/matrix-runner $(MAC) $(abspath $(BUILD)/production/hl-engine-linux-aarch64) \
		$(abspath $(BUILD)/compat/isolation/aarch64) $(abspath $(BUILD)/production/hl-engine-linux-x86_64) \
		$(abspath $(BUILD)/compat/isolation/x86_64) $(abspath tests/compat/isolation)

compat-syscall-edges: compat-engines $(BUILD)/tools/matrix-runner $(SYSCALL_EDGE_CASE_BINS)
	$(BUILD)/tools/matrix-runner $(MAC) $(abspath $(BUILD)/production/hl-engine-linux-aarch64) \
		$(abspath $(BUILD)/compat/syscall_edges/aarch64) $(abspath $(BUILD)/production/hl-engine-linux-x86_64) \
		$(abspath $(BUILD)/compat/syscall_edges/x86_64) $(abspath tests/compat/syscall_edges)

compat-soak: compat-engines $(BUILD)/tools/matrix-runner $(SOAK_CASE_BINS)
	$(BUILD)/tools/matrix-runner $(MAC) $(abspath $(BUILD)/production/hl-engine-linux-aarch64) \
		$(abspath $(BUILD)/soak/aarch64) $(abspath $(BUILD)/production/hl-engine-linux-x86_64) \
		$(abspath $(BUILD)/soak/x86_64) $(abspath tests/soak)

compat-procfs: compat-engines $(BUILD)/tools/matrix-runner $(PROCFS_CASE_BINS)
	$(BUILD)/tools/matrix-runner $(MAC) $(abspath $(BUILD)/production/hl-engine-linux-aarch64) \
		$(abspath $(BUILD)/compat/procfs/aarch64) $(abspath $(BUILD)/production/hl-engine-linux-x86_64) \
		$(abspath $(BUILD)/compat/procfs/x86_64) $(abspath tests/compat/procfs)

$(BUILD)/tools/config-e2e-runner: tools/config_e2e_runner.c include/hl/config.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $< -o $@

$(BUILD)/tools/rootfs-e2e-runner: tools/rootfs_e2e_runner.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(WARNINGS) $< -o $@

dynamic-e2e: compat-engines $(BUILD)/tools/rootfs-e2e-runner $(BUILD)/e2e/dynamic-aarch64 \
	$(BUILD)/e2e/dynamic-x86_64
	$(BUILD)/tools/rootfs-e2e-runner $(MAC) $(abspath $(BUILD)/production/hl-engine-linux-aarch64) \
		$(abspath $(BUILD)/rootfs/aarch64) $(abspath $(BUILD)/e2e/dynamic-aarch64) \
		$(AARCH64_DYNAMIC_LOADER) $(AARCH64_DYNAMIC_LIBC) /lib/ld-linux-aarch64.so.1 \
		"dynamic-ok ctor=17 tls=23 file=rootfs-data path=1 arg=probe"
	$(BUILD)/tools/rootfs-e2e-runner $(MAC) $(abspath $(BUILD)/production/hl-engine-linux-x86_64) \
		$(abspath $(BUILD)/rootfs/x86_64) $(abspath $(BUILD)/e2e/dynamic-x86_64) \
		$(X86_64_DYNAMIC_LOADER) $(X86_64_DYNAMIC_LIBC) /lib64/ld-linux-x86-64.so.2 \
		"dynamic-ok ctor=17 tls=23 file=rootfs-data path=1 arg=probe"

$(BUILD)/tools/perf-runner: tools/perf_runner.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(WARNINGS) $< -o $@

perf-compat: e2e-compat $(BUILD)/tools/perf-runner
	$(BUILD)/tools/perf-runner $(MAC) $(abspath $(BUILD)/production/hl-engine-linux-aarch64) \
		$(abspath $(BUILD)/e2e/atomics-aarch64) 0 25
	$(BUILD)/tools/perf-runner $(MAC) $(abspath $(BUILD)/production/hl-engine-linux-x86_64) \
		$(abspath $(BUILD)/e2e/atomics-x86_64) 0 25

unit: $(UNIT_RUN_TARGETS) $(LINUX_HOST_TEST)

define HL_UNIT_RULE
run-unit-$(1): $(BUILD)/tests/test_$(1)
	$$<
endef

$(foreach test,$(UNIT_NAMES),$(eval $(call HL_UNIT_RULE,$(test))))

run-unit-linux: $(BUILD)/tests/linux
	$<

$(BUILD)/tests/linux: tests/unit/linux.c $(BUILD)/lib/libhl-engine.a $(BUILD)/lib/libhl-host-linux.a
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -Itests/unit $(ENGINE_CFLAGS) $< $(BUILD)/lib/libhl-engine.a \
		$(BUILD)/lib/libhl-host-linux.a -pthread -o $@

$(BUILD)/tests/macos: tests/unit/macos.c src/host/macos/host.c src/host/sync.c src/core/host_services.c \
	src/core/log.c src/host/clock.c src/host/file.c include/hl/macos.h include/hl/host_services.h
	@mkdir -p $(@D)
	$(MAC) clang -Iinclude -Itests/unit $(ENGINE_CFLAGS) tests/unit/macos.c \
		src/host/macos/host.c src/host/sync.c src/core/host_services.c src/core/log.c src/host/clock.c \
		src/host/file.c -o $@

test-macos: $(BUILD)/tests/macos
	$(MAC) $(abspath $<)

$(BUILD)/tests/test-log-debug: tests/unit/test_log.c src/core/log.c
	@mkdir -p $(@D)
	$(CC) $(filter-out -DHL_ENABLE_LOGGING=%,$(CPPFLAGS)) -DHL_ENABLE_LOGGING=1 -Itests/unit $(ENGINE_CFLAGS) $^ -o $@

test-debug-log: $(BUILD)/tests/test-log-debug
	$<

compat-build: $(FIXTURE_BINS)

compat-native: $(NATIVE_SMOKE_BINS) $(BUILD)/tools/compat-runner
	$(BUILD)/tools/compat-runner $(NATIVE_SMOKE_BINS)

check-domains: $(BUILD)/tools/check-domains
	$(BUILD)/tools/check-domains $(PORTABLE_SOURCES) include/hl/*.h

test: unit check-domains compat-native

format:
	$(CLANG_FORMAT) -i $(PORTABLE_SOURCES) $(LINUX_HOST_SOURCES) $(MACOS_HOST_SOURCES) $(PRIVATE_HEADERS) src/runner/main.c include/hl/*.h tests/unit/*.c tests/unit/*.h tools/*.c

format-check:
	$(CLANG_FORMAT) --dry-run --Werror $(PORTABLE_SOURCES) $(LINUX_HOST_SOURCES) $(MACOS_HOST_SOURCES) $(PRIVATE_HEADERS) src/runner/main.c include/hl/*.h tests/unit/*.c tests/unit/*.h tools/*.c

clean:
	rm -rf $(BUILD)

help:
	@echo 'make all           build pure-C static libraries and runner'
	@echo 'make test          unit, domain-boundary, and native compatibility smoke tests'
	@echo 'make compat-build  compile every Linux behavior fixture'
	@echo 'make e2e-compat    build/codesign production engines and execute both guest ISAs'
	@echo 'make perf-compat   report repeated end-to-end baseline distributions in C'
	@echo 'make format-check  enforce the repository clang-format policy'

# Compiler-generated prerequisites keep standalone objects synchronized with public and private headers.
# Missing .d files are expected on a clean tree; -MP leaves harmless stubs for headers removed later.
-include $(DEPENDENCY_FILES)
