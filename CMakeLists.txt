cmake_minimum_required(VERSION 3.20)
project(hl_engine VERSION 0.1.0 LANGUAGES C)

find_package(Threads REQUIRED)

option(HL_ENABLE_LOGGING "Compile tagged engine logging into the binaries" OFF)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_C_VISIBILITY_PRESET hidden)

add_library(hl-translator STATIC
    src/translator/arena.c
    src/translator/codegen.c
    src/translator/digest.c
    src/translator/guest/x86_64/decode.c
    src/translator/identity.c
    src/translator/reloc.c
    src/translator/window.c
    src/translator/host/aarch64/codegen.c
    src/translator/host/x86_64/codegen.c
    src/translator/ir/interpreter.c
    src/translator/ir/ir.c)
target_include_directories(hl-translator PUBLIC include)

add_library(hl-linux-abi STATIC
    src/linux_abi/affinity.c
    src/linux_abi/container/vfs/gmap.c
    src/linux_abi/device.c
    src/linux_abi/encode.c
    src/linux_abi/errno.c
    src/linux_abi/fdcache.c
    src/linux_abi/limits.c
    src/linux_abi/linux_abi.c
    src/linux_abi/number.c
    src/linux_abi/parse.c
    src/linux_abi/readonly.c
    src/linux_abi/stat.c
    src/linux_abi/xattr.c)
target_include_directories(hl-linux-abi PUBLIC include)

add_library(hl-engine STATIC src/core/cli.c src/core/config.c src/core/engine.c src/core/host_services.c src/core/launch.c src/core/log.c
                             src/core/options.c)
target_include_directories(hl-engine PUBLIC include)
target_compile_definitions(hl-engine PUBLIC HL_ENABLE_LOGGING=$<BOOL:${HL_ENABLE_LOGGING}>)
target_link_libraries(hl-engine PUBLIC hl-translator hl-linux-abi)

add_library(hl-host-clock OBJECT src/host/clock.c)
add_library(hl-host-file OBJECT src/host/file.c)

target_include_directories(hl-host-clock PRIVATE include src/host)
target_compile_options(hl-host-clock PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wstrict-prototypes
                                             -Wmissing-prototypes)
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
    add_library(hl-host-linux STATIC src/host/linux/host.c src/host/sync.c)
    target_include_directories(hl-host-linux PUBLIC include)
    target_link_libraries(hl-host-linux PUBLIC Threads::Threads)
    target_compile_options(hl-host-linux PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wstrict-prototypes
                                                 -Wmissing-prototypes)
endif()

if(APPLE)
    add_library(hl-host-macos STATIC src/host/macos/host.c src/host/sync.c)
    target_include_directories(hl-host-macos PUBLIC include)
    target_compile_options(hl-host-macos PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wstrict-prototypes
                                                 -Wmissing-prototypes)
endif()

add_executable(hl-engine-runner src/runner/main.c)
target_link_libraries(hl-engine-runner PRIVATE hl-engine)

