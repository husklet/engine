CC ?= cc
AR ?= ar
INSTALL ?= install
CLANG_FORMAT ?= clang-format
BUILD ?= build
HOST ?= $(if $(filter Darwin,$(shell uname -s)),macos,linux)
HOST_ARCH ?= $(shell uname -m)
PREFIX ?= /usr/local
DESTDIR ?=
VERSION := 0.1.6
DEBUG ?= 0
MAC ?= $(if $(filter Darwin,$(shell uname -s)),env,mac)
CODESIGN ?= codesign
PERF_WARMUPS ?= 3
PERF_SAMPLES ?= 25
PERF_HEAVY_SAMPLES ?= 7
PERF_OP_SAMPLES ?= 7
PERF_LIMIT_startup := 15000 10000
PERF_LIMIT_compute := 750000 650000
PERF_LIMIT_syscall-startup := 30000 25000
PERF_LIMIT_syscall-1m := 500000 400000
PERF_LIMIT_fork-stress := 9000000 8000000
PERF_LIMIT_mmap := 150000 120000
PERF_LIMIT_file := 75000 60000
PERF_LIMIT_pipe := 250000 200000
PERF_LIMIT_event := 250000 200000
PERF_LIMIT_ipc-latency := 150000 120000
PERF_LIMIT_ipc-throughput := 75000 60000
PERF_LIMIT_translation := 40000 30000
PERF_LIMIT_warm-cache := 100000 80000
PERF_MAC_LIMIT_startup := 30000 25000
PERF_MAC_LIMIT_compute := 750000 650000
PERF_MAC_LIMIT_syscall-startup := 40000 30000
PERF_MAC_LIMIT_syscall-1m := 100000 80000
PERF_MAC_LIMIT_fork-stress := 8000000 7000000
PERF_MAC_LIMIT_mmap := 150000 120000
PERF_MAC_LIMIT_file := 100000 80000
PERF_MAC_LIMIT_pipe := 300000 250000
PERF_MAC_LIMIT_event := 350000 300000
PERF_MAC_LIMIT_ipc-latency := 200000 150000
PERF_MAC_LIMIT_ipc-throughput := 100000 80000
PERF_MAC_LIMIT_translation := 50000 40000
PERF_MAC_LIMIT_warm-cache := 750000 650000
SANITIZE_BUILD ?= build/sanitize
PERF_MAC_OS = $(shell $(MAC) uname -s)
PERF_MAC_RELEASE = $(shell $(MAC) uname -r)
PERF_MAC_ARCH = $(shell $(MAC) uname -m)
ifeq ($(HOST),linux)
ifneq ($(filter aarch64 arm64,$(HOST_ARCH)),)
AARCH64_LINUX_CC ?= $(CC)
AARCH64_LINUX_AR ?= $(AR)
else
AARCH64_LINUX_CC ?= aarch64-linux-gnu-gcc
AARCH64_LINUX_AR ?= aarch64-linux-gnu-ar
endif
else
AARCH64_LINUX_CC ?= aarch64-linux-gnu-gcc
AARCH64_LINUX_AR ?= aarch64-linux-gnu-ar
endif
X86_64_LINUX_CC ?= x86_64-linux-gnu-gcc
AARCH64_LINUX_STATIC_CC ?= $(AARCH64_LINUX_CC)
X86_64_LINUX_STATIC_CC ?= $(X86_64_LINUX_CC)
AARCH64_DYNAMIC_LOADER ?= /usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1
AARCH64_DYNAMIC_LIBC ?= /usr/lib/aarch64-linux-gnu/libc.so.6
X86_64_DYNAMIC_LOADER ?= /usr/x86_64-linux-gnu/lib/ld-linux-x86-64.so.2
X86_64_DYNAMIC_LIBC ?= /usr/x86_64-linux-gnu/lib/libc.so.6

CPPFLAGS := -Iinclude -DHL_ENABLE_LOGGING=$(DEBUG)
CFLAGS ?= -O2 -g
WARNINGS := -std=c11 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wstrict-prototypes -Wmissing-prototypes
ENGINE_CFLAGS := $(CFLAGS) $(WARNINGS) -fvisibility=hidden
DEPFLAGS := -MMD -MP
PRIVATE_HEADERS := src/core/cli.h src/core/target/native.h src/core/target/services.h src/host/sync.h src/linux_abi/encode.h src/linux_abi/seccomp_vm.h src/translator/guest/x86_64/decoder.h \
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

CORE_SOURCES := src/core/bus.c src/core/cli.c src/core/config.c src/core/engine.c src/core/fatal.c src/core/host_services.c src/core/launch.c src/core/log.c \
	src/core/options.c src/core/target/bus.c src/core/target/native.c src/core/target/run.c src/core/target/services.c
IR_SOURCES := src/translator/arena.c src/translator/codegen.c src/translator/digest.c src/translator/identity.c src/translator/persist.c src/translator/reloc.c \
	src/translator/window.c src/translator/guest/x86_64/decode.c src/translator/guest/x86_64/address.c src/translator/host/aarch64/codegen.c \
	src/translator/guest/x86_64/glue.c src/translator/guest/x86_64/avx.c \
	src/translator/host/aarch64/asm.c \
	src/translator/guest/x86_64/cpuid.c src/translator/guest/x86_64/cmpxchg.c \
	src/translator/guest/x86_64/legacy.c \
	src/translator/guest/x86_64/lower/alu.c \
	src/translator/guest/x86_64/lower/crypto.c \
	src/translator/guest/x86_64/lower/mov.c \
	src/translator/guest/x86_64/lower/repstr.c \
	src/translator/guest/x86_64/lower/shift.c \
	src/translator/guest/x86_64/lower/sse4x.c \
	src/translator/guest/x86_64/lower/trace.c \
	src/translator/guest/x86_64/lower/x87.c \
	src/translator/guest/x86_64/lower/x87_stack.c \
	src/translator/guest/x86_64/rep.c \
	src/translator/guest/x86_64/rotate.c \
	src/translator/guest/x86_64/x87math.c \
	src/translator/guest/x86_64/x87state.c \
	src/translator/guest/aarch64/signal.c \
	src/translator/guest/x86_64/signal.c \
	src/translator/guest/x86_64/operand.c \
	src/translator/guest/x86_64/flags.c \
	src/translator/host/x86_64/codegen.c src/translator/ir/interpreter.c \
	src/translator/ir/ir.c
LINUX_ABI_SOURCES := src/linux_abi/affinity.c src/linux_abi/container/key.c src/linux_abi/container/pidmap.c src/linux_abi/container/ports.c src/linux_abi/container/snapshot.c src/linux_abi/container/vfs/gmap.c src/linux_abi/container/shm.c src/linux_abi/device.c src/linux_abi/image.c \
	src/linux_abi/fdcache.c \
	src/linux_abi/epoll.c src/linux_abi/eventfd.c src/linux_abi/fork_codec.c src/linux_abi/inotify.c src/linux_abi/pipe.c src/linux_abi/placement.c src/linux_abi/errno.c src/linux_abi/limits.c src/linux_abi/linux_abi.c src/linux_abi/number.c \
	src/linux_abi/open_plan.c src/linux_abi/parse.c src/linux_abi/readonly.c src/linux_abi/seccomp_vm.c src/linux_abi/shared.c src/linux_abi/stat.c src/linux_abi/watch.c src/linux_abi/xattr.c \
	src/linux_abi/syscall/misc.c
FAKE_HOST_SOURCES := src/host/fake/host.c
MACOS_HOST_SOURCES := src/host/macos/directory.c src/host/macos/host.c src/host/macos/process.c src/host/macos/range.c \
	src/host/macos/system.c
COMMON_HOST_SOURCES := src/host/child.c src/host/fork_wire.c src/host/private.c src/host/range.c src/host/resolve.c src/host/sync.c
MAC_LINUX_ABI_SOURCES := $(LINUX_ABI_SOURCES)
MAC_HOST_SOURCES := $(MACOS_HOST_SOURCES) $(COMMON_HOST_SOURCES) src/host/clock.c src/host/file.c
MAC_CORE_OBJECTS := $(CORE_SOURCES:%.c=$(BUILD)/mac/%.o)
MAC_TRANSLATOR_OBJECTS := $(IR_SOURCES:%.c=$(BUILD)/mac/%.o)
MAC_LINUX_ABI_OBJECTS := $(MAC_LINUX_ABI_SOURCES:%.c=$(BUILD)/mac/%.o)
MAC_HOST_OBJECTS := $(MAC_HOST_SOURCES:%.c=$(BUILD)/mac/%.o)
EMBEDDED_MAC_SOURCES := $(CORE_SOURCES) $(IR_SOURCES) $(MAC_LINUX_ABI_SOURCES) $(MAC_HOST_SOURCES)
EMBEDDED_MAC_OBJECTS := $(EMBEDDED_MAC_SOURCES:%.c=$(BUILD)/mac/embedded/%.o)
LINUX_AARCH64_EMBEDDED_SOURCES = $(CORE_SOURCES) $(IR_SOURCES) $(LINUX_ABI_SOURCES) $(LINUX_HOST_SOURCES)
LINUX_AARCH64_EMBEDDED_OBJECTS = $(LINUX_AARCH64_EMBEDDED_SOURCES:%.c=$(BUILD)/linux-aarch64/embedded/%.o)
MAC_LIBS := $(BUILD)/mac/lib/libhl-engine.a $(BUILD)/mac/lib/libhl-translator.a \
	$(BUILD)/mac/lib/libhl-linux-abi.a $(BUILD)/mac/lib/libhl-host-macos.a
PORTABLE_SOURCES := $(CORE_SOURCES) $(IR_SOURCES) $(LINUX_ABI_SOURCES) $(FAKE_HOST_SOURCES) $(COMMON_HOST_SOURCES)
CORE_OBJECTS := $(CORE_SOURCES:%.c=$(BUILD)/%.o)
TRANSLATOR_OBJECTS := $(IR_SOURCES:%.c=$(BUILD)/%.o)
LINUX_ABI_OBJECTS := $(LINUX_ABI_SOURCES:%.c=$(BUILD)/%.o)
FAKE_HOST_OBJECTS := $(FAKE_HOST_SOURCES:%.c=$(BUILD)/%.o)

LINUX_HOST_SOURCES := src/host/linux/directory.c src/host/linux/host.c src/host/linux/process.c src/host/linux/range.c \
	src/host/linux/system.c src/host/clock.c src/host/file.c \
	$(COMMON_HOST_SOURCES)
LINUX_HOST_OBJECTS := $(LINUX_HOST_SOURCES:%.c=$(BUILD)/%.o)

ifeq ($(HOST),linux)
LINUX_HOST_PRODUCTS := $(BUILD)/lib/libhl-host-linux.a
LINUX_HOST_TEST := run-unit-linux
PACKAGE_HOST := linux
PACKAGE_HOST_LIBRARY := $(BUILD)/lib/libhl-host-linux.a
PACKAGE_HOST_OBJECTS := $(LINUX_HOST_OBJECTS)
PACKAGE_SYSTEM_LIBS := -pthread
else ifeq ($(HOST),macos)
PACKAGE_HOST := macos
PACKAGE_HOST_LIBRARY := $(BUILD)/lib/libhl-host-macos.a
PACKAGE_HOST_OBJECTS := $(MACOS_HOST_SOURCES:%.c=$(BUILD)/%.o) $(COMMON_HOST_SOURCES:%.c=$(BUILD)/%.o) \
	$(BUILD)/src/host/clock.o $(BUILD)/src/host/file.o
PACKAGE_SYSTEM_LIBS := -pthread
else
$(error HOST must be linux or macos)
endif

PACKAGE_LIBRARIES := $(BUILD)/lib/libhl-engine.a $(BUILD)/lib/libhl-translator.a \
	$(BUILD)/lib/libhl-linux-abi.a $(PACKAGE_HOST_LIBRARY)
