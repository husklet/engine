CC ?= cc
AR ?= ar
CLANG_FORMAT ?= clang-format
BUILD ?= build
HOST ?= linux
DEBUG ?= 0
MAC ?= mac
AARCH64_LINUX_CC ?= aarch64-linux-gnu-gcc
X86_64_LINUX_CC ?= x86_64-linux-gnu-gcc

CPPFLAGS := -Iinclude -DHL_ENABLE_LOGGING=$(DEBUG)
CFLAGS ?= -O2 -g
WARNINGS := -std=c11 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wstrict-prototypes -Wmissing-prototypes
ENGINE_CFLAGS := $(CFLAGS) $(WARNINGS) -fvisibility=hidden
PRIVATE_HEADERS := src/translator/host/aarch64/aarch64_codegen.h src/translator/host/x86_64/x86_64_codegen.h

# Production engines are unity translation units: their target .c files textually include the engine,
# translator, and Linux-personality implementation. Make cannot discover those nested includes from the
# compiler command, so conservatively track the complete production C/header tree plus public HL headers.
# This GNU Make-only recursive wildcard keeps newly added included files in the dependency closure without
# a generated list or a clean build.
rwildcard = $(foreach entry,$(wildcard $1*),$(call rwildcard,$(entry)/,$2) $(filter $(subst *,%,$2),$(entry)))
PRODUCTION_UNITY_DEPS := $(sort $(call rwildcard,src/production/,*.c) \
	$(call rwildcard,src/production/,*.h) $(call rwildcard,include/hl/,*.h))

CORE_SOURCES := src/core/config.c src/core/engine.c src/core/host_services.c src/core/log.c
IR_SOURCES := src/translator/codegen.c src/translator/host/aarch64/codegen.c src/translator/host/x86_64/codegen.c src/translator/ir/interpreter.c \
	src/translator/ir/ir.c
LINUX_ABI_SOURCES := src/linux_abi/linux_abi.c src/linux_abi/stat.c
FAKE_HOST_SOURCES := src/host/fake/host.c
MACOS_HOST_SOURCES := src/host/macos/host.c
PORTABLE_SOURCES := $(CORE_SOURCES) $(IR_SOURCES) $(LINUX_ABI_SOURCES) $(FAKE_HOST_SOURCES)
CORE_OBJECTS := $(CORE_SOURCES:%.c=$(BUILD)/%.o)
TRANSLATOR_OBJECTS := $(IR_SOURCES:%.c=$(BUILD)/%.o)
LINUX_ABI_OBJECTS := $(LINUX_ABI_SOURCES:%.c=$(BUILD)/%.o)
FAKE_HOST_OBJECTS := $(FAKE_HOST_SOURCES:%.c=$(BUILD)/%.o)

ifeq ($(HOST),linux)
LINUX_HOST_SOURCES := src/host/linux/host.c
LINUX_HOST_OBJECTS := $(LINUX_HOST_SOURCES:%.c=$(BUILD)/%.o)
LINUX_HOST_PRODUCTS := $(BUILD)/lib/libhl-host-linux.a
LINUX_HOST_TEST := run-unit-host_linux
endif

UNIT_NAMES := codegen config host_services ir linux_abi stat engine log options readonly xattr_cache
UNIT_BINS := $(UNIT_NAMES:%=$(BUILD)/tests/test_%)
UNIT_RUN_TARGETS := $(UNIT_NAMES:%=run-unit-%)

