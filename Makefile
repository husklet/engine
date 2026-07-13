CC ?= cc
AR ?= ar
CLANG_FORMAT ?= clang-format
BUILD ?= build

CPPFLAGS := -Iinclude
CFLAGS ?= -O2 -g
WARNINGS := -std=c11 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wstrict-prototypes -Wmissing-prototypes
ENGINE_CFLAGS := $(CFLAGS) $(WARNINGS) -fvisibility=hidden

CORE_SOURCES := src/core/config.c src/core/engine.c src/core/host_services.c
IR_SOURCES := src/translator/ir/ir.c
LINUX_ABI_SOURCES := src/linux_abi/linux_abi.c
FAKE_HOST_SOURCES := src/host/fake/fake_host.c
PORTABLE_SOURCES := $(CORE_SOURCES) $(IR_SOURCES) $(LINUX_ABI_SOURCES) $(FAKE_HOST_SOURCES)
OBJECTS := $(PORTABLE_SOURCES:%.c=$(BUILD)/%.o)

UNIT_NAMES := config host_services ir linux_abi engine
UNIT_BINS := $(UNIT_NAMES:%=$(BUILD)/tests/test_%)
UNIT_RUN_TARGETS := $(UNIT_NAMES:%=run-unit-%)

FIXTURE_SOURCES := $(sort $(wildcard tests/compat/fixtures/*.c))
FIXTURE_BINS := $(FIXTURE_SOURCES:tests/compat/fixtures/%.c=$(BUILD)/fixtures/%)
NATIVE_SMOKE := atomics clockelapsed epoll epoll_edge eventfd eventfd_sema forkwait mmapanon mmapshared statx_agree timerfd
NATIVE_SMOKE_BINS := $(NATIVE_SMOKE:%=$(BUILD)/fixtures/%)

.PHONY: all clean test unit $(UNIT_RUN_TARGETS) compat-build compat-native check-domains format format-check help

all: $(BUILD)/lib/libhl-engine.a $(BUILD)/lib/libhl-host-fake.a $(BUILD)/bin/hl-engine-runner

$(BUILD)/src/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(ENGINE_CFLAGS) -c $< -o $@

$(BUILD)/lib/libhl-engine.a: $(filter-out $(BUILD)/src/host/fake/fake_host.o,$(OBJECTS))
	@mkdir -p $(@D)
	$(AR) rcs $@ $^

$(BUILD)/lib/libhl-host-fake.a: $(BUILD)/src/host/fake/fake_host.o
	@mkdir -p $(@D)
	$(AR) rcs $@ $^

$(BUILD)/bin/hl-engine-runner: src/runner/main.c $(BUILD)/lib/libhl-engine.a
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(ENGINE_CFLAGS) $< $(BUILD)/lib/libhl-engine.a -o $@

$(BUILD)/tests/test_%: tests/unit/test_%.c $(BUILD)/lib/libhl-engine.a $(BUILD)/lib/libhl-host-fake.a
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -Itests/unit $(ENGINE_CFLAGS) $< $(BUILD)/lib/libhl-engine.a \
		$(BUILD)/lib/libhl-host-fake.a -o $@

$(BUILD)/fixtures/%: tests/compat/fixtures/%.c
	@mkdir -p $(@D)
	$(CC) -O2 -g -std=gnu11 -Wall -Wextra $< -pthread -o $@

$(BUILD)/tools/check-domains: tools/check_domains.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(WARNINGS) $< -o $@

$(BUILD)/tools/compat-runner: tools/compat_runner.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(WARNINGS) $< -o $@

unit: $(UNIT_RUN_TARGETS)

define HL_UNIT_RULE
run-unit-$(1): $(BUILD)/tests/test_$(1)
	$$<
endef

$(foreach test,$(UNIT_NAMES),$(eval $(call HL_UNIT_RULE,$(test))))

compat-build: $(FIXTURE_BINS)

compat-native: $(NATIVE_SMOKE_BINS) $(BUILD)/tools/compat-runner
	$(BUILD)/tools/compat-runner $(NATIVE_SMOKE_BINS)

check-domains: $(BUILD)/tools/check-domains
	$(BUILD)/tools/check-domains $(PORTABLE_SOURCES) include/hl/*.h

test: unit check-domains compat-native

format:
	$(CLANG_FORMAT) -i $(PORTABLE_SOURCES) src/runner/main.c include/hl/*.h tests/unit/*.c tests/unit/*.h tools/*.c

format-check:
	$(CLANG_FORMAT) --dry-run --Werror $(PORTABLE_SOURCES) src/runner/main.c include/hl/*.h tests/unit/*.c tests/unit/*.h tools/*.c

clean:
	rm -rf $(BUILD)

help:
	@echo 'make all           build pure-C static libraries and runner'
	@echo 'make test          unit, domain-boundary, and native compatibility smoke tests'
	@echo 'make compat-build  compile every imported compatibility fixture'
	@echo 'make format-check  enforce the repository clang-format policy'