PACKAGE_PC := $(BUILD)/pkgconfig/hl-engine.pc
PUBLIC_HEADERS := $(wildcard include/hl/*.h)

ifneq ($(filter arm64 aarch64,$(HOST_ARCH)),)
ifeq ($(HOST),macos)
ACTIVATION_ARCHIVE := $(BUILD)/package/macos-aarch64/libhl-engine.a
ACTIVATION_LIBS := -Wl,-force_load,$${libdir}/libhl-engine-activation.a
ACTIVATION_CONSUMER_LIBS := -Wl,-force_load,$(abspath $(BUILD)/package-root)/usr/lib/libhl-engine-activation.a
else
ACTIVATION_ARCHIVE := $(BUILD)/package/linux-aarch64/libhl-engine.a
ACTIVATION_LIBS := -Wl,--whole-archive $${libdir}/libhl-engine-activation.a -Wl,--no-whole-archive -pthread -ldl -lm -latomic
ACTIVATION_CONSUMER_LIBS := -Wl,--whole-archive $(abspath $(BUILD)/package-root)/usr/lib/libhl-engine-activation.a \
	-Wl,--no-whole-archive -pthread -ldl -lm -latomic
endif
ACTIVATION_PC := $(BUILD)/pkgconfig/hl-engine-activation.pc
INSTALL_HEADERS := $(PUBLIC_HEADERS)
else
ACTIVATION_ARCHIVE :=
ACTIVATION_PC :=
INSTALL_HEADERS := $(filter-out include/hl/activation.h,$(PUBLIC_HEADERS))
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

UNIT_NAMES := a64_asm address affinity arena avx bus child cli clock codegen config cpuid cmpxchg decoder device digest directory directory_services emit epoll eventfd eventfd_fork fatal fdcache file flags fork_wire glue gmap host_services identity image inotify ir key launch legacy linux_abi linux_fork lower_alu lower_crypto lower_mov lower_repstr lower_shift lower_sse4x lower_trace lower_x87 misc native open_plan operand persist pidmap pipe pipe_linux placement ports private process range rep resolve resolve_services rotate shared shm signal_aarch64 signal_x86_64 snapshot system seccomp_vm stat engine errno limits log namespace number options parse profile readonly reloc target_bus watch window x87_stack x87math x87state xattr_cache

$(BUILD)/tests/test_x87math: tests/unit/test_x87math.c $(BUILD)/lib/libhl-engine.a $(BUILD)/lib/libhl-translator.a \
	$(BUILD)/lib/libhl-linux-abi.a $(BUILD)/lib/libhl-host-fake.a
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -Itests/unit $(ENGINE_CFLAGS) $< $(BUILD)/lib/libhl-engine.a \
		$(BUILD)/lib/libhl-translator.a $(BUILD)/lib/libhl-linux-abi.a $(BUILD)/lib/libhl-host-fake.a -lm -o $@
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
ifeq ($(HOST),linux)
E2E_NATIVE_ORACLE_RUNS := $(E2E_CASE_RUNS)
else
E2E_NATIVE_ORACLE_RUNS :=
endif
ABI_CASE_SOURCES := $(sort $(wildcard tests/compat/abi/*.c))
ABI_CASE_NAMES := $(basename $(notdir $(ABI_CASE_SOURCES)))
ABI_CASE_AARCH64_NAMES := $(filter-out cpuid_features rflags_id,$(ABI_CASE_NAMES))
ABI_CASE_X86_64_NAMES := $(filter-out lse_atomics,$(ABI_CASE_NAMES))
ABI_CASE_BINS := $(ABI_CASE_AARCH64_NAMES:%=$(BUILD)/compat/abi/aarch64/%) \
	$(ABI_CASE_X86_64_NAMES:%=$(BUILD)/compat/abi/x86_64/%)
ABI_CORPUS_SOURCES := $(sort $(wildcard tests/compat/abi/corpus/*.c))
ABI_CORPUS_NAMES := $(basename $(notdir $(ABI_CORPUS_SOURCES)))
ABI_CORPUS_BINS := $(ABI_CORPUS_NAMES:%=$(BUILD)/compat/abi-corpus/aarch64/%) \
	$(ABI_CORPUS_NAMES:%=$(BUILD)/compat/abi-corpus/x86_64/%)
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
	$(addprefix $(BUILD)/compat/signals/x86_64/,$(filter-out sigurg_preempt,$(SIGNALS_CASE_NAMES)))
PROCESS_CASE_SOURCES := $(filter-out tests/compat/process/integration/%.c,$(sort $(wildcard tests/compat/process/*.c tests/compat/process/*/*.c)))
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
IPC_CASE_SOURCES := $(sort $(wildcard tests/compat/ipc/*.c))
IPC_CASE_NAMES := $(basename $(notdir $(IPC_CASE_SOURCES)))
IPC_CASE_BINS := $(addprefix $(BUILD)/compat/ipc/aarch64/,$(filter-out ipc_tso_unaligned,$(IPC_CASE_NAMES))) \
	$(addprefix $(BUILD)/compat/ipc/x86_64/,$(filter-out ipc_mq_notify neonshm,$(IPC_CASE_NAMES)))
THREAD_CASE_SOURCES := $(sort $(wildcard tests/compat/threads/*.c))
THREAD_CASE_NAMES := $(basename $(notdir $(THREAD_CASE_SOURCES)))
THREAD_CASE_BINS := $(THREAD_CASE_NAMES:%=$(BUILD)/compat/threads/aarch64/%) \
	$(THREAD_CASE_NAMES:%=$(BUILD)/compat/threads/x86_64/%) \
	$(BUILD)/compat/threads/aarch64/threads_mutex_nopie \
	$(BUILD)/compat/threads/x86_64/threads_mutex_nopie
PURPOSE_ABI_PIE := atomics_builtins cpuid_features rflags_id tls tlsmodels
PURPOSE_FILESYSTEM_PIE := dup2redir fcntlflags ltp_aterr ltp_dupfcntl ltp_linkstat mkfifo scratch_exec sentry_fs
PURPOSE_PROCESS_PIE := execfault forkserver_probe forkstorm forkwait ltp_checkpoint ltp_procmisc \
	pipeproc procreap sentry_fork sysinfo thrfork waitcore
PURPOSE_NETWORK_PIE := ltp_neterr net_nonblock net_sendmsg net_sockopt net_tcp net_udp net_unix sentry_net
PURPOSE_IPC_PIE := msg neonshm sem shm shmposix sysvshm
PURPOSE_THREADS_PIE := threads_basic threads_many threads_mutex_queue
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

.PHONY: all linux-compile clean install uninstall package-test FORCE test sanitize unit $(UNIT_RUN_TARGETS) test-debug-log test-macos compat-build compat-native compat-engines dynamic-e2e e2e-compat \
	compat-abi compat-abi-corpus compat-core compat-core-abi compat-core-regress compat-core-syscall compat-core-workload compat-filesystem compat-ipc compat-isa-x86-64 compat-isolation compat-libc compat-completeness compat-memory compat-network compat-posix compat-process compat-procfs compat-signals compat-soak compat-syscall compat-syscall-edges compat-threads compat-time $(E2E_CASE_RUNS) perf-compat perf-macos perf-native-aarch64 format format-check help

all: $(BUILD)/lib/libhl-engine.a $(BUILD)/lib/libhl-translator.a $(BUILD)/lib/libhl-linux-abi.a \
	$(BUILD)/lib/libhl-host-fake.a $(LINUX_HOST_PRODUCTS) $(BUILD)/bin/hl-engine-runner

# Linux-host compile/link gate for the independently compiled libraries, Linux provider, and runner.
# Full production runtime behavior is covered separately by test-linux-production-typed.
linux-compile: $(BUILD)/lib/libhl-engine.a $(BUILD)/lib/libhl-translator.a $(BUILD)/lib/libhl-linux-abi.a \
	$(BUILD)/lib/libhl-host-fake.a $(BUILD)/lib/libhl-host-linux.a $(BUILD)/bin/hl-engine-runner \
	$(BUILD)/tests/linux
	@echo 'linux-compile: portable libraries, Linux host archive, runner, and host test linked'

$(BUILD)/src/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(ENGINE_CFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD)/mac/%.o: %.c
	@mkdir -p $(@D)
	$(MAC) clang $(CPPFLAGS) $(ENGINE_CFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD)/mac/embedded/%.o: %.c
	@mkdir -p $(@D)
	$(MAC) clang $(CPPFLAGS) $(ENGINE_CFLAGS) -DHL_EMBEDDED_BUILD=1 $(DEPFLAGS) -c $< -o $@

$(BUILD)/linux-aarch64/embedded/%.o: %.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_CC) $(CPPFLAGS) $(ENGINE_CFLAGS) -D_GNU_SOURCE -DHL_EMBEDDED_BUILD=1 $(DEPFLAGS) -c $< -o $@

$(BUILD)/mac/lib/libhl-engine.a: $(MAC_CORE_OBJECTS)
	@mkdir -p $(@D)
	$(MAC) ar rcs $@ $^

$(BUILD)/mac/lib/libhl-translator.a: $(MAC_TRANSLATOR_OBJECTS)
	@mkdir -p $(@D)
	$(MAC) ar rcs $@ $^

$(BUILD)/mac/lib/libhl-linux-abi.a: $(MAC_LINUX_ABI_OBJECTS)
	@mkdir -p $(@D)
	rm -f $@
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
	rm -f $@
	$(AR) rcs $@ $^

$(BUILD)/lib/libhl-host-fake.a: $(FAKE_HOST_OBJECTS)
	@mkdir -p $(@D)
	$(AR) rcs $@ $^

$(BUILD)/lib/libhl-host-linux.a: $(LINUX_HOST_OBJECTS)
	@mkdir -p $(@D)
	$(AR) rcs $@ $^

$(BUILD)/lib/libhl-host-macos.a: $(PACKAGE_HOST_OBJECTS)
	@mkdir -p $(@D)
	$(AR) rcs $@ $^

$(PACKAGE_PC): Makefile FORCE
	@mkdir -p $(@D)
	@{ \
		printf '%s\n' 'prefix=$(PREFIX)'; \
		printf '%s\n' 'exec_prefix=$${prefix}' 'libdir=$${exec_prefix}/lib' 'includedir=$${prefix}/include'; \
		printf '\nName: hl-engine\nDescription: Portable Linux guest translation and ABI engine\n'; \
		printf '%s\n' 'Version: $(VERSION)' 'Libs: -L$${libdir} -lhl-host-$(PACKAGE_HOST) -lhl-engine -lhl-translator -lhl-linux-abi $(PACKAGE_SYSTEM_LIBS)' 'Cflags: -I$${includedir}'; \
	} > $@

$(BUILD)/pkgconfig/hl-engine-activation.pc: Makefile FORCE
	@mkdir -p $(@D)
	@{ \
		printf '%s\n' 'prefix=$(PREFIX)'; \
		printf '%s\n' 'exec_prefix=$${prefix}' 'libdir=$${exec_prefix}/lib' 'includedir=$${prefix}/include'; \
		printf '\nName: hl-engine-activation\nDescription: Complete embedded Linux activation engine\n'; \
		printf '%s\n' 'Version: $(VERSION)' 'Libs: $(ACTIVATION_LIBS)' 'Cflags: -I$${includedir}'; \
	} > $@

install: $(PACKAGE_LIBRARIES) $(BUILD)/bin/hl-engine-runner $(PACKAGE_PC) $(ACTIVATION_ARCHIVE) $(ACTIVATION_PC)
	$(INSTALL) -d '$(DESTDIR)$(PREFIX)/include/hl' '$(DESTDIR)$(PREFIX)/lib/pkgconfig' \
		'$(DESTDIR)$(PREFIX)/bin'
	$(INSTALL) -m 0644 $(INSTALL_HEADERS) '$(DESTDIR)$(PREFIX)/include/hl/'
	$(INSTALL) -m 0644 $(PACKAGE_LIBRARIES) '$(DESTDIR)$(PREFIX)/lib/'
	$(INSTALL) -m 0644 $(PACKAGE_PC) '$(DESTDIR)$(PREFIX)/lib/pkgconfig/hl-engine.pc'
	$(if $(ACTIVATION_ARCHIVE),$(INSTALL) -m 0644 $(ACTIVATION_ARCHIVE) '$(DESTDIR)$(PREFIX)/lib/libhl-engine-activation.a')
	$(if $(ACTIVATION_PC),$(INSTALL) -m 0644 $(ACTIVATION_PC) '$(DESTDIR)$(PREFIX)/lib/pkgconfig/hl-engine-activation.pc')
	$(INSTALL) -m 0755 $(BUILD)/bin/hl-engine-runner '$(DESTDIR)$(PREFIX)/bin/'

uninstall:
	rm -f '$(DESTDIR)$(PREFIX)/bin/hl-engine-runner' '$(DESTDIR)$(PREFIX)/lib/pkgconfig/hl-engine.pc' \
		'$(DESTDIR)$(PREFIX)/lib/pkgconfig/hl-engine-activation.pc' \
		'$(DESTDIR)$(PREFIX)/lib/libhl-engine.a' '$(DESTDIR)$(PREFIX)/lib/libhl-translator.a' \
		'$(DESTDIR)$(PREFIX)/lib/libhl-linux-abi.a' '$(DESTDIR)$(PREFIX)/lib/libhl-host-$(PACKAGE_HOST).a' \
		'$(DESTDIR)$(PREFIX)/lib/libhl-engine-activation.a'
	rm -f $(foreach header,$(notdir $(wildcard include/hl/*.h)),'$(DESTDIR)$(PREFIX)/include/hl/$(header)')
	@rmdir '$(DESTDIR)$(PREFIX)/include/hl' '$(DESTDIR)$(PREFIX)/include' \
		'$(DESTDIR)$(PREFIX)/lib/pkgconfig' '$(DESTDIR)$(PREFIX)/lib' '$(DESTDIR)$(PREFIX)/bin' \
		'$(DESTDIR)$(PREFIX)' 2>/dev/null || :

package-test:
	rm -rf '$(BUILD)/package-root' '$(BUILD)/package-consumer'
	$(MAKE) HOST='$(HOST)' BUILD='$(BUILD)' PREFIX=/usr DESTDIR='$(abspath $(BUILD)/package-root)' install
	@printf 'not owned by hl-engine\n' > '$(BUILD)/package-root/usr/include/foreign.h'
	@mkdir -p '$(BUILD)/package-consumer'
	$(CC) -I'$(abspath $(BUILD)/package-root)/usr/include' tests/integration/package.c \
		-L'$(abspath $(BUILD)/package-root)/usr/lib' -lhl-host-$(PACKAGE_HOST) -lhl-engine \
		-lhl-translator -lhl-linux-abi $(PACKAGE_SYSTEM_LIBS) \
		-o '$(BUILD)/package-consumer/package'
	'$(BUILD)/package-consumer/package'
	$(if $(ACTIVATION_ARCHIVE),$(MAKE) HOST='$(HOST)' HOST_ARCH='$(HOST_ARCH)' BUILD='$(BUILD)' package-activation-installed-test)
	$(MAKE) HOST='$(HOST)' PREFIX=/usr DESTDIR='$(abspath $(BUILD)/package-root)' uninstall
	test ! -e '$(BUILD)/package-root/usr/include/hl/engine.h'
	test -e '$(BUILD)/package-root/usr/include/foreign.h'
	rm -rf '$(BUILD)/package-root'

.PHONY: package-activation-installed-test
package-activation-installed-test: $(BUILD)/e2e/guest-descendant-aarch64
	$(CC) -I'$(abspath $(BUILD)/package-root)/usr/include' tests/integration/activation_package.c \
		$(ACTIVATION_CONSUMER_LIBS) -o '$(BUILD)/package-consumer/activation-package'
	$(if $(filter macos,$(HOST)),$(CODESIGN) -s - --entitlements packaging/macos/jit.entitlements -f '$(BUILD)/package-consumer/activation-package')
	'$(BUILD)/package-consumer/activation-package'
	'$(abspath $(BUILD)/package-consumer/activation-package)' '$(abspath $(BUILD)/e2e/guest-descendant-aarch64)'

.PHONY: package-activation-macos-test
package-activation-macos-test: HOST = macos
package-activation-macos-test: HOST_ARCH = aarch64
package-activation-macos-test: ACTIVATION_LIBS = -Wl,-force_load,$${libdir}/libhl-engine-activation.a
package-activation-macos-test: $(BUILD)/package/macos-aarch64/libhl-engine.a \
	$(BUILD)/pkgconfig/hl-engine-activation.pc $(BUILD)/e2e/guest-descendant-aarch64
	rm -rf '$(BUILD)/activation-package-root' '$(BUILD)/activation-package-consumer'
	$(INSTALL) -d '$(BUILD)/activation-package-root/include/hl' \
		'$(BUILD)/activation-package-root/lib/pkgconfig' '$(BUILD)/activation-package-consumer'
	$(INSTALL) -m 0644 include/hl/*.h '$(BUILD)/activation-package-root/include/hl/'
	$(INSTALL) -m 0644 $(BUILD)/package/macos-aarch64/libhl-engine.a \
		'$(BUILD)/activation-package-root/lib/libhl-engine-activation.a'
	$(INSTALL) -m 0644 $(BUILD)/pkgconfig/hl-engine-activation.pc \
		'$(BUILD)/activation-package-root/lib/pkgconfig/hl-engine-activation.pc'
	$(MAC) clang -I'$(abspath $(BUILD)/activation-package-root/include)' tests/integration/activation_package.c \
		-Wl,-force_load,'$(abspath $(BUILD)/activation-package-root/lib/libhl-engine-activation.a)' \
		-o '$(BUILD)/activation-package-consumer/activation-package'
	$(MAC) $(CODESIGN) -s - --entitlements packaging/macos/jit.entitlements -f \
		'$(BUILD)/activation-package-consumer/activation-package'
	$(MAC) $(abspath $(BUILD)/activation-package-consumer/activation-package)
	$(MAC) $(abspath $(BUILD)/activation-package-consumer/activation-package) \
		$(abspath $(BUILD)/e2e/guest-descendant-aarch64)

FORCE:

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

$(BUILD)/tests/test_watch: tests/unit/test_watch.c $(BUILD)/lib/libhl-linux-abi.a
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -Itests/unit $(ENGINE_CFLAGS) -pthread $< $(BUILD)/lib/libhl-linux-abi.a -o $@

$(BUILD)/tests/test_native: tests/unit/test_native.c $(BUILD)/lib/libhl-engine.a $(BUILD)/lib/libhl-translator.a $(BUILD)/lib/libhl-host-linux.a
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -Itests/unit $(ENGINE_CFLAGS) $< $(BUILD)/lib/libhl-engine.a $(BUILD)/lib/libhl-translator.a \
		$(BUILD)/lib/libhl-host-linux.a -pthread -o $@

$(BUILD)/tests/test_private: tests/unit/test_private.c $(BUILD)/lib/libhl-host-linux.a
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -Itests/unit $(ENGINE_CFLAGS) $< $(BUILD)/lib/libhl-host-linux.a -o $@

$(BUILD)/tests/native-capacity: tests/unit/test_native_capacity.c $(BUILD)/lib/libhl-engine.a $(BUILD)/lib/libhl-host-linux.a
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -Itests/unit $(ENGINE_CFLAGS) $< $(BUILD)/lib/libhl-engine.a \
		$(BUILD)/lib/libhl-host-linux.a -pthread -o $@

test-native-capacity: $(BUILD)/tests/native-capacity
	$(BUILD)/tests/native-capacity

$(BUILD)/tests/test_directory_services: tests/unit/test_directory_services.c $(BUILD)/lib/libhl-engine.a \
	$(BUILD)/lib/libhl-host-linux.a
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -Itests/unit $(ENGINE_CFLAGS) $< $(BUILD)/lib/libhl-engine.a \
		$(BUILD)/lib/libhl-host-linux.a -pthread -o $@

$(BUILD)/tests/test_fdcache: tests/unit/test_fdcache.c $(BUILD)/lib/libhl-linux-abi.a $(BUILD)/lib/libhl-host-fake.a
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -Isrc/linux_abi -Itests/unit $(ENGINE_CFLAGS) $< $(BUILD)/lib/libhl-linux-abi.a \
		$(BUILD)/lib/libhl-host-fake.a -o $@

$(BUILD)/tests/test_resolve: tests/unit/test_resolve.c src/host/resolve.c src/host/resolve.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -Itests/unit $(ENGINE_CFLAGS) tests/unit/test_resolve.c src/host/resolve.c -o $@

$(BUILD)/tests/test_resolve_services: tests/unit/test_resolve_services.c $(BUILD)/lib/libhl-host-linux.a
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -Itests/unit $(ENGINE_CFLAGS) $< $(BUILD)/lib/libhl-host-linux.a -pthread -o $@

$(BUILD)/tests/test_linux_fork: tests/unit/test_linux_fork.c $(BUILD)/lib/libhl-linux-abi.a \
	$(BUILD)/lib/libhl-host-linux.a
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -Itests/unit $(ENGINE_CFLAGS) $< $(BUILD)/lib/libhl-linux-abi.a \
		$(BUILD)/lib/libhl-host-linux.a -pthread -o $@

$(BUILD)/tests/test_pipe_linux: tests/unit/test_pipe_linux.c $(BUILD)/lib/libhl-linux-abi.a \
	$(BUILD)/lib/libhl-host-linux.a
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -Isrc/linux_abi -Itests/unit $(ENGINE_CFLAGS) $< $(BUILD)/lib/libhl-linux-abi.a \
		$(BUILD)/lib/libhl-host-linux.a -pthread -o $@

$(BUILD)/tests/test_eventfd_fork: tests/unit/test_eventfd_fork.c $(BUILD)/lib/libhl-linux-abi.a \
	$(BUILD)/lib/libhl-host-linux.a
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -Isrc/linux_abi -Itests/unit $(ENGINE_CFLAGS) $< $(BUILD)/lib/libhl-linux-abi.a \
		$(BUILD)/lib/libhl-host-linux.a -pthread -o $@

$(BUILD)/tests/test_seccomp_vm: tests/unit/test_seccomp_vm.c $(BUILD)/lib/libhl-linux-abi.a
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -Isrc/linux_abi -Itests/unit $(ENGINE_CFLAGS) $< $(BUILD)/lib/libhl-linux-abi.a -o $@

$(BUILD)/tests/test_fork_wire: tests/unit/test_fork_wire.c $(BUILD)/lib/libhl-linux-abi.a \
	$(PACKAGE_HOST_LIBRARY)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -Isrc/linux_abi -Isrc/host -Itests/unit $(ENGINE_CFLAGS) $< \
		$(BUILD)/lib/libhl-linux-abi.a $(PACKAGE_HOST_LIBRARY) -pthread -o $@

$(BUILD)/tests/test_limits: tests/unit/test_limits.c $(BUILD)/lib/libhl-linux-abi.a
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -Itests/unit $(ENGINE_CFLAGS) $< $(BUILD)/lib/libhl-linux-abi.a -pthread -o $@

$(BUILD)/tests/test_reloc: tests/unit/test_reloc.c src/translator/reloc.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -Itests/unit $(ENGINE_CFLAGS) $^ -o $@

$(BUILD)/tests/test_digest: tests/unit/test_digest.c src/translator/digest.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -Itests/unit $(ENGINE_CFLAGS) $^ -o $@

$(BUILD)/tests/test_x87_stack: tests/unit/test_x87_stack.c src/translator/guest/x86_64/lower/x87_stack.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -Itests/unit $(ENGINE_CFLAGS) $^ -o $@

$(BUILD)/tests/test_lower_x87: tests/unit/test_lower_x87.c src/translator/guest/x86_64/lower/x87.c src/translator/guest/x86_64/lower/x87_stack.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -Itests/unit $(ENGINE_CFLAGS) $^ -o $@

$(BUILD)/tests/test_lower_sse4x: tests/unit/test_lower_sse4x.c src/translator/guest/x86_64/lower/sse4x.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -Itests/unit $(ENGINE_CFLAGS) $^ -o $@

$(BUILD)/tests/test_lower_repstr: tests/unit/test_lower_repstr.c src/translator/guest/x86_64/lower/repstr.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -Itests/unit $(ENGINE_CFLAGS) $^ -o $@

$(BUILD)/tests/test_lower_crypto: tests/unit/test_lower_crypto.c src/translator/guest/x86_64/lower/crypto.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -Itests/unit $(ENGINE_CFLAGS) $^ -o $@

$(BUILD)/tests/test_lower_trace: tests/unit/test_lower_trace.c src/translator/guest/x86_64/lower/trace.c
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

$(BUILD)/tests/test_child: tests/unit/test_child.c src/host/child.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -Itests/unit $(ENGINE_CFLAGS) $^ -o $@

$(BUILD)/tests/test_directory: tests/unit/test_directory.c src/host/linux/directory.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -Itests/unit $(ENGINE_CFLAGS) $^ -o $@

$(BUILD)/tests/test_process: tests/unit/test_process.c src/host/linux/process.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -Itests/unit $(ENGINE_CFLAGS) $^ -o $@

$(BUILD)/tests/test_file: tests/unit/test_file.c src/host/file.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -Itests/unit $(ENGINE_CFLAGS) $^ -o $@

$(BUILD)/tests/test_range: tests/unit/test_range.c src/host/range.c src/host/linux/range.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -Itests/unit $(ENGINE_CFLAGS) $^ -o $@

$(BUILD)/tests/test_system: tests/unit/test_system.c src/host/linux/system.c src/host/private.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -Itests/unit $(ENGINE_CFLAGS) $^ -o $@

$(BUILD)/fixtures/%: tests/compat/fixtures/%.c
	@mkdir -p $(@D)
	$(CC) -O2 -g -std=gnu11 -Wall -Wextra $< -pthread -o $@

$(BUILD)/e2e/guest-exit-aarch64: tests/e2e/guest_exit.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_STATIC_CC) -nostdlib -static -fno-stack-protector -Wl,-e,_start $< -o $@

$(BUILD)/e2e/guest-exit-x86_64: tests/e2e/guest_exit.c
	@mkdir -p $(@D)
	$(X86_64_LINUX_STATIC_CC) -nostdlib -static -fno-stack-protector -Wl,-e,_start $< -o $@

$(BUILD)/e2e/guest-exit70-aarch64: tests/e2e/guest_exit.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_STATIC_CC) -DHL_GUEST_EXIT_STATUS=70 -nostdlib -static -fno-stack-protector -Wl,-e,_start $< -o $@

$(BUILD)/e2e/guest-exit70-x86_64: tests/e2e/guest_exit.c
	@mkdir -p $(@D)
	$(X86_64_LINUX_STATIC_CC) -DHL_GUEST_EXIT_STATUS=70 -nostdlib -static -fno-stack-protector -Wl,-e,_start $< -o $@

$(BUILD)/e2e/guest-exit139-aarch64: tests/e2e/guest_exit.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_STATIC_CC) -DHL_GUEST_EXIT_STATUS=139 -nostdlib -static -fno-stack-protector -Wl,-e,_start $< -o $@

$(BUILD)/e2e/guest-exit139-x86_64: tests/e2e/guest_exit.c
	@mkdir -p $(@D)
	$(X86_64_LINUX_STATIC_CC) -DHL_GUEST_EXIT_STATUS=139 -nostdlib -static -fno-stack-protector -Wl,-e,_start $< -o $@

$(BUILD)/e2e/guest-fault-aarch64: tests/e2e/guest_fault.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_STATIC_CC) -nostdlib -static -fno-stack-protector -Wl,-e,_start $< -o $@

$(BUILD)/e2e/guest-fault-x86_64: tests/e2e/guest_fault.c
	@mkdir -p $(@D)
	$(X86_64_LINUX_STATIC_CC) -nostdlib -static -fno-stack-protector -Wl,-e,_start $< -o $@

$(BUILD)/e2e/guest-output-aarch64: tests/e2e/guest_output.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_STATIC_CC) -nostdlib -static -fno-stack-protector -Wl,-e,_start $< -o $@

$(BUILD)/e2e/guest-spin-aarch64: tests/e2e/guest_spin.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_STATIC_CC) -O0 -nostdlib -static -fno-stack-protector -Wl,-e,_start $< -o $@

$(BUILD)/e2e/guest-spin-x86_64: tests/e2e/guest_spin.c
	@mkdir -p $(@D)
	$(X86_64_LINUX_STATIC_CC) -O0 -nostdlib -static -fno-stack-protector -Wl,-e,_start $< -o $@

$(BUILD)/e2e/guest-descendant-aarch64: tests/e2e/guest_descendant.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_STATIC_CC) -O2 -static $< -o $@

$(BUILD)/e2e/clock-injected-aarch64: tests/e2e/clock_injected.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_STATIC_CC) -O2 -static $< -o $@

$(BUILD)/e2e/clock-injected-x86_64: tests/e2e/clock_injected.c
	@mkdir -p $(@D)
	$(X86_64_LINUX_STATIC_CC) -O2 -static $< -o $@

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
	$(AARCH64_LINUX_STATIC_CC) -O2 -static -pthread $< -o $@

$(BUILD)/e2e/%-x86_64: tests/compat/fixtures/%.c
	@mkdir -p $(@D)
	$(X86_64_LINUX_STATIC_CC) -O2 -static -pthread $< -o $@

$(BUILD)/compat/abi/aarch64/%: tests/compat/abi/%.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_STATIC_CC) -O2 -static $< -lm -o $@

$(BUILD)/compat/abi/x86_64/%: tests/compat/abi/%.c
	@mkdir -p $(@D)
	$(X86_64_LINUX_STATIC_CC) -O2 -static $< -lm -o $@

$(BUILD)/compat/abi-corpus/aarch64/%: tests/compat/abi/corpus/%.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_STATIC_CC) -O2 -static -std=gnu11 -pthread $< -lm -o $@

$(BUILD)/compat/abi-corpus/x86_64/%: tests/compat/abi/corpus/%.c
	@mkdir -p $(@D)
	$(X86_64_LINUX_STATIC_CC) -O2 -static -std=gnu11 -pthread $< -lm -o $@

$(BUILD)/compat/libc/aarch64/%: tests/compat/libc/%.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_STATIC_CC) -O2 -static -std=gnu11 $< -lm -o $@

$(BUILD)/compat/libc/x86_64/%: tests/compat/libc/%.c
	@mkdir -p $(@D)
	$(X86_64_LINUX_STATIC_CC) -O2 -static -std=gnu11 $< -lm -o $@

$(BUILD)/compat/completeness/aarch64/%: tests/compat/completeness/%.c tests/compat/completeness/compat.h
	@mkdir -p $(@D)
	$(AARCH64_LINUX_STATIC_CC) -O2 -static -std=gnu11 -Itests/compat/completeness $< -o $@

$(BUILD)/compat/completeness/x86_64/%: tests/compat/completeness/%.c tests/compat/completeness/compat.h
	@mkdir -p $(@D)
	$(X86_64_LINUX_STATIC_CC) -O2 -static -std=gnu11 -Itests/compat/completeness $< -o $@

$(BUILD)/compat/posix/aarch64/%: tests/compat/posix/%.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_STATIC_CC) -O2 -static -std=gnu11 $< -pthread -lutil -o $@

$(BUILD)/compat/posix/x86_64/%: tests/compat/posix/%.c
	@mkdir -p $(@D)
	$(X86_64_LINUX_STATIC_CC) -O2 -static -std=gnu11 $< -pthread -lutil -o $@

$(BUILD)/compat/syscall/aarch64/%: tests/compat/syscall/%.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_STATIC_CC) -O2 -static -std=gnu11 $< -pthread -o $@

$(BUILD)/compat/syscall/x86_64/%: tests/compat/syscall/%.c
	@mkdir -p $(@D)
	$(X86_64_LINUX_STATIC_CC) -O2 -static -std=gnu11 $< -pthread -o $@

$(BUILD)/compat/network/aarch64/%: tests/compat/network/%.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_STATIC_CC) -O2 -static -std=gnu11 $< -pthread -o $@

$(BUILD)/compat/network/x86_64/%: tests/compat/network/%.c
	@mkdir -p $(@D)
	$(X86_64_LINUX_STATIC_CC) -O2 -static -std=gnu11 $< -pthread -o $@

$(BUILD)/compat/procfs/aarch64/%: tests/compat/procfs/%.c tests/compat/procfs/pf.h
	@mkdir -p $(@D)
	$(AARCH64_LINUX_STATIC_CC) -O2 -static -std=gnu11 -Itests/compat/procfs $< -pthread -o $@

$(BUILD)/compat/procfs/x86_64/%: tests/compat/procfs/%.c tests/compat/procfs/pf.h
	@mkdir -p $(@D)
	$(X86_64_LINUX_STATIC_CC) -O2 -static -std=gnu11 -Itests/compat/procfs $< -pthread -o $@

$(BUILD)/compat/memory/aarch64/%: tests/compat/memory/%.c tests/compat/memory/memrss.h
	@mkdir -p $(@D)
	$(AARCH64_LINUX_STATIC_CC) -O2 -static -std=gnu11 -Itests/compat/memory $< -pthread -o $@

$(BUILD)/compat/memory/x86_64/%: tests/compat/memory/%.c tests/compat/memory/memrss.h
	@mkdir -p $(@D)
	$(X86_64_LINUX_STATIC_CC) -O2 -static -std=gnu11 -Itests/compat/memory $< -pthread -o $@

$(BUILD)/compat/signals/aarch64/%: tests/compat/signals/%.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_STATIC_CC) -O2 -static-pie -std=gnu11 $< -pthread -o $@

$(BUILD)/compat/signals/x86_64/%: tests/compat/signals/%.c
	@mkdir -p $(@D)
	$(X86_64_LINUX_STATIC_CC) -O2 -static-pie -std=gnu11 $< -pthread -o $@

$(BUILD)/compat/filesystem/aarch64/dentry/%: tests/compat/filesystem/dentry/%.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_STATIC_CC) -O2 -static-pie -std=gnu11 $< -pthread -o $@

$(BUILD)/compat/filesystem/x86_64/dentry/%: tests/compat/filesystem/dentry/%.c
	@mkdir -p $(@D)
	$(X86_64_LINUX_STATIC_CC) -O2 -static-pie -std=gnu11 $< -pthread -o $@

$(BUILD)/compat/filesystem/aarch64/pcachex/%: tests/compat/filesystem/pcachex/%.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_STATIC_CC) -O2 -static-pie -std=gnu11 $< -pthread -o $@

$(BUILD)/compat/filesystem/x86_64/pcachex/%: tests/compat/filesystem/pcachex/%.c
	@mkdir -p $(@D)
	$(X86_64_LINUX_STATIC_CC) -O2 -static-pie -std=gnu11 $< -pthread -o $@

$(BUILD)/compat/filesystem/aarch64/%: tests/compat/filesystem/%.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_STATIC_CC) -O2 -static -std=gnu11 $< -pthread -lrt -o $@

$(BUILD)/compat/filesystem/x86_64/%: tests/compat/filesystem/%.c
	@mkdir -p $(@D)
	$(X86_64_LINUX_STATIC_CC) -O2 -static -std=gnu11 $< -pthread -lrt -o $@

$(BUILD)/compat/process/aarch64/%: tests/compat/process/%.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_STATIC_CC) -O2 -static -std=gnu11 $< -pthread -o $@

$(BUILD)/compat/process/x86_64/%: tests/compat/process/%.c
	@mkdir -p $(@D)
	$(X86_64_LINUX_STATIC_CC) -O2 -static -std=gnu11 $< -pthread -o $@

$(BUILD)/compat/process/aarch64/procexe/%: tests/compat/process/procexe/%.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_STATIC_CC) -O2 -static-pie -std=gnu11 $< -pthread -o $@

$(BUILD)/compat/process/x86_64/procexe/%: tests/compat/process/procexe/%.c
	@mkdir -p $(@D)
	$(X86_64_LINUX_STATIC_CC) -O2 -static-pie -std=gnu11 $< -pthread -o $@

$(BUILD)/compat/process/aarch64/nonpie_ptrargs: tests/compat/process/nonpie_ptrargs.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_STATIC_CC) -O2 -static -fno-pie -no-pie -std=gnu11 $< -pthread -o $@

$(BUILD)/compat/process/x86_64/nonpie_ptrargs: tests/compat/process/nonpie_ptrargs.c
	@mkdir -p $(@D)
	$(X86_64_LINUX_STATIC_CC) -O2 -static -fno-pie -no-pie -std=gnu11 $< -pthread -o $@

$(BUILD)/compat/time/aarch64/%: tests/compat/time/%.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_STATIC_CC) -O2 -static -std=gnu11 $< -pthread -lrt -o $@

$(BUILD)/compat/time/x86_64/%: tests/compat/time/%.c
	@mkdir -p $(@D)
	$(X86_64_LINUX_STATIC_CC) -O2 -static -std=gnu11 $< -pthread -lrt -o $@

$(BUILD)/compat/isa/x86_64/%: tests/compat/isa/x86_64/%
	@mkdir -p $(@D)
	cp -p $< $@

$(BUILD)/compat/core/abi/aarch64/%: tests/compat/core/abi/%.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_STATIC_CC) -O2 -static-pie -pthread $< -lm -o $@

$(BUILD)/compat/core/abi/x86_64/%: tests/compat/core/abi/%.c
	@mkdir -p $(@D)
	$(X86_64_LINUX_STATIC_CC) -O2 -static-pie -pthread $< -lm -o $@

$(BUILD)/compat/core/workload/aarch64/%: tests/compat/core/workload/%.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_STATIC_CC) -O2 -static-pie -pthread $< -lm -o $@

$(BUILD)/compat/core/workload/x86_64/%: tests/compat/core/workload/%.c
	@mkdir -p $(@D)
	$(X86_64_LINUX_STATIC_CC) -O2 -static-pie -pthread $< -lm -o $@

$(BUILD)/compat/core/workload/aarch64/dbserver $(BUILD)/compat/core/workload/aarch64/sqlite: \
	$(BUILD)/compat/core/workload/aarch64/%: tests/compat/core/workload/%.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_STATIC_CC) -O2 -static-pie -pthread $< -lsqlite3 -lm -ldl -o $@

$(BUILD)/compat/core/workload/aarch64/ibtc_dispatch: tests/compat/core/abi/ibtc_dispatch.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_STATIC_CC) -O2 -static-pie -pthread $< -lm -o $@

$(BUILD)/compat/core/workload/x86_64/ibtc_dispatch: tests/compat/core/abi/ibtc_dispatch.c
	@mkdir -p $(@D)
	$(X86_64_LINUX_STATIC_CC) -O2 -static-pie -pthread $< -lm -o $@

$(BUILD)/compat/core/syscall/aarch64/%: tests/compat/core/syscall/%.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_STATIC_CC) -O2 -static-pie -pthread $< -lm -o $@

$(BUILD)/compat/core/syscall/x86_64/%: tests/compat/core/syscall/%.c
	@mkdir -p $(@D)
	$(X86_64_LINUX_STATIC_CC) -O2 -static-pie -pthread $< -lm -o $@

$(BUILD)/compat/core/regress/aarch64/%: tests/compat/core/regress/%.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_STATIC_CC) -O2 -static-pie -pthread $< -lm -o $@

$(BUILD)/compat/core/regress/x86_64/%: tests/compat/core/regress/%.c
	@mkdir -p $(@D)
	$(X86_64_LINUX_STATIC_CC) -O2 -static-pie -pthread $< -lm -o $@

$(BUILD)/compat/core/regress/aarch64/nonpie_ldapr $(BUILD)/compat/core/regress/aarch64/nonpie_pairatomics: \
	$(BUILD)/compat/core/regress/aarch64/%: tests/compat/core/regress/%.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_STATIC_CC) -O2 -static -fno-pie -no-pie -pthread $< -lm -o $@

$(BUILD)/compat/core/regress/x86_64/nonpie_vec $(BUILD)/compat/core/regress/x86_64/repcmps_nopie \
	$(BUILD)/compat/core/regress/x86_64/nonpie_v8blob: \
	$(BUILD)/compat/core/regress/x86_64/%: tests/compat/core/regress/%.c
	@mkdir -p $(@D)
	$(X86_64_LINUX_STATIC_CC) -O2 -static -fno-pie -no-pie -pthread $< -lm -o $@

$(BUILD)/compat/core/regress/aarch64/go_cgo_stackgrow_arm: tests/compat/core/regress/go_cgo_stackgrow_arm
	@mkdir -p $(@D)
	cp -p $< $@

$(BUILD)/compat/ipc/aarch64/%: tests/compat/ipc/%.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_STATIC_CC) -O2 -static -std=gnu11 -pthread $< -lrt -o $@

$(BUILD)/compat/ipc/x86_64/%: tests/compat/ipc/%.c
	@mkdir -p $(@D)
	$(X86_64_LINUX_STATIC_CC) -O2 -static -std=gnu11 -pthread $< -lrt -o $@

$(BUILD)/compat/threads/aarch64/%: tests/compat/threads/%.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_STATIC_CC) -O2 -static -std=gnu11 -pthread $< -lm -o $@

$(BUILD)/compat/threads/x86_64/%: tests/compat/threads/%.c
	@mkdir -p $(@D)
	$(X86_64_LINUX_STATIC_CC) -O2 -static -std=gnu11 -pthread $< -lm -o $@

$(BUILD)/compat/threads/aarch64/threads_mutex_nopie: tests/compat/threads/threads_mutex_queue.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_STATIC_CC) -O2 -static -fno-pie -no-pie -pthread $< -lm -o $@

$(BUILD)/compat/threads/x86_64/threads_mutex_nopie: tests/compat/threads/threads_mutex_queue.c
	@mkdir -p $(@D)
	$(X86_64_LINUX_STATIC_CC) -O2 -static -fno-pie -no-pie -pthread $< -lm -o $@

$(PURPOSE_ABI_PIE:%=$(BUILD)/compat/abi/aarch64/%): $(BUILD)/compat/abi/aarch64/%: tests/compat/abi/%.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_STATIC_CC) -O2 -static-pie -pthread $< -lm -ldl -o $@

$(PURPOSE_ABI_PIE:%=$(BUILD)/compat/abi/x86_64/%): $(BUILD)/compat/abi/x86_64/%: tests/compat/abi/%.c
	@mkdir -p $(@D)
	$(X86_64_LINUX_STATIC_CC) -O2 -static-pie -pthread $< -lm -ldl -o $@

$(PURPOSE_FILESYSTEM_PIE:%=$(BUILD)/compat/filesystem/aarch64/%): $(BUILD)/compat/filesystem/aarch64/%: tests/compat/filesystem/%.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_STATIC_CC) -O2 -static-pie -pthread $< -lm -ldl -o $@

$(PURPOSE_FILESYSTEM_PIE:%=$(BUILD)/compat/filesystem/x86_64/%): $(BUILD)/compat/filesystem/x86_64/%: tests/compat/filesystem/%.c
	@mkdir -p $(@D)
	$(X86_64_LINUX_STATIC_CC) -O2 -static-pie -pthread $< -lm -ldl -o $@

$(PURPOSE_PROCESS_PIE:%=$(BUILD)/compat/process/aarch64/%): $(BUILD)/compat/process/aarch64/%: tests/compat/process/%.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_STATIC_CC) -O2 -static-pie -pthread $< -lm -ldl -o $@

$(PURPOSE_PROCESS_PIE:%=$(BUILD)/compat/process/x86_64/%): $(BUILD)/compat/process/x86_64/%: tests/compat/process/%.c
	@mkdir -p $(@D)
	$(X86_64_LINUX_STATIC_CC) -O2 -static-pie -pthread $< -lm -ldl -o $@

$(BUILD)/compat/process/aarch64/nonpie_dladdr: tests/compat/process/nonpie_dladdr.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_CC) -O2 -no-pie -rdynamic -Wl,--dynamic-linker=/lib/ld-linux-aarch64.so.1,-rpath,/lib $< -o $@

$(BUILD)/compat/process/x86_64/nonpie_dladdr: tests/compat/process/nonpie_dladdr.c
	@mkdir -p $(@D)
	$(X86_64_LINUX_CC) -O2 -no-pie -rdynamic -Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2,-rpath,/lib $< -o $@

$(PURPOSE_NETWORK_PIE:%=$(BUILD)/compat/network/aarch64/%): $(BUILD)/compat/network/aarch64/%: tests/compat/network/%.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_STATIC_CC) -O2 -static-pie -pthread $< -lm -ldl -o $@

$(PURPOSE_NETWORK_PIE:%=$(BUILD)/compat/network/x86_64/%): $(BUILD)/compat/network/x86_64/%: tests/compat/network/%.c
	@mkdir -p $(@D)
	$(X86_64_LINUX_STATIC_CC) -O2 -static-pie -pthread $< -lm -ldl -o $@

$(PURPOSE_IPC_PIE:%=$(BUILD)/compat/ipc/aarch64/%): $(BUILD)/compat/ipc/aarch64/%: tests/compat/ipc/%.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_STATIC_CC) -O2 -static-pie -pthread $< -lm -ldl -o $@

$(PURPOSE_IPC_PIE:%=$(BUILD)/compat/ipc/x86_64/%): $(BUILD)/compat/ipc/x86_64/%: tests/compat/ipc/%.c
	@mkdir -p $(@D)
	$(X86_64_LINUX_STATIC_CC) -O2 -static-pie -pthread $< -lm -ldl -o $@

$(PURPOSE_THREADS_PIE:%=$(BUILD)/compat/threads/aarch64/%): $(BUILD)/compat/threads/aarch64/%: tests/compat/threads/%.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_STATIC_CC) -O2 -static-pie -pthread $< -lm -ldl -o $@

$(PURPOSE_THREADS_PIE:%=$(BUILD)/compat/threads/x86_64/%): $(BUILD)/compat/threads/x86_64/%: tests/compat/threads/%.c
	@mkdir -p $(@D)
	$(X86_64_LINUX_STATIC_CC) -O2 -static-pie -pthread $< -lm -ldl -o $@

$(BUILD)/compat/abi/aarch64/lse_atomics: tests/compat/abi/lse_atomics.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_STATIC_CC) -O2 -static-pie -pthread -march=armv8.2-a+lse -mno-outline-atomics $< -lm -o $@

$(BUILD)/compat/threads/aarch64/threads_nopie_tls: tests/compat/threads/threads_nopie_tls.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_STATIC_CC) -O2 -static -fno-pie -no-pie -pthread $< -lm -o $@

$(BUILD)/compat/threads/x86_64/threads_nopie_tls: tests/compat/threads/threads_nopie_tls.c
	@mkdir -p $(@D)
	$(X86_64_LINUX_STATIC_CC) -O2 -static -fno-pie -no-pie -pthread $< -lm -o $@

$(BUILD)/compat/abi/aarch64/tlsmodels_main: tests/compat/abi/tlsmodels_main.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_STATIC_CC) -O2 -static -fno-pie -no-pie -pthread $< -lm -ldl -o $@

$(BUILD)/compat/abi/x86_64/tlsmodels_main: tests/compat/abi/tlsmodels_main.c
	@mkdir -p $(@D)
	$(X86_64_LINUX_STATIC_CC) -O2 -static -fno-pie -no-pie -pthread $< -lm -ldl -o $@

$(BUILD)/compat/isolation/aarch64/%: tests/compat/isolation/%.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_STATIC_CC) -O2 -static-pie -std=gnu11 $< -pthread -o $@

$(BUILD)/compat/isolation/x86_64/%: tests/compat/isolation/%.c
	@mkdir -p $(@D)
	$(X86_64_LINUX_STATIC_CC) -O2 -static-pie -std=gnu11 $< -pthread -o $@

$(BUILD)/compat/syscall_edges/aarch64/%: tests/compat/syscall_edges/%.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_STATIC_CC) -O2 -static -std=gnu11 $< -pthread -lrt -o $@

$(BUILD)/compat/syscall_edges/x86_64/%: tests/compat/syscall_edges/%.c
	@mkdir -p $(@D)
	$(X86_64_LINUX_STATIC_CC) -O2 -static -std=gnu11 $< -pthread -lrt -o $@

$(BUILD)/soak/aarch64/%: tests/soak/%.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_STATIC_CC) -O2 -static-pie -std=gnu11 $< -pthread -lm -o $@

$(BUILD)/soak/x86_64/%: tests/soak/%.c
	@mkdir -p $(@D)
	$(X86_64_LINUX_STATIC_CC) -O2 -static-pie -std=gnu11 $< -pthread -lm -o $@

$(BUILD)/e2e/fd-binding-aarch64: tests/e2e/fd_binding.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_STATIC_CC) -O2 -static $< -o $@

$(BUILD)/e2e/fd-binding-x86_64: tests/e2e/fd_binding.c
	@mkdir -p $(@D)
	$(X86_64_LINUX_STATIC_CC) -O2 -static $< -o $@

$(BUILD)/e2e/stdio-binding-aarch64: tests/e2e/stdio_binding.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_STATIC_CC) -O2 -static $< -o $@

$(BUILD)/e2e/stdio-binding-x86_64: tests/e2e/stdio_binding.c
	@mkdir -p $(@D)
	$(X86_64_LINUX_STATIC_CC) -O2 -static $< -o $@

$(BUILD)/e2e/dir-binding-aarch64: tests/e2e/dir_binding.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_STATIC_CC) -O2 -static $< -o $@

$(BUILD)/e2e/dir-binding-x86_64: tests/e2e/dir_binding.c
	@mkdir -p $(@D)
	$(X86_64_LINUX_STATIC_CC) -O2 -static $< -o $@

$(BUILD)/mac/target/aarch64.o: src/core/target/aarch64.c $(PRODUCTION_UNITY_DEPS)
	@mkdir -p $(@D)
	$(MAC) clang $(CPPFLAGS) -O2 $(DEPFLAGS) -c $< -o $@

$(BUILD)/mac/target/x86_64.o: src/core/target/x86_64.c $(PRODUCTION_UNITY_DEPS)
	@mkdir -p $(@D)
	$(MAC) clang $(CPPFLAGS) -O2 $(DEPFLAGS) -c $< -o $@

$(BUILD)/production/hl-engine-linux-aarch64: $(BUILD)/mac/target/aarch64.o $(BUILD)/mac/lifecycle/aarch64-core.o $(MAC_LIBS) \
	packaging/macos/jit.entitlements
	@mkdir -p $(@D)
	$(MAC) clang -o $@ $< $(BUILD)/mac/lifecycle/aarch64-core.o $(MAC_LIBS)
	$(MAC) $(CODESIGN) -s - --entitlements packaging/macos/jit.entitlements -f $@

$(BUILD)/production/hl-engine-linux-x86_64: $(BUILD)/mac/target/x86_64.o $(BUILD)/mac/lifecycle/x86_64-core.o $(MAC_LIBS) \
	packaging/macos/jit.entitlements
	@mkdir -p $(@D)
	$(MAC) clang -o $@ $< $(BUILD)/mac/lifecycle/x86_64-core.o $(MAC_LIBS)
	$(MAC) $(CODESIGN) -s - --entitlements packaging/macos/jit.entitlements -f $@

# First native-Linux production lane: AArch64 host executing an x86-64 Linux
# guest through the production JIT. This target is smoke-scoped until the
# remaining event/path personality seams have native Linux implementations.
$(BUILD)/linux-production/target/aarch64.o: src/core/target/aarch64.c $(PRODUCTION_UNITY_DEPS)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -D_GNU_SOURCE -O2 $(DEPFLAGS) -c $< -o $@

$(BUILD)/linux-production/target/x86_64.o: src/core/target/x86_64.c $(PRODUCTION_UNITY_DEPS)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -D_GNU_SOURCE -O2 $(DEPFLAGS) -c $< -o $@

$(BUILD)/linux-production/lifecycle/x86_64-core.o: src/core/lifecycle.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -DHL_PRODUCTION_GUEST_ISA=HL_GUEST_ISA_X86_64 -O2 $(DEPFLAGS) -c $< -o $@

$(BUILD)/linux-production/lifecycle/aarch64-core.o: src/core/lifecycle.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -DHL_PRODUCTION_GUEST_ISA=HL_GUEST_ISA_AARCH64 -O2 $(DEPFLAGS) -c $< -o $@

$(BUILD)/linux-production/lifecycle/aarch64-runner.o: tools/lifecycle_e2e_runner.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -D_GNU_SOURCE -DHL_TEST_HOST_LINUX=1 -DHL_TEST_GUEST_ISA=HL_GUEST_ISA_AARCH64 \
		-O2 $(DEPFLAGS) -c $< -o $@

$(BUILD)/linux-production/lifecycle/x86_64-runner.o: tools/lifecycle_e2e_runner.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -D_GNU_SOURCE -DHL_TEST_HOST_LINUX=1 -DHL_TEST_GUEST_ISA=HL_GUEST_ISA_X86_64 \
		-O2 $(DEPFLAGS) -c $< -o $@

$(BUILD)/linux-production/lifecycle/aarch64-target.o: src/core/target/aarch64.c $(PRODUCTION_UNITY_DEPS)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -D_GNU_SOURCE -DHL_ENGINE_NO_MAIN=1 -O2 $(DEPFLAGS) -c $< -o $@

$(BUILD)/linux-production/lifecycle/x86_64-target.o: src/core/target/x86_64.c $(PRODUCTION_UNITY_DEPS)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -D_GNU_SOURCE -DHL_ENGINE_NO_MAIN=1 -O2 $(DEPFLAGS) -c $< -o $@

$(BUILD)/tools/lifecycle-linux-aarch64: $(BUILD)/linux-production/lifecycle/aarch64-runner.o \
	$(BUILD)/linux-production/lifecycle/aarch64-target.o $(BUILD)/linux-production/lifecycle/aarch64-core.o \
	$(BUILD)/lib/libhl-engine.a $(BUILD)/lib/libhl-translator.a $(BUILD)/lib/libhl-linux-abi.a \
	$(BUILD)/lib/libhl-host-linux.a
	@mkdir -p $(@D)
	$(CC) -o $@ $^ -pthread -lm -ldl -latomic

$(BUILD)/tools/lifecycle-linux-x86_64: $(BUILD)/linux-production/lifecycle/x86_64-runner.o \
	$(BUILD)/linux-production/lifecycle/x86_64-target.o $(BUILD)/linux-production/lifecycle/x86_64-core.o \
	$(BUILD)/lib/libhl-engine.a $(BUILD)/lib/libhl-translator.a $(BUILD)/lib/libhl-linux-abi.a \
	$(BUILD)/lib/libhl-host-linux.a
	@mkdir -p $(@D)
	$(CC) -o $@ $^ -pthread -lm -ldl -latomic

.PHONY: test-linux-lifecycle
test-linux-lifecycle: $(BUILD)/tools/lifecycle-linux-aarch64 $(BUILD)/tools/lifecycle-linux-x86_64 \
	$(BUILD)/e2e/guest-exit-aarch64 $(BUILD)/e2e/guest-exit-x86_64 \
	$(BUILD)/e2e/guest-exit139-aarch64 $(BUILD)/e2e/guest-exit139-x86_64 \
	$(BUILD)/e2e/guest-fault-aarch64 $(BUILD)/e2e/guest-fault-x86_64 \
	$(BUILD)/e2e/clock-injected-aarch64 $(BUILD)/e2e/clock-injected-x86_64 \
	$(BUILD)/e2e/guest-spin-aarch64 $(BUILD)/e2e/guest-spin-x86_64
	$(BUILD)/tools/lifecycle-linux-aarch64 $(abspath $(BUILD)/e2e/guest-exit-aarch64); test $$? -eq 42
	$(BUILD)/tools/lifecycle-linux-x86_64 $(abspath $(BUILD)/e2e/guest-exit-x86_64); test $$? -eq 42
	$(BUILD)/tools/lifecycle-linux-aarch64 --expect-exit 139 $(abspath $(BUILD)/e2e/guest-exit139-aarch64)
	$(BUILD)/tools/lifecycle-linux-x86_64 --expect-exit 139 $(abspath $(BUILD)/e2e/guest-exit139-x86_64)
	$(BUILD)/tools/lifecycle-linux-aarch64 --expect-signal 11 $(abspath $(BUILD)/e2e/guest-fault-aarch64)
	$(BUILD)/tools/lifecycle-linux-x86_64 --expect-signal 11 $(abspath $(BUILD)/e2e/guest-fault-x86_64)
	$(BUILD)/tools/lifecycle-linux-aarch64 --clock-spy $(abspath $(BUILD)/e2e/clock-injected-aarch64)
	$(BUILD)/tools/lifecycle-linux-x86_64 --clock-spy $(abspath $(BUILD)/e2e/clock-injected-x86_64)
	$(BUILD)/tools/lifecycle-linux-aarch64 --force-stop $(abspath $(BUILD)/e2e/guest-spin-aarch64)
	$(BUILD)/tools/lifecycle-linux-x86_64 --force-stop $(abspath $(BUILD)/e2e/guest-spin-x86_64)

$(BUILD)/linux-production/hl-engine-linux-aarch64: $(BUILD)/linux-production/target/aarch64.o \
	$(BUILD)/linux-production/lifecycle/aarch64-core.o $(BUILD)/lib/libhl-engine.a \
	$(BUILD)/lib/libhl-translator.a $(BUILD)/lib/libhl-linux-abi.a $(BUILD)/lib/libhl-host-linux.a
	@mkdir -p $(@D)
	$(CC) -o $@ $^ -pthread -lm -ldl -latomic

$(BUILD)/linux-production/hl-engine-linux-x86_64: $(BUILD)/linux-production/target/x86_64.o \
	$(BUILD)/linux-production/lifecycle/x86_64-core.o $(BUILD)/lib/libhl-engine.a \
	$(BUILD)/lib/libhl-translator.a $(BUILD)/lib/libhl-linux-abi.a $(BUILD)/lib/libhl-host-linux.a
	@mkdir -p $(@D)
	$(CC) -o $@ $^ -pthread -lm

$(BUILD)/linux-production/linux-production-smoke: tools/linux_production_smoke.c
	@mkdir -p $(@D)
	$(CC) -O2 -std=c11 -Wall -Wextra -Werror $< -o $@

.PHONY: run-linux-production-smoke
run-linux-production-smoke: $(BUILD)/linux-production/hl-engine-linux-x86_64 \
	$(BUILD)/linux-production/linux-production-smoke $(BUILD)/compat/core/abi/x86_64/hello
	$(BUILD)/linux-production/linux-production-smoke $(BUILD)/linux-production/hl-engine-linux-x86_64 \
		$(BUILD)/compat/core/abi/x86_64/hello

.PHONY: run-linux-production-aarch64-smoke
run-linux-production-aarch64-smoke: $(BUILD)/linux-production/hl-engine-linux-aarch64 \
	$(BUILD)/linux-production/linux-production-smoke $(BUILD)/compat/core/abi/aarch64/hello
	$(BUILD)/linux-production/linux-production-smoke $(BUILD)/linux-production/hl-engine-linux-aarch64 \
		$(BUILD)/compat/core/abi/aarch64/hello

LINUX_PRODUCTION_MATRIX_CASES := \
	$(BUILD)/compat/core/abi/x86_64/files \
	$(BUILD)/compat/core/abi/x86_64/statfile \
	$(BUILD)/compat/core/abi/x86_64/mmapanon \
	$(BUILD)/compat/core/abi/x86_64/pipe \
	$(BUILD)/compat/posix/x86_64/pollpipe \
	$(BUILD)/compat/posix/x86_64/waitstatus \
	$(BUILD)/compat/signals/x86_64/signals_core \
	$(BUILD)/compat/memory/x86_64/mprotect_enforce \
	$(BUILD)/compat/posix/x86_64/mmapfile \
	$(BUILD)/compat/core/syscall/x86_64/epoll \
	$(BUILD)/compat/core/syscall/x86_64/epoll_edge \
	$(BUILD)/compat/core/syscall/x86_64/epoll_dup_lifetime \
	$(BUILD)/compat/core/syscall/x86_64/epoll_fork_inherit \
	$(BUILD)/e2e/epoll_mod-x86_64 \
	$(BUILD)/e2e/epoll_et-x86_64 \
	$(BUILD)/e2e/epoll_reblock_inf-x86_64 \
	$(BUILD)/compat/syscall/x86_64/epoll_pwait \
	$(BUILD)/compat/core/syscall/x86_64/eventfd \
	$(BUILD)/compat/core/syscall/x86_64/eventfd_sema \
	$(BUILD)/e2e/eventfd_nonblock-x86_64 \
	$(BUILD)/e2e/timerfd-x86_64 \
	$(BUILD)/e2e/timerfd_interval-x86_64 \
	$(BUILD)/e2e/epoll_oneshot-x86_64 \
	$(BUILD)/compat/procfs/x86_64/peerfd

$(BUILD)/tools/linux-matrix: tools/linux_matrix.c
	@mkdir -p $(@D)
	$(CC) -O2 -std=c11 -Wall -Wextra -Werror $< -o $@

.PHONY: test-linux-production-matrix
test-linux-production-matrix: $(BUILD)/linux-production/hl-engine-linux-x86_64 \
	$(BUILD)/tools/linux-matrix $(LINUX_PRODUCTION_MATRIX_CASES)
	$(BUILD)/tools/linux-matrix $(BUILD)/linux-production/hl-engine-linux-x86_64 \
		$(BUILD)/compat/core/abi/x86_64/files tests/compat/core/abi/expected/files.out 0 \
		$(BUILD)/compat/core/abi/x86_64/statfile tests/compat/core/abi/expected/statfile.out 0 \
		$(BUILD)/compat/core/abi/x86_64/mmapanon tests/compat/core/abi/expected/mmapanon.out 0 \
		$(BUILD)/compat/core/abi/x86_64/pipe tests/compat/core/abi/expected/pipe.out 0 \
		$(BUILD)/compat/posix/x86_64/pollpipe tests/compat/posix/expected/pollpipe.out 0 \
		$(BUILD)/compat/posix/x86_64/waitstatus tests/compat/posix/expected/waitstatus.out 0 \
		$(BUILD)/compat/signals/x86_64/signals_core tests/compat/signals/expected/signals_core.out 0 \
		$(BUILD)/compat/memory/x86_64/mprotect_enforce tests/compat/memory/expected/mprotect_enforce.out 0 \
		$(BUILD)/compat/posix/x86_64/mmapfile tests/compat/posix/expected/mmapfile.out 0 \
		$(BUILD)/compat/core/syscall/x86_64/epoll tests/compat/core/syscall/expected/epoll.out 0 \
		$(BUILD)/compat/core/syscall/x86_64/epoll_edge tests/compat/core/syscall/expected/epoll_edge.out 0 \
		$(BUILD)/compat/core/syscall/x86_64/epoll_dup_lifetime tests/compat/core/syscall/expected/epoll_dup_lifetime.out 0 \
		$(BUILD)/compat/core/syscall/x86_64/epoll_fork_inherit tests/compat/core/syscall/expected/epoll_fork_inherit.out 0 \
		$(BUILD)/e2e/epoll_mod-x86_64 tests/compat/syscall/expected/epoll_mod.out 0 \
		$(BUILD)/e2e/epoll_et-x86_64 tests/compat/syscall/expected/epoll_et.out 0 \
		$(BUILD)/e2e/epoll_reblock_inf-x86_64 tests/compat/syscall/expected/epoll_reblock_inf.out 0 \
		$(BUILD)/compat/syscall/x86_64/epoll_pwait tests/compat/syscall/expected/epoll_pwait.out 0 \
		$(BUILD)/compat/core/syscall/x86_64/eventfd tests/compat/core/syscall/expected/eventfd.out 0 \
		$(BUILD)/compat/core/syscall/x86_64/eventfd_sema tests/compat/core/syscall/expected/eventfd_sema.out 0 \
		$(BUILD)/e2e/eventfd_nonblock-x86_64 tests/compat/syscall/expected/eventfd_nonblock.out 0 \
		$(BUILD)/e2e/timerfd-x86_64 tests/compat/core/syscall/expected/timerfd.out 0 \
		$(BUILD)/e2e/timerfd_interval-x86_64 tests/compat/syscall/expected/timerfd_interval.out 0 \
		$(BUILD)/e2e/epoll_oneshot-x86_64 tests/compat/syscall/expected/epoll_oneshot.out 0 \
		$(BUILD)/compat/procfs/x86_64/peerfd tests/compat/procfs/expected/shared/peerfd.out 0

.PHONY: test-linux-production-core-abi
test-linux-production-core-abi: $(BUILD)/linux-production/hl-engine-linux-x86_64 \
	$(BUILD)/tools/linux-matrix $(filter %/x86_64/%,$(CORE_ABI_BINS))
	$(BUILD)/tools/linux-matrix --suite $(BUILD)/linux-production/hl-engine-linux-x86_64 \
		$(BUILD)/compat/core/abi/x86_64 tests/compat/core/abi

.PHONY: test-linux-production-config
test-linux-production-config: $(BUILD)/linux-production/hl-engine-linux-x86_64 \
	$(BUILD)/tools/config-e2e-runner $(BUILD)/tools/remote-supervisor $(BUILD)/e2e/guest-exit-x86_64 \
	$(BUILD)/e2e/guest-exit70-x86_64
	$(BUILD)/tools/config-e2e-runner env $(abspath $(BUILD)/linux-production/hl-engine-linux-x86_64) \
		$(abspath $(BUILD)/e2e/guest-exit-x86_64) 42 32
	$(BUILD)/tools/config-e2e-runner $(abspath $(BUILD)/tools/remote-supervisor) \
		$(abspath $(BUILD)/linux-production/hl-engine-linux-x86_64) \
		$(abspath $(BUILD)/e2e/guest-exit-x86_64) 42 32
	$(BUILD)/tools/config-e2e-runner env $(abspath $(BUILD)/linux-production/hl-engine-linux-x86_64) \
		$(abspath $(BUILD)/e2e/guest-exit70-x86_64) 70

LINUX_PRODUCTION_COMPAT_BINS := $(filter %/x86_64/%,$(ABI_CASE_BINS) $(ABI_CORPUS_BINS) $(LIBC_CASE_BINS) \
	$(COMPLETENESS_BINS) $(POSIX_CASE_BINS) $(SYSCALL_CASE_BINS) $(NETWORK_CASE_BINS) $(PROCFS_CASE_BINS) \
	$(MEMORY_CASE_BINS) $(FILESYSTEM_CASE_BINS) $(SIGNALS_CASE_BINS) $(PROCESS_CASE_BINS) $(TIME_CASE_BINS) \
	$(ISA_X86_64_BINS) $(CORE_ABI_BINS) $(CORE_WORKLOAD_BINS) $(CORE_SYSCALL_BINS) $(CORE_REGRESS_BINS) \
	$(IPC_CASE_BINS) $(THREAD_CASE_BINS) $(ISOLATION_CASE_BINS) $(SYSCALL_EDGE_CASE_BINS) $(SOAK_CASE_BINS))

LINUX_PRODUCTION_AARCH64_COMPAT_BINS := $(filter %/aarch64/%,$(ABI_CASE_BINS) $(ABI_CORPUS_BINS) \
	$(LIBC_CASE_BINS) $(COMPLETENESS_BINS) $(POSIX_CASE_BINS) $(SYSCALL_CASE_BINS) $(NETWORK_CASE_BINS) \
	$(PROCFS_CASE_BINS) $(MEMORY_CASE_BINS) $(FILESYSTEM_CASE_BINS) $(SIGNALS_CASE_BINS) $(PROCESS_CASE_BINS) \
	$(TIME_CASE_BINS) $(CORE_ABI_BINS) $(CORE_WORKLOAD_BINS) $(CORE_SYSCALL_BINS) $(CORE_REGRESS_BINS) \
	$(IPC_CASE_BINS) $(THREAD_CASE_BINS) $(ISOLATION_CASE_BINS) $(SYSCALL_EDGE_CASE_BINS) $(SOAK_CASE_BINS))

define HL_LINUX_PRODUCTION_SUITE
	$(BUILD)/tools/linux-matrix --suite $(BUILD)/linux-production/hl-engine-linux-x86_64 $(1) $(2)
endef

define HL_LINUX_PRODUCTION_AARCH64_SUITE
	$(BUILD)/tools/linux-matrix --suite $(BUILD)/linux-production/hl-engine-linux-aarch64 $(1) $(2)
endef

.PHONY: test-linux-production-aarch64-full
test-linux-production-aarch64-full: $(BUILD)/linux-production/hl-engine-linux-aarch64 \
	$(BUILD)/tools/linux-matrix $(LINUX_PRODUCTION_AARCH64_COMPAT_BINS)
	$(call HL_LINUX_PRODUCTION_AARCH64_SUITE,$(BUILD)/compat/abi/aarch64,tests/compat/abi)
	$(call HL_LINUX_PRODUCTION_AARCH64_SUITE,$(BUILD)/compat/abi-corpus/aarch64,tests/compat/abi/corpus)
	$(call HL_LINUX_PRODUCTION_AARCH64_SUITE,$(BUILD)/compat/libc/aarch64,tests/compat/libc)
	$(call HL_LINUX_PRODUCTION_AARCH64_SUITE,$(BUILD)/compat/completeness/aarch64,tests/compat/completeness)
	$(call HL_LINUX_PRODUCTION_AARCH64_SUITE,$(BUILD)/compat/posix/aarch64,tests/compat/posix)
	$(call HL_LINUX_PRODUCTION_AARCH64_SUITE,$(BUILD)/compat/syscall/aarch64,tests/compat/syscall)
	$(call HL_LINUX_PRODUCTION_AARCH64_SUITE,$(BUILD)/compat/network/aarch64,tests/compat/network)
	$(call HL_LINUX_PRODUCTION_AARCH64_SUITE,$(BUILD)/compat/procfs/aarch64,tests/compat/procfs)
	$(call HL_LINUX_PRODUCTION_AARCH64_SUITE,$(BUILD)/compat/memory/aarch64,tests/compat/memory)
	$(call HL_LINUX_PRODUCTION_AARCH64_SUITE,$(BUILD)/compat/filesystem/aarch64,tests/compat/filesystem)
	$(call HL_LINUX_PRODUCTION_AARCH64_SUITE,$(BUILD)/compat/signals/aarch64,tests/compat/signals)
	$(call HL_LINUX_PRODUCTION_AARCH64_SUITE,$(BUILD)/compat/process/aarch64,tests/compat/process)
	$(call HL_LINUX_PRODUCTION_AARCH64_SUITE,$(BUILD)/compat/time/aarch64,tests/compat/time)
	$(call HL_LINUX_PRODUCTION_AARCH64_SUITE,$(BUILD)/compat/core/abi/aarch64,tests/compat/core/abi)
	$(call HL_LINUX_PRODUCTION_AARCH64_SUITE,$(BUILD)/compat/core/workload/aarch64,tests/compat/core/workload)
	$(call HL_LINUX_PRODUCTION_AARCH64_SUITE,$(BUILD)/compat/core/syscall/aarch64,tests/compat/core/syscall)
	$(call HL_LINUX_PRODUCTION_AARCH64_SUITE,$(BUILD)/compat/core/regress/aarch64,tests/compat/core/regress)
	$(call HL_LINUX_PRODUCTION_AARCH64_SUITE,$(BUILD)/compat/ipc/aarch64,tests/compat/ipc)
	$(call HL_LINUX_PRODUCTION_AARCH64_SUITE,$(BUILD)/compat/threads/aarch64,tests/compat/threads)
	$(call HL_LINUX_PRODUCTION_AARCH64_SUITE,$(BUILD)/compat/isolation/aarch64,tests/compat/isolation)
	$(call HL_LINUX_PRODUCTION_AARCH64_SUITE,$(BUILD)/compat/syscall_edges/aarch64,tests/compat/syscall_edges)
	$(BUILD)/tools/linux-matrix --suite $(BUILD)/linux-production/hl-engine-linux-aarch64 \
		$(BUILD)/soak/aarch64 tests/soak

.PHONY: test-linux-production-full
test-linux-production-full: test-linux-production-config $(BUILD)/linux-production/hl-engine-linux-x86_64 \
	$(BUILD)/tools/linux-matrix \
	$(LINUX_PRODUCTION_COMPAT_BINS)
	$(call HL_LINUX_PRODUCTION_SUITE,$(BUILD)/compat/abi/x86_64,tests/compat/abi)
	$(call HL_LINUX_PRODUCTION_SUITE,$(BUILD)/compat/abi-corpus/x86_64,tests/compat/abi/corpus)
	$(call HL_LINUX_PRODUCTION_SUITE,$(BUILD)/compat/libc/x86_64,tests/compat/libc)
	$(call HL_LINUX_PRODUCTION_SUITE,$(BUILD)/compat/completeness/x86_64,tests/compat/completeness)
	$(call HL_LINUX_PRODUCTION_SUITE,$(BUILD)/compat/posix/x86_64,tests/compat/posix)
	$(call HL_LINUX_PRODUCTION_SUITE,$(BUILD)/compat/syscall/x86_64,tests/compat/syscall)
	$(call HL_LINUX_PRODUCTION_SUITE,$(BUILD)/compat/network/x86_64,tests/compat/network)
	$(call HL_LINUX_PRODUCTION_SUITE,$(BUILD)/compat/procfs/x86_64,tests/compat/procfs)
	$(call HL_LINUX_PRODUCTION_SUITE,$(BUILD)/compat/memory/x86_64,tests/compat/memory)
	$(call HL_LINUX_PRODUCTION_SUITE,$(BUILD)/compat/filesystem/x86_64,tests/compat/filesystem)
	$(call HL_LINUX_PRODUCTION_SUITE,$(BUILD)/compat/signals/x86_64,tests/compat/signals)
	$(call HL_LINUX_PRODUCTION_SUITE,$(BUILD)/compat/process/x86_64,tests/compat/process)
	$(call HL_LINUX_PRODUCTION_SUITE,$(BUILD)/compat/time/x86_64,tests/compat/time)
	$(call HL_LINUX_PRODUCTION_SUITE,$(BUILD)/compat/isa/x86_64,tests/compat/isa/x86_64)
	$(call HL_LINUX_PRODUCTION_SUITE,$(BUILD)/compat/core/abi/x86_64,tests/compat/core/abi)
	$(call HL_LINUX_PRODUCTION_SUITE,$(BUILD)/compat/core/workload/x86_64,tests/compat/core/workload)
	$(call HL_LINUX_PRODUCTION_SUITE,$(BUILD)/compat/core/syscall/x86_64,tests/compat/core/syscall)
	$(call HL_LINUX_PRODUCTION_SUITE,$(BUILD)/compat/core/regress/x86_64,tests/compat/core/regress)
	$(call HL_LINUX_PRODUCTION_SUITE,$(BUILD)/compat/ipc/x86_64,tests/compat/ipc)
	$(call HL_LINUX_PRODUCTION_SUITE,$(BUILD)/compat/threads/x86_64,tests/compat/threads)
	$(call HL_LINUX_PRODUCTION_SUITE,$(BUILD)/compat/isolation/x86_64,tests/compat/isolation)
	$(call HL_LINUX_PRODUCTION_SUITE,$(BUILD)/compat/syscall_edges/x86_64,tests/compat/syscall_edges)
	$(call HL_LINUX_PRODUCTION_SUITE,$(BUILD)/soak/x86_64,tests/soak)

compat-engines: $(BUILD)/production/hl-engine-linux-aarch64 $(BUILD)/production/hl-engine-linux-x86_64 \
	$(BUILD)/production/hl-remote-supervisor

$(BUILD)/production/hl-remote-supervisor: tools/remote_supervisor.c
	@mkdir -p $(@D)
	$(MAC) clang $(CFLAGS) $(WARNINGS) $< -o $@

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

$(BUILD)/mac/dual/aarch64-target.o: src/core/target/aarch64.c $(PRODUCTION_UNITY_DEPS)
	@mkdir -p $(@D)
	$(MAC) clang $(CPPFLAGS) -DHL_EMBEDDED_BUILD=1 -DHL_ENGINE_NO_MAIN=1 -DHL_TARGET_NAMESPACE=aarch64 -O2 $(DEPFLAGS) -c $< -o $@

$(BUILD)/mac/dual/x86_64-target.o: src/core/target/x86_64.c $(PRODUCTION_UNITY_DEPS)
	@mkdir -p $(@D)
	$(MAC) clang $(CPPFLAGS) -DHL_EMBEDDED_BUILD=1 -DHL_ENGINE_NO_MAIN=1 -DHL_TARGET_NAMESPACE=x86_64 -O2 $(DEPFLAGS) -c $< -o $@

$(BUILD)/mac/dual/aarch64-core.o: src/core/lifecycle.c src/core/target/namespace.h
	@mkdir -p $(@D)
	$(MAC) clang $(CPPFLAGS) -DHL_EMBEDDED_BUILD=1 -DHL_TARGET_NAMESPACE=aarch64 -DHL_PRODUCTION_GUEST_ISA=HL_GUEST_ISA_AARCH64 -O2 $(DEPFLAGS) -c $< -o $@

$(BUILD)/mac/dual/x86_64-core.o: src/core/lifecycle.c src/core/target/namespace.h
	@mkdir -p $(@D)
	$(MAC) clang $(CPPFLAGS) -DHL_EMBEDDED_BUILD=1 -DHL_TARGET_NAMESPACE=x86_64 -DHL_PRODUCTION_GUEST_ISA=HL_GUEST_ISA_X86_64 -O2 $(DEPFLAGS) -c $< -o $@

$(BUILD)/mac/dual/dispatch.o: src/core/target/dual.c
	@mkdir -p $(@D)
	$(MAC) clang $(CPPFLAGS) -O2 $(DEPFLAGS) -c $< -o $@

$(BUILD)/mac/dual/activation.o: src/core/activation.c include/hl/activation.h
	@mkdir -p $(@D)
	$(MAC) clang $(CPPFLAGS) $(ENGINE_CFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD)/mac/lib/libhl-engine-dual.a: $(BUILD)/mac/dual/aarch64-target.o $(BUILD)/mac/dual/x86_64-target.o \
	$(BUILD)/mac/dual/aarch64-core.o $(BUILD)/mac/dual/x86_64-core.o $(BUILD)/mac/dual/dispatch.o \
	$(BUILD)/mac/dual/activation.o $(EMBEDDED_MAC_OBJECTS)
	@mkdir -p $(@D)
	$(MAC) libtool -static -o $@ $^

$(BUILD)/linux-aarch64/dual/aarch64-target.o: src/core/target/aarch64.c $(PRODUCTION_UNITY_DEPS)
	@mkdir -p $(@D)
	$(AARCH64_LINUX_CC) $(CPPFLAGS) -D_GNU_SOURCE -DHL_EMBEDDED_BUILD=1 -DHL_ENGINE_NO_MAIN=1 \
		-DHL_TARGET_NAMESPACE=aarch64 -O2 $(DEPFLAGS) -c $< -o $@

$(BUILD)/linux-aarch64/dual/x86_64-target.o: src/core/target/x86_64.c $(PRODUCTION_UNITY_DEPS)
	@mkdir -p $(@D)
	$(AARCH64_LINUX_CC) $(CPPFLAGS) -D_GNU_SOURCE -DHL_EMBEDDED_BUILD=1 -DHL_ENGINE_NO_MAIN=1 \
		-DHL_TARGET_NAMESPACE=x86_64 -O2 $(DEPFLAGS) -c $< -o $@

$(BUILD)/linux-aarch64/dual/aarch64-core.o: src/core/lifecycle.c src/core/target/namespace.h
	@mkdir -p $(@D)
	$(AARCH64_LINUX_CC) $(CPPFLAGS) -D_GNU_SOURCE -DHL_EMBEDDED_BUILD=1 -DHL_TARGET_NAMESPACE=aarch64 \
		-DHL_PRODUCTION_GUEST_ISA=HL_GUEST_ISA_AARCH64 -O2 $(DEPFLAGS) -c $< -o $@

$(BUILD)/linux-aarch64/dual/x86_64-core.o: src/core/lifecycle.c src/core/target/namespace.h
	@mkdir -p $(@D)
	$(AARCH64_LINUX_CC) $(CPPFLAGS) -D_GNU_SOURCE -DHL_EMBEDDED_BUILD=1 -DHL_TARGET_NAMESPACE=x86_64 \
		-DHL_PRODUCTION_GUEST_ISA=HL_GUEST_ISA_X86_64 -O2 $(DEPFLAGS) -c $< -o $@

$(BUILD)/linux-aarch64/dual/dispatch.o: src/core/target/dual.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_CC) $(CPPFLAGS) -D_GNU_SOURCE -O2 $(DEPFLAGS) -c $< -o $@

$(BUILD)/linux-aarch64/dual/activation.o: src/core/activation.c include/hl/activation.h
	@mkdir -p $(@D)
	$(AARCH64_LINUX_CC) $(CPPFLAGS) $(ENGINE_CFLAGS) -D_GNU_SOURCE $(DEPFLAGS) -c $< -o $@

$(BUILD)/package/macos-aarch64/libhl-engine.a: $(BUILD)/mac/lib/libhl-engine-dual.a
	@mkdir -p $(@D)
	cp $< $@

$(BUILD)/package/linux-aarch64/libhl-engine.a: $(BUILD)/linux-aarch64/dual/aarch64-target.o \
	$(BUILD)/linux-aarch64/dual/x86_64-target.o $(BUILD)/linux-aarch64/dual/aarch64-core.o \
	$(BUILD)/linux-aarch64/dual/x86_64-core.o $(BUILD)/linux-aarch64/dual/dispatch.o \
	$(BUILD)/linux-aarch64/dual/activation.o $(LINUX_AARCH64_EMBEDDED_OBJECTS)
	@mkdir -p $(@D)
	$(AARCH64_LINUX_AR) rcs $@ $^

.PHONY: package-embedded package-embedded-macos package-embedded-linux
package-embedded-macos: $(BUILD)/package/macos-aarch64/libhl-engine.a \
	$(BUILD)/package/macos-aarch64/link-test

package-embedded-linux: $(BUILD)/package/linux-aarch64/libhl-engine.a \
	$(BUILD)/package/linux-aarch64/link-test

ifeq ($(HOST),macos)
package-embedded: package-embedded-macos
else
package-embedded: package-embedded-linux
endif

$(BUILD)/package/macos-aarch64/link-test: tools/dual_backend_e2e_runner.c \
	$(BUILD)/package/macos-aarch64/libhl-engine.a packaging/macos/jit.entitlements
	$(MAC) clang $(CPPFLAGS) -o $@ $< -Wl,-force_load,$(BUILD)/package/macos-aarch64/libhl-engine.a
	$(MAC) $(CODESIGN) -s - --entitlements packaging/macos/jit.entitlements -f $@

$(BUILD)/package/linux-aarch64/link-test: tools/dual_backend_e2e_runner.c \
	$(BUILD)/package/linux-aarch64/libhl-engine.a
	$(AARCH64_LINUX_CC) -D_GNU_SOURCE $(CPPFLAGS) -o $@ $< -Wl,--whole-archive \
		$(BUILD)/package/linux-aarch64/libhl-engine.a -Wl,--no-whole-archive -pthread -ldl -lm -latomic

$(BUILD)/tools/dual-backend-e2e: tools/dual_backend_e2e_runner.c $(BUILD)/mac/lib/libhl-engine-dual.a \
	packaging/macos/jit.entitlements
	@mkdir -p $(@D)
	$(MAC) clang $(CPPFLAGS) -o $@ $< -Wl,-force_load,$(BUILD)/mac/lib/libhl-engine-dual.a
	$(MAC) $(CODESIGN) -s - --entitlements packaging/macos/jit.entitlements -f $@

.PHONY: test-dual-backend
test-dual-backend: $(BUILD)/tools/dual-backend-e2e $(BUILD)/e2e/guest-exit-aarch64 $(BUILD)/e2e/guest-exit-x86_64 \
	$(BUILD)/e2e/guest-exit70-aarch64 $(BUILD)/e2e/guest-exit70-x86_64 $(BUILD)/e2e/guest-spin-aarch64 \
	$(BUILD)/e2e/guest-output-aarch64
	$(abspath $(BUILD)/tools/dual-backend-e2e) $(abspath $(BUILD)/e2e/guest-exit-aarch64) \
		$(abspath $(BUILD)/e2e/guest-exit-x86_64) $(abspath $(BUILD)/e2e/guest-exit70-aarch64) \
		$(abspath $(BUILD)/e2e/guest-exit70-x86_64) $(abspath $(BUILD)/e2e/guest-spin-aarch64) \
		$(abspath $(BUILD)/e2e/guest-output-aarch64)

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
	$(MAC) $(CODESIGN) -s - --entitlements packaging/macos/jit.entitlements -f $@

$(BUILD)/tools/lifecycle-x86_64: $(BUILD)/mac/lifecycle/x86_64-runner.o \
	$(BUILD)/mac/lifecycle/x86_64-target.o $(BUILD)/mac/lifecycle/x86_64-core.o $(MAC_LIBS) \
	packaging/macos/jit.entitlements
	@mkdir -p $(@D)
	$(MAC) clang -o $@ $(filter %.o %.a,$^)
	$(MAC) $(CODESIGN) -s - --entitlements packaging/macos/jit.entitlements -f $@

$(BUILD)/tools/binding-aarch64: $(BUILD)/mac/binding/aarch64-runner.o \
	$(BUILD)/mac/lifecycle/aarch64-target.o $(BUILD)/mac/lifecycle/aarch64-core.o $(MAC_LIBS) \
	packaging/macos/jit.entitlements
	@mkdir -p $(@D)
	$(MAC) clang -o $@ $(filter %.o %.a,$^)
	$(MAC) $(CODESIGN) -s - --entitlements packaging/macos/jit.entitlements -f $@

$(BUILD)/tools/binding-x86_64: $(BUILD)/mac/binding/x86_64-runner.o \
	$(BUILD)/mac/lifecycle/x86_64-target.o $(BUILD)/mac/lifecycle/x86_64-core.o $(MAC_LIBS) \
	packaging/macos/jit.entitlements
	@mkdir -p $(@D)
	$(MAC) clang -o $@ $(filter %.o %.a,$^)
	$(MAC) $(CODESIGN) -s - --entitlements packaging/macos/jit.entitlements -f $@

$(BUILD)/tools/dir-aarch64: $(BUILD)/mac/dir/aarch64-runner.o \
	$(BUILD)/mac/lifecycle/aarch64-target.o $(BUILD)/mac/lifecycle/aarch64-core.o $(MAC_LIBS) \
	packaging/macos/jit.entitlements
	@mkdir -p $(@D)
	$(MAC) clang -o $@ $(filter %.o %.a,$^)
	$(MAC) $(CODESIGN) -s - --entitlements packaging/macos/jit.entitlements -f $@

$(BUILD)/tools/dir-x86_64: $(BUILD)/mac/dir/x86_64-runner.o \
	$(BUILD)/mac/lifecycle/x86_64-target.o $(BUILD)/mac/lifecycle/x86_64-core.o $(MAC_LIBS) \
	packaging/macos/jit.entitlements
	@mkdir -p $(@D)
	$(MAC) clang -o $@ $(filter %.o %.a,$^)
	$(MAC) $(CODESIGN) -s - --entitlements packaging/macos/jit.entitlements -f $@

$(BUILD)/tools/stdio-aarch64: $(BUILD)/mac/stdio/aarch64-runner.o \
	$(BUILD)/mac/lifecycle/aarch64-target.o $(BUILD)/mac/lifecycle/aarch64-core.o $(MAC_LIBS) \
	packaging/macos/jit.entitlements
	@mkdir -p $(@D)
	$(MAC) clang -o $@ $(filter %.o %.a,$^)
	$(MAC) $(CODESIGN) -s - --entitlements packaging/macos/jit.entitlements -f $@

$(BUILD)/tools/stdio-x86_64: $(BUILD)/mac/stdio/x86_64-runner.o \
	$(BUILD)/mac/lifecycle/x86_64-target.o $(BUILD)/mac/lifecycle/x86_64-core.o $(MAC_LIBS) \
	packaging/macos/jit.entitlements
	@mkdir -p $(@D)
	$(MAC) clang -o $@ $(filter %.o %.a,$^)
	$(MAC) $(CODESIGN) -s - --entitlements packaging/macos/jit.entitlements -f $@

e2e-compat: test-macos compat-engines compat-abi compat-abi-corpus compat-core compat-filesystem compat-ipc compat-threads compat-isa-x86-64 compat-isolation compat-libc compat-completeness compat-memory compat-network compat-posix compat-process compat-procfs compat-signals compat-soak compat-syscall compat-syscall-edges compat-time $(BUILD)/tools/lifecycle-aarch64 $(BUILD)/tools/lifecycle-x86_64 \
	$(BUILD)/tools/binding-aarch64 $(BUILD)/tools/binding-x86_64 \
	$(BUILD)/e2e/fd-binding-aarch64 $(BUILD)/e2e/fd-binding-x86_64 \
	$(BUILD)/tools/stdio-aarch64 $(BUILD)/tools/stdio-x86_64 \
	$(BUILD)/e2e/stdio-binding-aarch64 $(BUILD)/e2e/stdio-binding-x86_64 \
	$(BUILD)/tools/dir-aarch64 $(BUILD)/tools/dir-x86_64 \
	$(BUILD)/e2e/dir-binding-aarch64 $(BUILD)/e2e/dir-binding-x86_64 \
	$(BUILD)/e2e/guest-exit-aarch64 $(BUILD)/e2e/guest-exit-x86_64 \
	$(BUILD)/e2e/guest-exit70-aarch64 $(BUILD)/e2e/guest-exit70-x86_64 \
	$(BUILD)/e2e/guest-exit139-aarch64 $(BUILD)/e2e/guest-exit139-x86_64 \
	$(BUILD)/e2e/guest-fault-aarch64 $(BUILD)/e2e/guest-fault-x86_64 \
	$(BUILD)/e2e/guest-spin-aarch64 $(BUILD)/e2e/guest-spin-x86_64 \
	$(BUILD)/e2e/clock-injected-aarch64 $(BUILD)/e2e/clock-injected-x86_64 \
	$(BUILD)/tools/e2e-runner $(BUILD)/tools/config-e2e-runner $(E2E_NATIVE_ORACLE_RUNS)
	$(BUILD)/tools/e2e-runner $(MAC) $(abspath $(BUILD)/production/hl-engine-linux-aarch64) \
		$(abspath $(BUILD)/e2e/guest-exit-aarch64) 42
	$(BUILD)/tools/e2e-runner $(MAC) $(abspath $(BUILD)/production/hl-engine-linux-x86_64) \
		$(abspath $(BUILD)/e2e/guest-exit-x86_64) 42
	$(BUILD)/tools/config-e2e-runner $(MAC) $(abspath $(BUILD)/production/hl-engine-linux-aarch64) \
		$(abspath $(BUILD)/e2e/guest-exit-aarch64) 42
	$(BUILD)/tools/config-e2e-runner $(MAC) $(abspath $(BUILD)/production/hl-engine-linux-x86_64) \
		$(abspath $(BUILD)/e2e/guest-exit-x86_64) 42
	$(BUILD)/tools/config-e2e-runner $(MAC) $(abspath $(BUILD)/production/hl-engine-linux-aarch64) \
		$(abspath $(BUILD)/e2e/guest-exit70-aarch64) 70
	$(BUILD)/tools/config-e2e-runner $(MAC) $(abspath $(BUILD)/production/hl-engine-linux-x86_64) \
		$(abspath $(BUILD)/e2e/guest-exit70-x86_64) 70
	$(BUILD)/tools/e2e-runner $(MAC) $(abspath $(BUILD)/tools/lifecycle-aarch64) \
		$(abspath $(BUILD)/e2e/guest-exit-aarch64) 42
	$(BUILD)/tools/e2e-runner $(MAC) $(abspath $(BUILD)/tools/lifecycle-x86_64) \
		$(abspath $(BUILD)/e2e/guest-exit-x86_64) 42
	$(MAC) $(abspath $(BUILD)/tools/lifecycle-aarch64) --expect-exit 139 \
		$(abspath $(BUILD)/e2e/guest-exit139-aarch64)
	$(MAC) $(abspath $(BUILD)/tools/lifecycle-x86_64) --expect-exit 139 \
		$(abspath $(BUILD)/e2e/guest-exit139-x86_64)
	$(MAC) $(abspath $(BUILD)/tools/lifecycle-aarch64) --expect-signal 11 \
		$(abspath $(BUILD)/e2e/guest-fault-aarch64)
	$(MAC) $(abspath $(BUILD)/tools/lifecycle-x86_64) --expect-signal 11 \
		$(abspath $(BUILD)/e2e/guest-fault-x86_64)
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

$(BUILD)/tools/compat-runner: tools/compat_runner.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(WARNINGS) $< -o $@

$(BUILD)/tools/e2e-runner: tools/e2e_runner.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(WARNINGS) $< -o $@

$(BUILD)/tools/forkserver-runner: tests/compat/process/integration/forkserver_runner.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(WARNINGS) $< -o $@

$(BUILD)/tools/matrix-runner: tools/matrix_runner.c include/hl/config.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) \
		-DAARCH64_DYNAMIC_LOADER='"$(AARCH64_DYNAMIC_LOADER)"' \
		-DAARCH64_DYNAMIC_LIBC='"$(AARCH64_DYNAMIC_LIBC)"' \
		-DX86_64_DYNAMIC_LOADER='"$(X86_64_DYNAMIC_LOADER)"' \
		-DX86_64_DYNAMIC_LIBC='"$(X86_64_DYNAMIC_LIBC)"' $< -lc -o $@

$(BUILD)/linux-production/hl-remote-supervisor: tools/remote_supervisor.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(WARNINGS) $< -lc -o $@

.PHONY: test-linux-production-typed
test-linux-production-typed: $(BUILD)/linux-production/hl-engine-linux-aarch64 \
	$(BUILD)/linux-production/hl-engine-linux-x86_64 $(BUILD)/linux-production/hl-remote-supervisor \
	$(BUILD)/tools/matrix-runner $(FILESYSTEM_CASE_BINS) $(ISOLATION_CASE_BINS) $(NETWORK_CASE_BINS) \
	$(PROCFS_CASE_BINS) $(PROCESS_CASE_BINS) $(MEMORY_CASE_BINS) $(SYSCALL_CASE_BINS) \
	$(SYSCALL_EDGE_CASE_BINS) $(ABI_CASE_BINS) $(COMPLETENESS_BINS) $(IPC_CASE_BINS) \
	$(LIBC_CASE_BINS) $(POSIX_CASE_BINS) $(SIGNALS_CASE_BINS) $(THREAD_CASE_BINS) $(TIME_CASE_BINS)
	$(BUILD)/tools/matrix-runner env $(abspath $(BUILD)/linux-production/hl-engine-linux-aarch64) \
		$(abspath $(BUILD)/compat/filesystem/aarch64) \
		$(abspath $(BUILD)/linux-production/hl-engine-linux-x86_64) \
		$(abspath $(BUILD)/compat/filesystem/x86_64) $(abspath tests/compat/filesystem)
	$(BUILD)/tools/matrix-runner env $(abspath $(BUILD)/linux-production/hl-engine-linux-aarch64) \
		$(abspath $(BUILD)/compat/isolation/aarch64) \
		$(abspath $(BUILD)/linux-production/hl-engine-linux-x86_64) \
		$(abspath $(BUILD)/compat/isolation/x86_64) $(abspath tests/compat/isolation)
	$(BUILD)/tools/matrix-runner env $(abspath $(BUILD)/linux-production/hl-engine-linux-aarch64) \
		$(abspath $(BUILD)/compat/network/aarch64) \
		$(abspath $(BUILD)/linux-production/hl-engine-linux-x86_64) \
		$(abspath $(BUILD)/compat/network/x86_64) $(abspath tests/compat/network)
	$(BUILD)/tools/matrix-runner env $(abspath $(BUILD)/linux-production/hl-engine-linux-aarch64) \
		$(abspath $(BUILD)/compat/procfs/aarch64) \
		$(abspath $(BUILD)/linux-production/hl-engine-linux-x86_64) \
		$(abspath $(BUILD)/compat/procfs/x86_64) $(abspath tests/compat/procfs)
	$(BUILD)/tools/matrix-runner env $(abspath $(BUILD)/linux-production/hl-engine-linux-aarch64) \
		$(abspath $(BUILD)/compat/process/aarch64) \
		$(abspath $(BUILD)/linux-production/hl-engine-linux-x86_64) \
		$(abspath $(BUILD)/compat/process/x86_64) $(abspath tests/compat/process)
	$(BUILD)/tools/matrix-runner env $(abspath $(BUILD)/linux-production/hl-engine-linux-aarch64) \
		$(abspath $(BUILD)/compat/memory/aarch64) \
		$(abspath $(BUILD)/linux-production/hl-engine-linux-x86_64) \
		$(abspath $(BUILD)/compat/memory/x86_64) $(abspath tests/compat/memory)
	$(BUILD)/tools/matrix-runner env $(abspath $(BUILD)/linux-production/hl-engine-linux-aarch64) \
		$(abspath $(BUILD)/compat/syscall/aarch64) \
		$(abspath $(BUILD)/linux-production/hl-engine-linux-x86_64) \
		$(abspath $(BUILD)/compat/syscall/x86_64) $(abspath tests/compat/syscall)
	$(BUILD)/tools/matrix-runner env $(abspath $(BUILD)/linux-production/hl-engine-linux-aarch64) \
		$(abspath $(BUILD)/compat/syscall_edges/aarch64) \
		$(abspath $(BUILD)/linux-production/hl-engine-linux-x86_64) \
		$(abspath $(BUILD)/compat/syscall_edges/x86_64) $(abspath tests/compat/syscall_edges)
	$(BUILD)/tools/matrix-runner env $(abspath $(BUILD)/linux-production/hl-engine-linux-aarch64) \
		$(abspath $(BUILD)/compat/abi/aarch64) \
		$(abspath $(BUILD)/linux-production/hl-engine-linux-x86_64) \
		$(abspath $(BUILD)/compat/abi/x86_64) $(abspath tests/compat/abi)
	$(BUILD)/tools/matrix-runner env $(abspath $(BUILD)/linux-production/hl-engine-linux-aarch64) \
		$(abspath $(BUILD)/compat/completeness/aarch64) \
		$(abspath $(BUILD)/linux-production/hl-engine-linux-x86_64) \
		$(abspath $(BUILD)/compat/completeness/x86_64) $(abspath tests/compat/completeness)
	$(BUILD)/tools/matrix-runner env $(abspath $(BUILD)/linux-production/hl-engine-linux-aarch64) \
		$(abspath $(BUILD)/compat/ipc/aarch64) \
		$(abspath $(BUILD)/linux-production/hl-engine-linux-x86_64) \
		$(abspath $(BUILD)/compat/ipc/x86_64) $(abspath tests/compat/ipc)
	$(BUILD)/tools/matrix-runner env $(abspath $(BUILD)/linux-production/hl-engine-linux-aarch64) \
		$(abspath $(BUILD)/compat/libc/aarch64) \
		$(abspath $(BUILD)/linux-production/hl-engine-linux-x86_64) \
		$(abspath $(BUILD)/compat/libc/x86_64) $(abspath tests/compat/libc)
	$(BUILD)/tools/matrix-runner env $(abspath $(BUILD)/linux-production/hl-engine-linux-aarch64) \
		$(abspath $(BUILD)/compat/posix/aarch64) \
		$(abspath $(BUILD)/linux-production/hl-engine-linux-x86_64) \
		$(abspath $(BUILD)/compat/posix/x86_64) $(abspath tests/compat/posix)
	$(BUILD)/tools/matrix-runner env $(abspath $(BUILD)/linux-production/hl-engine-linux-aarch64) \
		$(abspath $(BUILD)/compat/signals/aarch64) \
		$(abspath $(BUILD)/linux-production/hl-engine-linux-x86_64) \
		$(abspath $(BUILD)/compat/signals/x86_64) $(abspath tests/compat/signals)
	$(BUILD)/tools/matrix-runner env $(abspath $(BUILD)/linux-production/hl-engine-linux-aarch64) \
		$(abspath $(BUILD)/compat/threads/aarch64) \
		$(abspath $(BUILD)/linux-production/hl-engine-linux-x86_64) \
		$(abspath $(BUILD)/compat/threads/x86_64) $(abspath tests/compat/threads)
	$(BUILD)/tools/matrix-runner env $(abspath $(BUILD)/linux-production/hl-engine-linux-aarch64) \
		$(abspath $(BUILD)/compat/time/aarch64) \
		$(abspath $(BUILD)/linux-production/hl-engine-linux-x86_64) \
		$(abspath $(BUILD)/compat/time/x86_64) $(abspath tests/compat/time)

$(BUILD)/tools/remote-supervisor: tools/remote_supervisor.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(WARNINGS) $< -o $@

$(BUILD)/tests/remote-supervisor: tests/integration/remote_supervisor.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(WARNINGS) $< -o $@

.PHONY: remote-supervisor-test
remote-supervisor-test: $(BUILD)/tools/remote-supervisor $(BUILD)/tests/remote-supervisor
	$(BUILD)/tests/remote-supervisor $(BUILD)/tools/remote-supervisor

compat-abi: compat-engines $(BUILD)/tools/matrix-runner $(ABI_CASE_BINS)
	$(BUILD)/tools/matrix-runner $(MAC) $(abspath $(BUILD)/production/hl-engine-linux-aarch64) \
		$(abspath $(BUILD)/compat/abi/aarch64) $(abspath $(BUILD)/production/hl-engine-linux-x86_64) \
		$(abspath $(BUILD)/compat/abi/x86_64) $(abspath tests/compat/abi)

compat-abi-corpus: compat-engines $(BUILD)/tools/matrix-runner $(ABI_CORPUS_BINS)
	$(BUILD)/tools/matrix-runner $(MAC) $(abspath $(BUILD)/production/hl-engine-linux-aarch64) \
		$(abspath $(BUILD)/compat/abi-corpus/aarch64) $(abspath $(BUILD)/production/hl-engine-linux-x86_64) \
		$(abspath $(BUILD)/compat/abi-corpus/x86_64) $(abspath tests/compat/abi/corpus)

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

compat-process: compat-engines $(BUILD)/tools/matrix-runner $(BUILD)/tools/forkserver-runner $(PROCESS_CASE_BINS)
	$(BUILD)/tools/matrix-runner $(MAC) $(abspath $(BUILD)/production/hl-engine-linux-aarch64) \
		$(abspath $(BUILD)/compat/process/aarch64) $(abspath $(BUILD)/production/hl-engine-linux-x86_64) \
		$(abspath $(BUILD)/compat/process/x86_64) $(abspath tests/compat/process)
	$(BUILD)/tools/forkserver-runner $(MAC) $(abspath $(BUILD)/production/hl-engine-linux-aarch64) \
		$(abspath $(BUILD)/compat/process/aarch64/forkserver_probe) \
		$(abspath tests/compat/process/expected/forkserver_integration.out)
	$(BUILD)/tools/forkserver-runner $(MAC) $(abspath $(BUILD)/production/hl-engine-linux-x86_64) \
		$(abspath $(BUILD)/compat/process/x86_64/forkserver_probe) \
		$(abspath tests/compat/process/expected/forkserver_integration.out)

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

.PHONY: compat-core-workload-extended
compat-core-workload-extended: compat-engines $(BUILD)/tools/matrix-runner $(CORE_WORKLOAD_BINS)
	$(BUILD)/tools/matrix-runner $(MAC) $(abspath $(BUILD)/production/hl-engine-linux-aarch64) \
		$(abspath $(BUILD)/compat/core/workload/aarch64) $(abspath $(BUILD)/production/hl-engine-linux-x86_64) \
		$(abspath $(BUILD)/compat/core/workload/x86_64) $(abspath tests/compat/core/workload) --repeat 10

compat-core-syscall: compat-engines $(BUILD)/tools/matrix-runner $(CORE_SYSCALL_BINS)
	$(BUILD)/tools/matrix-runner $(MAC) $(abspath $(BUILD)/production/hl-engine-linux-aarch64) \
		$(abspath $(BUILD)/compat/core/syscall/aarch64) $(abspath $(BUILD)/production/hl-engine-linux-x86_64) \
		$(abspath $(BUILD)/compat/core/syscall/x86_64) $(abspath tests/compat/core/syscall)

compat-core-regress: compat-engines $(BUILD)/tools/matrix-runner $(CORE_REGRESS_BINS)
	$(BUILD)/tools/matrix-runner $(MAC) $(abspath $(BUILD)/production/hl-engine-linux-aarch64) \
		$(abspath $(BUILD)/compat/core/regress/aarch64) $(abspath $(BUILD)/production/hl-engine-linux-x86_64) \
		$(abspath $(BUILD)/compat/core/regress/x86_64) $(abspath tests/compat/core/regress)

compat-ipc: compat-engines $(BUILD)/tools/matrix-runner $(IPC_CASE_BINS)
	$(BUILD)/tools/matrix-runner $(MAC) $(abspath $(BUILD)/production/hl-engine-linux-aarch64) \
		$(abspath $(BUILD)/compat/ipc/aarch64) $(abspath $(BUILD)/production/hl-engine-linux-x86_64) \
		$(abspath $(BUILD)/compat/ipc/x86_64) $(abspath tests/compat/ipc)

compat-threads: compat-engines $(BUILD)/tools/matrix-runner $(THREAD_CASE_BINS)
	$(BUILD)/tools/matrix-runner $(MAC) $(abspath $(BUILD)/production/hl-engine-linux-aarch64) \
		$(abspath $(BUILD)/compat/threads/aarch64) $(abspath $(BUILD)/production/hl-engine-linux-x86_64) \
		$(abspath $(BUILD)/compat/threads/x86_64) $(abspath tests/compat/threads)

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

.PHONY: compat-soak-extended
compat-soak-extended: compat-engines $(BUILD)/tools/matrix-runner $(SOAK_CASE_BINS)
	$(BUILD)/tools/matrix-runner $(MAC) $(abspath $(BUILD)/production/hl-engine-linux-aarch64) \
		$(abspath $(BUILD)/soak/aarch64) $(abspath $(BUILD)/production/hl-engine-linux-x86_64) \
		$(abspath $(BUILD)/soak/x86_64) $(abspath tests/soak) --repeat 10

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

$(BUILD)/perf/syscall-aarch64: tests/perf/syscall.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_STATIC_CC) -O2 -static-pie $< -o $@

$(BUILD)/perf/syscall-x86_64: tests/perf/syscall.c
	@mkdir -p $(@D)
	$(X86_64_LINUX_STATIC_CC) -O2 -static-pie $< -o $@

$(BUILD)/perf/translate-aarch64: tests/perf/translate.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_STATIC_CC) -O2 -static-pie -std=c11 $< -o $@

$(BUILD)/perf/translate-x86_64: tests/perf/translate.c
	@mkdir -p $(@D)
	$(X86_64_LINUX_STATIC_CC) -O2 -static-pie -std=c11 $< -o $@

PERF_OPS := mmap file pipe event ipc-latency ipc-throughput
PERF_OP_mmap := 1
PERF_OP_file := 2
PERF_OP_pipe := 3
PERF_OP_event := 4
PERF_OP_ipc-latency := 5
PERF_OP_ipc-throughput := 6

define HL_PERF_OP_RULES
$(BUILD)/perf/$(1)-aarch64: tests/perf/ops.c
	@mkdir -p $$(@D)
	$(AARCH64_LINUX_STATIC_CC) -O2 -static-pie -std=gnu11 -DHL_PERF_OP=$(PERF_OP_$(1)) $$< -o $$@

$(BUILD)/perf/$(1)-x86_64: tests/perf/ops.c
	@mkdir -p $$(@D)
	$(X86_64_LINUX_STATIC_CC) -O2 -static-pie -std=gnu11 -DHL_PERF_OP=$(PERF_OP_$(1)) $$< -o $$@
endef

$(foreach operation,$(PERF_OPS),$(eval $(call HL_PERF_OP_RULES,$(operation))))

$(BUILD)/perf/resource-aarch64: tests/perf/resource.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_STATIC_CC) -O2 -static-pie -std=gnu11 -pthread $< -o $@

$(BUILD)/perf/resource-x86_64: tests/perf/resource.c
	@mkdir -p $(@D)
	$(X86_64_LINUX_STATIC_CC) -O2 -static-pie -std=gnu11 -pthread $< -o $@

define HL_PERF_ENGINE
	$(BUILD)/tools/perf-runner --label $(1)-$(2) --host-os $(PERF_MAC_OS) \
		--host-release $(PERF_MAC_RELEASE) --host-arch $(PERF_MAC_ARCH) \
		--warmups $(3) --samples $(4) --expect $(6) \
		--max-cold-us $(word 1,$(PERF_MAC_LIMIT_$(1))) --max-p99-us $(word 2,$(PERF_MAC_LIMIT_$(1))) -- \
		$(MAC) $(abspath $(BUILD)/production/hl-engine-linux-$(2)) $(abspath $(5))
endef

define HL_PERF_NATIVE
	$(BUILD)/tools/perf-runner --label native-$(1)-aarch64 --warmups $(2) --samples $(3) --expect $(5) -- \
		$(abspath $(4))
endef

define HL_PERF_LINUX
	$(BUILD)/tools/perf-runner --label linux-$(1)-$(2) --warmups $(3) --samples $(4) --expect $(6) \
		--max-cold-us $(word 1,$(PERF_LIMIT_$(1))) --max-p99-us $(word 2,$(PERF_LIMIT_$(1))) -- \
		$(abspath $(BUILD)/linux-production/hl-engine-linux-$(2)) $(abspath $(5))
endef

define HL_PERF_CACHE_MAC
	$(RM) -r $(BUILD)/perf/cache-warm-mac-$(1)
	$(BUILD)/tools/perf-runner --label mac-warm-cache-$(1) --host-os $(PERF_MAC_OS) \
		--host-release $(PERF_MAC_RELEASE) --host-arch $(PERF_MAC_ARCH) \
		--warmups $(PERF_WARMUPS) --samples $(PERF_SAMPLES) \
		--max-cold-us $(word 1,$(PERF_MAC_LIMIT_warm-cache)) \
		--max-p99-us $(word 2,$(PERF_MAC_LIMIT_warm-cache)) -- \
		$(BUILD)/tools/config-e2e-runner $(MAC) $(abspath $(BUILD)/production/hl-engine-linux-$(1)) \
		$(abspath $(BUILD)/perf/translate-$(1)) 0 1 $(abspath $(BUILD)/perf/cache-warm-mac-$(1))
endef

define HL_PERF_CACHE_LINUX
	$(RM) -r $(BUILD)/perf/cache-warm-linux-$(1)
	$(BUILD)/tools/perf-runner --label linux-warm-cache-$(1) \
		--warmups $(PERF_WARMUPS) --samples $(PERF_SAMPLES) \
		--max-cold-us $(word 1,$(PERF_LIMIT_warm-cache)) \
		--max-p99-us $(word 2,$(PERF_LIMIT_warm-cache)) -- \
		$(BUILD)/tools/config-e2e-runner env $(abspath $(BUILD)/linux-production/hl-engine-linux-$(1)) \
		$(abspath $(BUILD)/perf/translate-$(1)) 0 1 $(abspath $(BUILD)/perf/cache-warm-linux-$(1))
endef

# Keep the correctness gate explicit: standalone performance runs remain quick, while this target
# proves the measured binaries still pass the complete compatibility matrix first.
perf-compat: e2e-compat perf-macos

perf-macos: compat-engines $(BUILD)/tools/perf-runner $(BUILD)/tools/config-e2e-runner $(BUILD)/e2e/guest-exit-aarch64 \
	$(BUILD)/e2e/guest-exit-x86_64 $(BUILD)/compat/core/workload/aarch64/busyloop \
	$(BUILD)/compat/core/workload/x86_64/busyloop $(BUILD)/compat/syscall/aarch64/gettid \
	$(BUILD)/compat/syscall/x86_64/gettid $(BUILD)/perf/syscall-aarch64 $(BUILD)/perf/syscall-x86_64 \
	$(BUILD)/compat/process/aarch64/forkstorm \
	$(BUILD)/compat/process/x86_64/forkstorm \
	$(BUILD)/perf/translate-aarch64 $(BUILD)/perf/translate-x86_64 \
	$(foreach operation,$(PERF_OPS),$(BUILD)/perf/$(operation)-aarch64 $(BUILD)/perf/$(operation)-x86_64) \
	$(BUILD)/perf/resource-aarch64 $(BUILD)/perf/resource-x86_64
	$(call HL_PERF_ENGINE,startup,aarch64,$(PERF_WARMUPS),$(PERF_SAMPLES),$(BUILD)/e2e/guest-exit-aarch64,42)
	$(call HL_PERF_ENGINE,startup,x86_64,$(PERF_WARMUPS),$(PERF_SAMPLES),$(BUILD)/e2e/guest-exit-x86_64,42)
	$(call HL_PERF_ENGINE,compute,aarch64,$(PERF_WARMUPS),$(PERF_HEAVY_SAMPLES),$(BUILD)/compat/core/workload/aarch64/busyloop,0)
	$(call HL_PERF_ENGINE,compute,x86_64,$(PERF_WARMUPS),$(PERF_HEAVY_SAMPLES),$(BUILD)/compat/core/workload/x86_64/busyloop,0)
	$(call HL_PERF_ENGINE,syscall-startup,aarch64,$(PERF_WARMUPS),$(PERF_SAMPLES),$(BUILD)/compat/syscall/aarch64/gettid,0)
	$(call HL_PERF_ENGINE,syscall-startup,x86_64,$(PERF_WARMUPS),$(PERF_SAMPLES),$(BUILD)/compat/syscall/x86_64/gettid,0)
	$(call HL_PERF_ENGINE,syscall-1m,aarch64,$(PERF_WARMUPS),$(PERF_HEAVY_SAMPLES),$(BUILD)/perf/syscall-aarch64,0)
	$(call HL_PERF_ENGINE,syscall-1m,x86_64,$(PERF_WARMUPS),$(PERF_HEAVY_SAMPLES),$(BUILD)/perf/syscall-x86_64,0)
	$(call HL_PERF_ENGINE,fork-stress,aarch64,1,$(PERF_HEAVY_SAMPLES),$(BUILD)/compat/process/aarch64/forkstorm,0)
	$(call HL_PERF_ENGINE,fork-stress,x86_64,1,$(PERF_HEAVY_SAMPLES),$(BUILD)/compat/process/x86_64/forkstorm,0)
	$(call HL_PERF_ENGINE,translation,aarch64,$(PERF_WARMUPS),$(PERF_SAMPLES),$(BUILD)/perf/translate-aarch64,0)
	$(call HL_PERF_ENGINE,translation,x86_64,$(PERF_WARMUPS),$(PERF_SAMPLES),$(BUILD)/perf/translate-x86_64,0)
	$(call HL_PERF_CACHE_MAC,aarch64)
	$(call HL_PERF_CACHE_MAC,x86_64)
	$(call HL_PERF_ENGINE,mmap,aarch64,$(PERF_WARMUPS),$(PERF_OP_SAMPLES),$(BUILD)/perf/mmap-aarch64,0)
	$(call HL_PERF_ENGINE,file,aarch64,$(PERF_WARMUPS),$(PERF_OP_SAMPLES),$(BUILD)/perf/file-aarch64,0)
	$(call HL_PERF_ENGINE,pipe,aarch64,$(PERF_WARMUPS),$(PERF_OP_SAMPLES),$(BUILD)/perf/pipe-aarch64,0)
	$(call HL_PERF_ENGINE,event,aarch64,$(PERF_WARMUPS),$(PERF_OP_SAMPLES),$(BUILD)/perf/event-aarch64,0)
	$(call HL_PERF_ENGINE,ipc-latency,aarch64,$(PERF_WARMUPS),$(PERF_OP_SAMPLES),$(BUILD)/perf/ipc-latency-aarch64,0)
	$(call HL_PERF_ENGINE,ipc-throughput,aarch64,$(PERF_WARMUPS),$(PERF_OP_SAMPLES),$(BUILD)/perf/ipc-throughput-aarch64,0)
	$(call HL_PERF_ENGINE,mmap,x86_64,$(PERF_WARMUPS),$(PERF_OP_SAMPLES),$(BUILD)/perf/mmap-x86_64,0)
	$(call HL_PERF_ENGINE,file,x86_64,$(PERF_WARMUPS),$(PERF_OP_SAMPLES),$(BUILD)/perf/file-x86_64,0)
	$(call HL_PERF_ENGINE,pipe,x86_64,$(PERF_WARMUPS),$(PERF_OP_SAMPLES),$(BUILD)/perf/pipe-x86_64,0)
	$(call HL_PERF_ENGINE,event,x86_64,$(PERF_WARMUPS),$(PERF_OP_SAMPLES),$(BUILD)/perf/event-x86_64,0)
	$(call HL_PERF_ENGINE,ipc-latency,x86_64,$(PERF_WARMUPS),$(PERF_OP_SAMPLES),$(BUILD)/perf/ipc-latency-x86_64,0)
	$(call HL_PERF_ENGINE,ipc-throughput,x86_64,$(PERF_WARMUPS),$(PERF_OP_SAMPLES),$(BUILD)/perf/ipc-throughput-x86_64,0)
	$(MAC) $(abspath $(BUILD)/production/hl-engine-linux-aarch64) $(abspath $(BUILD)/perf/resource-aarch64)
	$(MAC) $(abspath $(BUILD)/production/hl-engine-linux-x86_64) $(abspath $(BUILD)/perf/resource-x86_64)

.PHONY: perf-linux
perf-linux: $(BUILD)/linux-production/hl-engine-linux-aarch64 \
	$(BUILD)/linux-production/hl-engine-linux-x86_64 $(BUILD)/tools/perf-runner $(BUILD)/tools/config-e2e-runner \
	$(BUILD)/e2e/guest-exit-aarch64 $(BUILD)/e2e/guest-exit-x86_64 \
	$(BUILD)/compat/core/workload/aarch64/busyloop $(BUILD)/compat/core/workload/x86_64/busyloop \
	$(BUILD)/compat/syscall/aarch64/gettid $(BUILD)/compat/syscall/x86_64/gettid \
	$(BUILD)/perf/syscall-aarch64 $(BUILD)/perf/syscall-x86_64 \
	$(BUILD)/perf/translate-aarch64 $(BUILD)/perf/translate-x86_64 \
	$(BUILD)/compat/process/aarch64/forkstorm $(BUILD)/compat/process/x86_64/forkstorm \
	$(foreach operation,$(PERF_OPS),$(BUILD)/perf/$(operation)-aarch64 $(BUILD)/perf/$(operation)-x86_64) \
	$(BUILD)/perf/resource-aarch64 $(BUILD)/perf/resource-x86_64
	$(call HL_PERF_LINUX,startup,aarch64,$(PERF_WARMUPS),$(PERF_SAMPLES),$(BUILD)/e2e/guest-exit-aarch64,42)
	$(call HL_PERF_LINUX,startup,x86_64,$(PERF_WARMUPS),$(PERF_SAMPLES),$(BUILD)/e2e/guest-exit-x86_64,42)
	$(call HL_PERF_LINUX,compute,aarch64,$(PERF_WARMUPS),$(PERF_HEAVY_SAMPLES),$(BUILD)/compat/core/workload/aarch64/busyloop,0)
	$(call HL_PERF_LINUX,compute,x86_64,$(PERF_WARMUPS),$(PERF_HEAVY_SAMPLES),$(BUILD)/compat/core/workload/x86_64/busyloop,0)
	$(call HL_PERF_LINUX,syscall-startup,aarch64,$(PERF_WARMUPS),$(PERF_SAMPLES),$(BUILD)/compat/syscall/aarch64/gettid,0)
	$(call HL_PERF_LINUX,syscall-startup,x86_64,$(PERF_WARMUPS),$(PERF_SAMPLES),$(BUILD)/compat/syscall/x86_64/gettid,0)
	$(call HL_PERF_LINUX,syscall-1m,aarch64,$(PERF_WARMUPS),$(PERF_HEAVY_SAMPLES),$(BUILD)/perf/syscall-aarch64,0)
	$(call HL_PERF_LINUX,syscall-1m,x86_64,$(PERF_WARMUPS),$(PERF_HEAVY_SAMPLES),$(BUILD)/perf/syscall-x86_64,0)
	$(call HL_PERF_LINUX,fork-stress,aarch64,1,$(PERF_HEAVY_SAMPLES),$(BUILD)/compat/process/aarch64/forkstorm,0)
	$(call HL_PERF_LINUX,fork-stress,x86_64,1,$(PERF_HEAVY_SAMPLES),$(BUILD)/compat/process/x86_64/forkstorm,0)
	$(call HL_PERF_LINUX,translation,aarch64,$(PERF_WARMUPS),$(PERF_SAMPLES),$(BUILD)/perf/translate-aarch64,0)
	$(call HL_PERF_LINUX,translation,x86_64,$(PERF_WARMUPS),$(PERF_SAMPLES),$(BUILD)/perf/translate-x86_64,0)
	$(call HL_PERF_CACHE_LINUX,aarch64)
	$(call HL_PERF_CACHE_LINUX,x86_64)
	$(call HL_PERF_LINUX,mmap,aarch64,$(PERF_WARMUPS),$(PERF_OP_SAMPLES),$(BUILD)/perf/mmap-aarch64,0)
	$(call HL_PERF_LINUX,file,aarch64,$(PERF_WARMUPS),$(PERF_OP_SAMPLES),$(BUILD)/perf/file-aarch64,0)
	$(call HL_PERF_LINUX,pipe,aarch64,$(PERF_WARMUPS),$(PERF_OP_SAMPLES),$(BUILD)/perf/pipe-aarch64,0)
	$(call HL_PERF_LINUX,event,aarch64,$(PERF_WARMUPS),$(PERF_OP_SAMPLES),$(BUILD)/perf/event-aarch64,0)
	$(call HL_PERF_LINUX,ipc-latency,aarch64,$(PERF_WARMUPS),$(PERF_OP_SAMPLES),$(BUILD)/perf/ipc-latency-aarch64,0)
	$(call HL_PERF_LINUX,ipc-throughput,aarch64,$(PERF_WARMUPS),$(PERF_OP_SAMPLES),$(BUILD)/perf/ipc-throughput-aarch64,0)
	$(call HL_PERF_LINUX,mmap,x86_64,$(PERF_WARMUPS),$(PERF_OP_SAMPLES),$(BUILD)/perf/mmap-x86_64,0)
	$(call HL_PERF_LINUX,file,x86_64,$(PERF_WARMUPS),$(PERF_OP_SAMPLES),$(BUILD)/perf/file-x86_64,0)
	$(call HL_PERF_LINUX,pipe,x86_64,$(PERF_WARMUPS),$(PERF_OP_SAMPLES),$(BUILD)/perf/pipe-x86_64,0)
	$(call HL_PERF_LINUX,event,x86_64,$(PERF_WARMUPS),$(PERF_OP_SAMPLES),$(BUILD)/perf/event-x86_64,0)
	$(call HL_PERF_LINUX,ipc-latency,x86_64,$(PERF_WARMUPS),$(PERF_OP_SAMPLES),$(BUILD)/perf/ipc-latency-x86_64,0)
	$(call HL_PERF_LINUX,ipc-throughput,x86_64,$(PERF_WARMUPS),$(PERF_OP_SAMPLES),$(BUILD)/perf/ipc-throughput-x86_64,0)
	$(BUILD)/linux-production/hl-engine-linux-aarch64 $(abspath $(BUILD)/perf/resource-aarch64)
	$(BUILD)/linux-production/hl-engine-linux-x86_64 $(abspath $(BUILD)/perf/resource-x86_64)

# Native comparison is meaningful only when the host can execute the AArch64 Linux fixtures directly.
perf-native-aarch64: $(BUILD)/tools/perf-runner $(BUILD)/e2e/guest-exit-aarch64 \
	$(BUILD)/compat/core/workload/aarch64/busyloop $(BUILD)/compat/syscall/aarch64/gettid \
	$(BUILD)/perf/syscall-aarch64 $(BUILD)/compat/process/aarch64/forkstorm \
	$(foreach operation,$(PERF_OPS),$(BUILD)/perf/$(operation)-aarch64) $(BUILD)/perf/resource-aarch64
	@test "$$(uname -s)" = Linux && test "$$(uname -m)" = aarch64 || \
		{ echo 'perf-native-aarch64 requires a Linux AArch64 host' >&2; exit 2; }
	$(call HL_PERF_NATIVE,startup,$(PERF_WARMUPS),$(PERF_SAMPLES),$(BUILD)/e2e/guest-exit-aarch64,42)
	$(call HL_PERF_NATIVE,compute,$(PERF_WARMUPS),$(PERF_HEAVY_SAMPLES),$(BUILD)/compat/core/workload/aarch64/busyloop,0)
	$(call HL_PERF_NATIVE,syscall-startup,$(PERF_WARMUPS),$(PERF_SAMPLES),$(BUILD)/compat/syscall/aarch64/gettid,0)
	$(call HL_PERF_NATIVE,syscall-1m,$(PERF_WARMUPS),$(PERF_HEAVY_SAMPLES),$(BUILD)/perf/syscall-aarch64,0)
	$(call HL_PERF_NATIVE,fork-stress,1,$(PERF_HEAVY_SAMPLES),$(BUILD)/compat/process/aarch64/forkstorm,0)
	$(call HL_PERF_NATIVE,mmap,$(PERF_WARMUPS),$(PERF_OP_SAMPLES),$(BUILD)/perf/mmap-aarch64,0)
	$(call HL_PERF_NATIVE,file,$(PERF_WARMUPS),$(PERF_OP_SAMPLES),$(BUILD)/perf/file-aarch64,0)
	$(call HL_PERF_NATIVE,pipe,$(PERF_WARMUPS),$(PERF_OP_SAMPLES),$(BUILD)/perf/pipe-aarch64,0)
	$(call HL_PERF_NATIVE,event,$(PERF_WARMUPS),$(PERF_OP_SAMPLES),$(BUILD)/perf/event-aarch64,0)
	$(call HL_PERF_NATIVE,ipc-latency,$(PERF_WARMUPS),$(PERF_OP_SAMPLES),$(BUILD)/perf/ipc-latency-aarch64,0)
	$(call HL_PERF_NATIVE,ipc-throughput,$(PERF_WARMUPS),$(PERF_OP_SAMPLES),$(BUILD)/perf/ipc-throughput-aarch64,0)
	$(BUILD)/perf/resource-aarch64

MAC_EXCLUDED_UNIT_TARGETS := run-unit-directory run-unit-directory_services run-unit-eventfd_fork run-unit-linux_fork run-unit-native \
	run-unit-pipe_linux run-unit-private run-unit-process run-unit-range run-unit-resolve_services run-unit-system
ifeq ($(HOST),macos)
unit: $(filter-out $(MAC_EXCLUDED_UNIT_TARGETS),$(UNIT_RUN_TARGETS)) test-macos
else
unit: $(UNIT_RUN_TARGETS) $(LINUX_HOST_TEST) test-native-capacity
endif

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

$(BUILD)/tests/macos: tests/unit/macos.c src/host/macos/host.c src/host/macos/system.c src/host/sync.c src/host/resolve.c src/core/host_services.c \
	src/core/log.c src/host/clock.c src/host/file.c src/host/private.c include/hl/macos.h include/hl/host_services.h
	@mkdir -p $(@D)
	$(MAC) clang -Iinclude -Itests/unit $(ENGINE_CFLAGS) tests/unit/macos.c \
	src/host/macos/host.c src/host/macos/system.c src/host/sync.c src/host/resolve.c src/core/host_services.c src/core/log.c src/host/clock.c \
		src/host/file.c src/host/private.c -o $@

.PHONY: run-unit-macos-destroy
run-unit-macos-destroy: $(BUILD)/tests/macos-destroy
	$(MAC) $<

$(BUILD)/tests/macos-destroy: tests/unit/test_macos_destroy.c src/host/macos/host.c src/host/macos/system.c \
	src/host/sync.c src/host/resolve.c src/core/host_services.c src/core/log.c src/host/clock.c src/host/file.c \
	src/host/private.c
	@mkdir -p $(@D)
	$(MAC) clang -Iinclude -Itests/unit $(ENGINE_CFLAGS) $^ -o $@

$(BUILD)/tests/range-macos: tests/unit/test_range.c src/host/range.c src/host/macos/range.c
	@mkdir -p $(@D)
	$(MAC) clang -Iinclude -Itests/unit $(ENGINE_CFLAGS) $^ -o $@

$(BUILD)/tests/system-macos: tests/unit/test_system.c src/host/macos/system.c src/host/private.c
	@mkdir -p $(@D)
	$(MAC) clang -Iinclude -Itests/unit $(ENGINE_CFLAGS) $^ -o $@

$(BUILD)/tests/private-macos: tests/unit/test_private.c src/host/private.c src/host/macos/system.c
	@mkdir -p $(@D)
	$(MAC) clang -Iinclude -Itests/unit $(ENGINE_CFLAGS) $^ -o $@

$(BUILD)/tests/child-macos: tests/unit/test_child.c src/host/child.c
	@mkdir -p $(@D)
	$(MAC) clang -Iinclude -Itests/unit $(ENGINE_CFLAGS) $^ -o $@

$(BUILD)/tests/directory-macos: tests/unit/test_directory.c src/host/macos/directory.c
	@mkdir -p $(@D)
	$(MAC) clang -Iinclude -Itests/unit $(ENGINE_CFLAGS) $^ -o $@

$(BUILD)/tests/directory-services-macos: tests/unit/test_directory_services.c $(MAC_LIBS)
	@mkdir -p $(@D)
	$(MAC) clang $(CPPFLAGS) -DHL_TEST_MACOS=1 -Itests/unit $(ENGINE_CFLAGS) $< $(MAC_LIBS) -o $@

$(BUILD)/tests/process-macos: tests/unit/test_process.c src/host/macos/process.c
	@mkdir -p $(@D)
	$(MAC) clang -Iinclude -Itests/unit $(ENGINE_CFLAGS) $^ -o $@

$(BUILD)/tests/native-macos: tests/unit/test_native.c $(MAC_LIBS)
	@mkdir -p $(@D)
	$(MAC) clang $(CPPFLAGS) -Itests/unit $(ENGINE_CFLAGS) $< $(MAC_LIBS) -o $@

$(BUILD)/tests/native-capacity-macos: tests/unit/test_native_capacity.c $(MAC_LIBS)
	@mkdir -p $(@D)
	$(MAC) clang $(CPPFLAGS) -Itests/unit $(ENGINE_CFLAGS) $< $(MAC_LIBS) -o $@

test-native-capacity-macos: $(BUILD)/tests/native-capacity-macos
	$(MAC) $(abspath $(BUILD)/tests/native-capacity-macos)

$(BUILD)/tests/resolve-services-macos: tests/unit/test_resolve_services.c $(BUILD)/mac/lib/libhl-host-macos.a
	@mkdir -p $(@D)
	$(MAC) clang $(CPPFLAGS) -DHL_TEST_HOST_MACOS=1 -Itests/unit $(ENGINE_CFLAGS) $< \
		$(BUILD)/mac/lib/libhl-host-macos.a -o $@

test-macos: $(BUILD)/tests/macos $(BUILD)/tests/child-macos $(BUILD)/tests/directory-macos $(BUILD)/tests/directory-services-macos $(BUILD)/tests/private-macos $(BUILD)/tests/process-macos $(BUILD)/tests/range-macos $(BUILD)/tests/system-macos $(BUILD)/tests/native-macos $(BUILD)/tests/native-capacity-macos $(BUILD)/tests/resolve-services-macos
	$(MAC) $(abspath $<)
	$(MAC) $(abspath $(BUILD)/tests/child-macos)
	$(MAC) $(abspath $(BUILD)/tests/directory-macos)
	$(MAC) $(abspath $(BUILD)/tests/directory-services-macos)
	$(MAC) $(abspath $(BUILD)/tests/private-macos)
	$(MAC) $(abspath $(BUILD)/tests/process-macos)
	$(MAC) $(abspath $(BUILD)/tests/range-macos)
	$(MAC) $(abspath $(BUILD)/tests/system-macos)
	$(MAC) $(abspath $(BUILD)/tests/native-macos)
	$(MAC) $(abspath $(BUILD)/tests/native-capacity-macos)
	$(MAC) $(abspath $(BUILD)/tests/resolve-services-macos)

$(BUILD)/tests/test-log-debug: tests/unit/test_log.c src/core/log.c
	@mkdir -p $(@D)
	$(CC) $(filter-out -DHL_ENABLE_LOGGING=%,$(CPPFLAGS)) -DHL_ENABLE_LOGGING=1 -Itests/unit $(ENGINE_CFLAGS) $^ -o $@

test-debug-log: $(BUILD)/tests/test-log-debug
	$<

$(BUILD)/tests/test-fatal-debug: tests/unit/test_fatal.c src/core/fatal.c
	@mkdir -p $(@D)
	$(CC) $(filter-out -DHL_ENABLE_LOGGING=%,$(CPPFLAGS)) -DHL_ENABLE_LOGGING=1 -Itests/unit $(ENGINE_CFLAGS) $^ -o $@

test-debug-fatal: $(BUILD)/tests/test-fatal-debug
	$<

compat-build: $(FIXTURE_BINS)

compat-native: $(NATIVE_SMOKE_BINS) $(BUILD)/tools/compat-runner
	$(BUILD)/tools/compat-runner $(NATIVE_SMOKE_BINS)

test: unit compat-native

# Keep sanitizer artifacts isolated from release objects.  The recursive invocation exercises the same
# authoritative C unit graph, including both the Linux ABI and selected native host provider.
sanitize:
	$(MAKE) BUILD=$(SANITIZE_BUILD) CFLAGS='-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined' unit

format:
	$(CLANG_FORMAT) -i $(PORTABLE_SOURCES) $(LINUX_HOST_SOURCES) $(MACOS_HOST_SOURCES) $(PRIVATE_HEADERS) src/runner/main.c include/hl/*.h tests/unit/*.c tests/unit/*.h tools/*.c

format-check:
	$(CLANG_FORMAT) --dry-run --Werror $(PORTABLE_SOURCES) $(LINUX_HOST_SOURCES) $(MACOS_HOST_SOURCES) $(PRIVATE_HEADERS) src/runner/main.c include/hl/*.h tests/unit/*.c tests/unit/*.h tools/*.c

clean:
	rm -rf $(BUILD)

help:
	@echo 'make all           build pure-C static libraries and runner'
	@echo 'make linux-compile compile/link the independent libraries, Linux provider, and runner'
	@echo 'make test          unit, domain-boundary, and native compatibility smoke tests'
	@echo 'make sanitize      run the complete C unit graph under ASan and UBSan'
	@echo 'make compat-build  compile every Linux behavior fixture'
	@echo 'make e2e-compat    build/codesign production engines and execute both guest ISAs'
	@echo 'make perf-compat   report repeated end-to-end baseline distributions in C'
	@echo 'make perf-macos    measure macOS-host startup, compute, syscall, fork, OS-operation, and resource baselines'
	@echo 'make perf-native-aarch64  measure matching native and resource fixtures on Linux AArch64'
	@echo 'make format-check  enforce the repository clang-format policy'

# Compiler-generated prerequisites keep standalone objects synchronized with public and private headers.
# Missing .d files are expected on a clean tree; -MP leaves harmless stubs for headers removed later.
-include $(DEPENDENCY_FILES)
