cmake_minimum_required(VERSION 3.20)
project(hl_engine VERSION 0.1.0 LANGUAGES C)

find_package(Threads REQUIRED)

option(HL_ENABLE_LOGGING "Compile tagged engine logging into the binaries" OFF)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_C_VISIBILITY_PRESET hidden)

add_library(hl-translator STATIC
    src/translator/codegen.c
    src/translator/identity.c
    src/translator/host/aarch64/codegen.c
    src/translator/host/x86_64/codegen.c
    src/translator/ir/interpreter.c
    src/translator/ir/ir.c)
target_include_directories(hl-translator PUBLIC include)

add_library(hl-linux-abi STATIC src/linux_abi/linux_abi.c src/linux_abi/stat.c)
target_include_directories(hl-linux-abi PUBLIC include)

add_library(hl-engine STATIC src/core/config.c src/core/engine.c src/core/host_services.c src/core/log.c)
target_include_directories(hl-engine PUBLIC include)
target_compile_definitions(hl-engine PUBLIC HL_ENABLE_LOGGING=$<BOOL:${HL_ENABLE_LOGGING}>)
target_link_libraries(hl-engine PUBLIC hl-translator hl-linux-abi)

add_library(hl-production-xattr-cache OBJECT src/production/os/linux/container/xattr_cache.c)
target_include_directories(hl-production-xattr-cache PRIVATE src/production/os/linux/container)
target_compile_options(hl-production-xattr-cache PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow
                                                         -Wstrict-prototypes -Wmissing-prototypes)

add_library(hl-production-readonly OBJECT src/production/os/linux/container/readonly/table.c)
target_include_directories(hl-production-readonly PRIVATE src/production/os/linux/container/readonly)
target_compile_options(hl-production-readonly PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow
                                                     -Wstrict-prototypes -Wmissing-prototypes)

add_library(hl-production-limits OBJECT src/production/os/linux/container/limits/table.c)
target_include_directories(hl-production-limits PRIVATE src/production/os/linux/container/limits)
target_compile_options(hl-production-limits PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow
                                                   -Wstrict-prototypes -Wmissing-prototypes)

add_library(hl-production-reloc OBJECT src/production/translate/reloc.c)
target_include_directories(hl-production-reloc PRIVATE src/production/translate)
target_compile_options(hl-production-reloc PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow
                                                  -Wstrict-prototypes -Wmissing-prototypes)

add_library(hl-production-digest OBJECT src/production/translate/digest.c)
target_include_directories(hl-production-digest PRIVATE src/production/translate)
target_compile_options(hl-production-digest PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow
                                                   -Wstrict-prototypes -Wmissing-prototypes)
add_library(hl-production-window OBJECT src/production/translate/window.c)
target_include_directories(hl-production-window PRIVATE src/production/translate)
target_compile_options(hl-production-window PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow
                                                   -Wstrict-prototypes -Wmissing-prototypes)
add_library(hl-production-device OBJECT src/production/os/linux/syscall/device.c)
target_include_directories(hl-production-device PRIVATE src/production/os/linux/syscall)
target_compile_options(hl-production-device PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow
                                                   -Wstrict-prototypes -Wmissing-prototypes)
add_library(hl-linux-affinity OBJECT src/linux_abi/affinity.c)
target_include_directories(hl-linux-affinity PRIVATE src/linux_abi)
target_compile_options(hl-linux-affinity PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wstrict-prototypes
                                                 -Wmissing-prototypes)

add_library(hl-production-clock OBJECT src/production/host/clock.c)
add_library(hl-host-file OBJECT src/host/file.c)

add_library(hl-production-errno OBJECT src/production/os/linux/syscall/errno.c)
target_include_directories(hl-production-errno PRIVATE src/production/os/linux/syscall)
target_compile_options(hl-production-errno PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow
                                                  -Wstrict-prototypes -Wmissing-prototypes)
add_library(hl-production-number OBJECT src/production/os/linux/syscall/number.c)
target_include_directories(hl-production-number PRIVATE src/production/os/linux/syscall)
target_compile_options(hl-production-number PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow
                                                   -Wstrict-prototypes -Wmissing-prototypes)
target_include_directories(hl-production-clock PRIVATE include src/production/host)
target_compile_options(hl-production-clock PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow
                                                  -Wstrict-prototypes -Wmissing-prototypes)
target_include_directories(hl-host-file PRIVATE include src/host)
target_compile_options(hl-host-file PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wstrict-prototypes
                                            -Wmissing-prototypes)

foreach(target_name hl-translator hl-linux-abi hl-engine)
    target_compile_options(${target_name} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wstrict-prototypes
                                                  -Wmissing-prototypes)
endforeach()