include(CTest)
if(BUILD_TESTING)
    foreach(test_name arena cli codegen config decoder emit fdcache gmap host_services identity ir launch linux_abi stat engine log namespace options profile)
        add_executable(test-${test_name} tests/unit/test_${test_name}.c)
        target_include_directories(test-${test_name} PRIVATE include tests/unit)
        target_link_libraries(test-${test_name} PRIVATE hl-engine hl-host-fake)
        if(test_name STREQUAL "linux_abi" OR test_name STREQUAL "stat" OR test_name STREQUAL "fdcache" OR
           test_name STREQUAL "gmap")
            target_link_libraries(test-${test_name} PRIVATE hl-linux-abi)
        endif()
        if(test_name STREQUAL "fdcache")
            target_include_directories(test-${test_name} PRIVATE src/linux_abi)
        endif()
        if(test_name STREQUAL "linux_abi")
            target_link_libraries(test-${test_name} PRIVATE Threads::Threads)
        endif()
        add_test(NAME ${test_name} COMMAND test-${test_name})
    endforeach()
    add_executable(test-readonly tests/unit/test_readonly.c)
    target_include_directories(test-readonly PRIVATE include tests/unit)
    target_link_libraries(test-readonly PRIVATE hl-linux-abi)
    add_test(NAME readonly COMMAND test-readonly)
    add_executable(test-limits tests/unit/test_limits.c)
    target_include_directories(test-limits PRIVATE include tests/unit)
    target_link_libraries(test-limits PRIVATE hl-linux-abi Threads::Threads)
    add_test(NAME limits COMMAND test-limits)
    add_executable(test-reloc tests/unit/test_reloc.c)
    target_include_directories(test-reloc PRIVATE include tests/unit src/translator)
    target_link_libraries(test-reloc PRIVATE hl-translator)
    add_test(NAME reloc COMMAND test-reloc)
    add_executable(test-digest tests/unit/test_digest.c)
    target_include_directories(test-digest PRIVATE include tests/unit src/translator)
    target_link_libraries(test-digest PRIVATE hl-translator)
    add_test(NAME digest COMMAND test-digest)
    add_executable(test-window tests/unit/test_window.c)
    target_include_directories(test-window PRIVATE include tests/unit src/translator)
    target_link_libraries(test-window PRIVATE hl-translator)
    add_test(NAME window COMMAND test-window)
    add_executable(test-device tests/unit/test_device.c)
    target_include_directories(test-device PRIVATE include tests/unit)
    target_link_libraries(test-device PRIVATE hl-linux-abi)
    add_test(NAME device COMMAND test-device)
    add_executable(test-affinity tests/unit/test_affinity.c)
    target_include_directories(test-affinity PRIVATE include tests/unit)
    target_link_libraries(test-affinity PRIVATE hl-linux-abi)
    add_test(NAME affinity COMMAND test-affinity)
    add_executable(test-errno tests/unit/test_errno.c)
    target_include_directories(test-errno PRIVATE include tests/unit)
    target_link_libraries(test-errno PRIVATE hl-linux-abi)
    add_test(NAME errno COMMAND test-errno)
    add_executable(test-number tests/unit/test_number.c)
    target_include_directories(test-number PRIVATE include tests/unit)
    target_link_libraries(test-number PRIVATE hl-linux-abi)
    add_test(NAME number COMMAND test-number)
    add_executable(test-parse tests/unit/test_parse.c)
    target_include_directories(test-parse PRIVATE include tests/unit src/linux_abi)
    target_link_libraries(test-parse PRIVATE hl-linux-abi)
    add_test(NAME parse COMMAND test-parse)
    add_executable(test-xattr_cache tests/unit/test_xattr_cache.c)
    target_include_directories(test-xattr_cache PRIVATE include tests/unit)
    target_link_libraries(test-xattr_cache PRIVATE hl-linux-abi)
    add_test(NAME xattr_cache COMMAND test-xattr_cache)
    add_executable(test-clock tests/unit/test_clock.c $<TARGET_OBJECTS:hl-host-clock>)
    target_include_directories(test-clock PRIVATE include tests/unit)
    target_link_libraries(test-clock PRIVATE hl-host-fake)
    add_test(NAME clock COMMAND test-clock)
    add_executable(test-file tests/unit/test_file.c $<TARGET_OBJECTS:hl-host-file>)
    target_include_directories(test-file PRIVATE include tests/unit)
    add_test(NAME file COMMAND test-file)
    if(TARGET hl-host-linux)
        add_executable(test-linux tests/unit/linux.c)
        target_include_directories(test-linux PRIVATE include tests/unit)
        target_link_libraries(test-linux PRIVATE hl-engine hl-host-linux)
        add_test(NAME linux COMMAND test-linux)
    endif()
endif()

install(TARGETS hl-engine hl-translator hl-linux-abi hl-engine-runner ARCHIVE DESTINATION lib RUNTIME DESTINATION bin)
install(DIRECTORY include/hl DESTINATION include)