FIXTURE_SOURCES := $(sort $(wildcard tests/compat/fixtures/*.c))
FIXTURE_BINS := $(FIXTURE_SOURCES:tests/compat/fixtures/%.c=$(BUILD)/fixtures/%)
NATIVE_SMOKE := atomics clockelapsed epoll epoll_edge eventfd eventfd_sema forkwait mmapanon mmapshared statx_agree sysv_ipc timerfd
NATIVE_SMOKE_BINS := $(NATIVE_SMOKE:%=$(BUILD)/fixtures/%)
E2E_CASES := atomics epoll_edge eventfd forkwait sysv_ipc
E2E_CASE_BINS := $(E2E_CASES:%=$(BUILD)/e2e/%-aarch64) $(E2E_CASES:%=$(BUILD)/e2e/%-x86_64)
E2E_CASE_RUNS := $(E2E_CASES:%=run-e2e-compat-%)

.PHONY: all clean test unit $(UNIT_RUN_TARGETS) test-debug-log test-macos-host compat-build compat-native compat-engines e2e-compat \
	$(E2E_CASE_RUNS) perf-compat check-domains format format-check help

all: $(BUILD)/lib/libhl-engine.a $(BUILD)/lib/libhl-translator.a $(BUILD)/lib/libhl-linux-abi.a \
	$(BUILD)/lib/libhl-host-fake.a $(LINUX_HOST_PRODUCTS) $(BUILD)/bin/hl-engine-runner

$(BUILD)/src/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(ENGINE_CFLAGS) -c $< -o $@

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

$(BUILD)/tests/test_xattr_cache: tests/unit/test_xattr_cache.c src/production/os/linux/container/xattr_cache.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(ENGINE_CFLAGS) $^ -o $@

$(BUILD)/tests/test_readonly: tests/unit/test_readonly.c src/production/os/linux/container/readonly/table.c
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

$(BUILD)/e2e/%-aarch64: tests/compat/fixtures/%.c
	@mkdir -p $(@D)
	$(AARCH64_LINUX_CC) -O2 -static -pthread $< -o $@

$(BUILD)/e2e/%-x86_64: tests/compat/fixtures/%.c
	@mkdir -p $(@D)
	$(X86_64_LINUX_CC) -O2 -static -pthread $< -o $@

$(BUILD)/production/hl-engine-linux-aarch64: src/production/targets/linux_aarch64.c $(PRODUCTION_UNITY_DEPS) \
	src/core/config.c src/core/host_services.c src/core/log.c src/host/macos/host.c \
	packaging/macos/jit.entitlements
	@mkdir -p $(@D)
	$(MAC) clang -Iinclude -DHL_ENABLE_LOGGING=$(DEBUG) -O2 -framework IOSurface -framework CoreFoundation -o $@ $< src/core/config.c \
		src/production/os/linux/container/xattr_cache.c \
		src/production/os/linux/container/readonly/table.c \
		src/core/host_services.c src/core/log.c src/host/macos/host.c
	$(MAC) codesign -s - --entitlements packaging/macos/jit.entitlements -f $@

$(BUILD)/production/hl-engine-linux-x86_64: src/production/targets/linux_x86_64.c $(PRODUCTION_UNITY_DEPS) \
	src/core/config.c src/core/host_services.c src/core/log.c src/host/macos/host.c \
	packaging/macos/jit.entitlements
	@mkdir -p $(@D)
	$(MAC) clang -Iinclude -DHL_ENABLE_LOGGING=$(DEBUG) -O2 -framework IOSurface -framework CoreFoundation -o $@ $< src/core/config.c \
		src/production/os/linux/container/xattr_cache.c \
		src/production/os/linux/container/readonly/table.c \
		src/core/host_services.c src/core/log.c src/host/macos/host.c
	$(MAC) codesign -s - --entitlements packaging/macos/jit.entitlements -f $@

compat-engines: $(BUILD)/production/hl-engine-linux-aarch64 $(BUILD)/production/hl-engine-linux-x86_64

$(BUILD)/tools/lifecycle-aarch64: tools/lifecycle_e2e_runner.c src/production/targets/linux_aarch64.c \
	$(PRODUCTION_UNITY_DEPS) src/core/config.c src/core/engine.c src/core/host_services.c src/core/log.c \
	src/host/macos/host.c packaging/macos/jit.entitlements
	@mkdir -p $(@D)
	$(MAC) clang -Iinclude -DHL_ENABLE_LOGGING=$(DEBUG) -DHL_ENGINE_NO_MAIN=1 \
		-DHL_TEST_GUEST_ISA=HL_GUEST_ISA_AARCH64 -DHL_PRODUCTION_GUEST_ISA=HL_GUEST_ISA_AARCH64 -O2 -framework IOSurface -framework CoreFoundation \
		-o $@ tools/lifecycle_e2e_runner.c src/production/targets/linux_aarch64.c src/core/config.c \
		src/production/os/linux/container/xattr_cache.c \
		src/production/os/linux/container/readonly/table.c \
		src/production/os/lifecycle_adapter.c \
		src/core/engine.c src/core/host_services.c src/core/log.c src/host/macos/host.c
	$(MAC) codesign -s - --entitlements packaging/macos/jit.entitlements -f $@

$(BUILD)/tools/lifecycle-x86_64: tools/lifecycle_e2e_runner.c src/production/targets/linux_x86_64.c \
	$(PRODUCTION_UNITY_DEPS) src/core/config.c src/core/engine.c src/core/host_services.c src/core/log.c \
	src/host/macos/host.c packaging/macos/jit.entitlements
	@mkdir -p $(@D)
	$(MAC) clang -Iinclude -DHL_ENABLE_LOGGING=$(DEBUG) -DHL_ENGINE_NO_MAIN=1 \
		-DHL_TEST_GUEST_ISA=HL_GUEST_ISA_X86_64 -DHL_PRODUCTION_GUEST_ISA=HL_GUEST_ISA_X86_64 -O2 -framework IOSurface -framework CoreFoundation \
		-o $@ tools/lifecycle_e2e_runner.c src/production/targets/linux_x86_64.c src/core/config.c \
		src/production/os/linux/container/xattr_cache.c \
		src/production/os/linux/container/readonly/table.c \
		src/production/os/lifecycle_adapter.c \
		src/core/engine.c src/core/host_services.c src/core/log.c src/host/macos/host.c
	$(MAC) codesign -s - --entitlements packaging/macos/jit.entitlements -f $@

e2e-compat: test-macos-host compat-engines $(BUILD)/tools/lifecycle-aarch64 $(BUILD)/tools/lifecycle-x86_64 \
	$(BUILD)/e2e/guest-exit-aarch64 $(BUILD)/e2e/guest-exit-x86_64 \
	$(BUILD)/e2e/guest-spin-aarch64 $(BUILD)/e2e/guest-spin-x86_64 \
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
	$(MAC) $(abspath $(BUILD)/tools/lifecycle-aarch64) --force-stop \
		$(abspath $(BUILD)/e2e/guest-spin-aarch64)
	$(MAC) $(abspath $(BUILD)/tools/lifecycle-x86_64) --force-stop \
		$(abspath $(BUILD)/e2e/guest-spin-x86_64)

define HL_E2E_CASE_RULE
run-e2e-compat-$(1): $(BUILD)/e2e/$(1)-aarch64 $(BUILD)/e2e/$(1)-x86_64 $(BUILD)/tools/e2e-runner \
	compat-engines
	$(BUILD)/tools/e2e-runner $(MAC) $(abspath $(BUILD)/production/hl-engine-linux-aarch64) \
		$(abspath $(BUILD)/e2e/$(1)-aarch64) 0
	$(BUILD)/tools/e2e-runner $(MAC) $(abspath $(BUILD)/production/hl-engine-linux-x86_64) \
		$(abspath $(BUILD)/e2e/$(1)-x86_64) 0
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

$(BUILD)/tools/config-e2e-runner: tools/config_e2e_runner.c include/hl/config.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $< -o $@

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

run-unit-host_linux: $(BUILD)/tests/test_host_linux
	$<

$(BUILD)/tests/test_host_linux: tests/unit/linux.c $(BUILD)/lib/libhl-engine.a $(BUILD)/lib/libhl-host-linux.a
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -Itests/unit $(ENGINE_CFLAGS) $< $(BUILD)/lib/libhl-engine.a \
		$(BUILD)/lib/libhl-host-linux.a -pthread -o $@

$(BUILD)/tests/test-host-macos: tests/unit/macos.c src/host/macos/host.c src/core/host_services.c \
	src/core/log.c include/hl/macos.h include/hl/host_services.h
	@mkdir -p $(@D)
	$(MAC) clang -Iinclude -Itests/unit $(ENGINE_CFLAGS) tests/unit/macos.c \
		src/host/macos/host.c src/core/host_services.c src/core/log.c -o $@

test-macos-host: $(BUILD)/tests/test-host-macos
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