add_library(hl-host-fake STATIC src/host/fake/host.c)
target_include_directories(hl-host-fake PUBLIC include)
target_compile_options(hl-host-fake PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wstrict-prototypes
                                            -Wmissing-prototypes)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_library(hl-host-linux STATIC src/host/linux/host.c)
    target_include_directories(hl-host-linux PUBLIC include)
    target_link_libraries(hl-host-linux PUBLIC Threads::Threads)
    target_compile_options(hl-host-linux PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wstrict-prototypes
                                                 -Wmissing-prototypes)
endif()

if(APPLE)
    add_library(hl-host-macos STATIC src/host/macos/host.c)
    target_include_directories(hl-host-macos PUBLIC include)
    target_compile_options(hl-host-macos PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wstrict-prototypes
                                                 -Wmissing-prototypes)
endif()

add_executable(hl-engine-runner src/runner/main.c)
target_link_libraries(hl-engine-runner PRIVATE hl-engine)

include(CTest)
if(BUILD_TESTING)
    foreach(test_name codegen config host_services identity ir linux_abi stat engine log options)
        add_executable(test-${test_name} tests/unit/test_${test_name}.c)
        target_include_directories(test-${test_name} PRIVATE include tests/unit)
        target_link_libraries(test-${test_name} PRIVATE hl-engine hl-host-fake)
        if(test_name STREQUAL "linux_abi" OR test_name STREQUAL "stat")
            target_link_libraries(test-${test_name} PRIVATE hl-linux-abi)
        endif()
        add_test(NAME ${test_name} COMMAND test-${test_name})
    endforeach()
    add_executable(test-readonly tests/unit/test_readonly.c $<TARGET_OBJECTS:hl-production-readonly>)
    target_include_directories(test-readonly PRIVATE include tests/unit)
    add_test(NAME readonly COMMAND test-readonly)
    add_executable(test-limits tests/unit/test_limits.c $<TARGET_OBJECTS:hl-production-limits>)
    target_include_directories(test-limits PRIVATE include tests/unit)
    target_link_libraries(test-limits PRIVATE Threads::Threads)
    add_test(NAME limits COMMAND test-limits)
    add_executable(test-reloc tests/unit/test_reloc.c $<TARGET_OBJECTS:hl-production-reloc>)
    target_include_directories(test-reloc PRIVATE include tests/unit src/production/translate)
    add_test(NAME reloc COMMAND test-reloc)
    add_executable(test-digest tests/unit/test_digest.c $<TARGET_OBJECTS:hl-production-digest>)
    target_include_directories(test-digest PRIVATE include tests/unit src/production/translate)
    add_test(NAME digest COMMAND test-digest)
    add_executable(test-window tests/unit/test_window.c $<TARGET_OBJECTS:hl-production-window>)
    target_include_directories(test-window PRIVATE include tests/unit src/production/translate)
    add_test(NAME window COMMAND test-window)
    add_executable(test-device tests/unit/test_device.c $<TARGET_OBJECTS:hl-production-device>)
    target_include_directories(test-device PRIVATE include tests/unit)
    add_test(NAME device COMMAND test-device)
    add_executable(test-affinity tests/unit/test_affinity.c $<TARGET_OBJECTS:hl-linux-affinity>)
    target_include_directories(test-affinity PRIVATE include tests/unit)
    add_test(NAME affinity COMMAND test-affinity)
    add_executable(test-errno tests/unit/test_errno.c $<TARGET_OBJECTS:hl-production-errno>)
    target_include_directories(test-errno PRIVATE include tests/unit)
    add_test(NAME errno COMMAND test-errno)
    add_executable(test-number tests/unit/test_number.c $<TARGET_OBJECTS:hl-production-number>)
    target_include_directories(test-number PRIVATE include tests/unit)
    add_test(NAME number COMMAND test-number)
    add_executable(test-clock tests/unit/test_clock.c $<TARGET_OBJECTS:hl-production-clock>)
    target_include_directories(test-clock PRIVATE include tests/unit)
    target_link_libraries(test-clock PRIVATE hl-host-fake)
    add_test(NAME clock COMMAND test-clock)
    add_executable(test-file tests/unit/test_file.c $<TARGET_OBJECTS:hl-host-file>)
    target_include_directories(test-file PRIVATE include tests/unit)
    add_test(NAME file COMMAND test-file)
    if(TARGET hl-host-linux)
        add_executable(test-host_linux tests/unit/linux.c)
        target_include_directories(test-host_linux PRIVATE include tests/unit)
        target_link_libraries(test-host_linux PRIVATE hl-engine hl-host-linux)
        add_test(NAME host_linux COMMAND test-host_linux)
    endif()
endif()

install(TARGETS hl-engine hl-translator hl-linux-abi hl-engine-runner ARCHIVE DESTINATION lib RUNTIME DESTINATION bin)
install(DIRECTORY include/hl DESTINATION include)
