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
    foreach(test_name codegen config host_services ir linux_abi stat engine log options)
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
    if(TARGET hl-host-linux)
        add_executable(test-host_linux tests/unit/linux.c)
        target_include_directories(test-host_linux PRIVATE include tests/unit)
        target_link_libraries(test-host_linux PRIVATE hl-engine hl-host-linux)
        add_test(NAME host_linux COMMAND test-host_linux)
    endif()
endif()

install(TARGETS hl-engine hl-translator hl-linux-abi hl-engine-runner ARCHIVE DESTINATION lib RUNTIME DESTINATION bin)
install(DIRECTORY include/hl DESTINATION include)
