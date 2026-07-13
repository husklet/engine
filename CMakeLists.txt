cmake_minimum_required(VERSION 3.20)
project(hl_engine VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_C_VISIBILITY_PRESET hidden)

add_library(hl-engine STATIC
    src/core/config.c
    src/core/engine.c
    src/core/host_services.c
    src/translator/ir/interpreter.c
    src/translator/ir/ir.c
    src/linux_abi/linux_abi.c)
target_include_directories(hl-engine PUBLIC include)
target_compile_options(hl-engine PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wstrict-prototypes
                                         -Wmissing-prototypes)

add_library(hl-host-fake STATIC src/host/fake/fake_host.c)
target_include_directories(hl-host-fake PUBLIC include)
target_compile_options(hl-host-fake PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wstrict-prototypes
                                            -Wmissing-prototypes)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(Threads REQUIRED)
    add_library(hl-host-linux STATIC src/host/linux/host_linux.c)
    target_include_directories(hl-host-linux PUBLIC include)
    target_link_libraries(hl-host-linux PUBLIC Threads::Threads)
    target_compile_options(hl-host-linux PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wstrict-prototypes
                                                 -Wmissing-prototypes)
endif()

add_executable(hl-engine-runner src/runner/main.c)
target_link_libraries(hl-engine-runner PRIVATE hl-engine)

include(CTest)
if(BUILD_TESTING)
    foreach(test_name config host_services ir linux_abi engine)
        add_executable(test-${test_name} tests/unit/test_${test_name}.c)
        target_include_directories(test-${test_name} PRIVATE include tests/unit)
        target_link_libraries(test-${test_name} PRIVATE hl-engine hl-host-fake)
        add_test(NAME ${test_name} COMMAND test-${test_name})
    endforeach()
    if(TARGET hl-host-linux)
        add_executable(test-host_linux tests/unit/test_host_linux.c)
        target_include_directories(test-host_linux PRIVATE include tests/unit)
        target_link_libraries(test-host_linux PRIVATE hl-engine hl-host-linux)
        add_test(NAME host_linux COMMAND test-host_linux)
    endif()
endif()

install(TARGETS hl-engine hl-engine-runner ARCHIVE DESTINATION lib RUNTIME DESTINATION bin)
install(DIRECTORY include/hl DESTINATION include)
