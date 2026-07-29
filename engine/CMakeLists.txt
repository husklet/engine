# Engine libraries. The source tree remains at the repository root until the
# workspace ownership seam is proven on every host.
set(_hl_root "${PROJECT_SOURCE_DIR}")

set(PROVIDER_SOURCES
  ${_hl_root}/src/core/environment.c ${_hl_root}/src/core/provider/client.c
  ${_hl_root}/src/core/provider/demux.c ${_hl_root}/src/core/provider/files.c
  ${_hl_root}/src/core/provider/handles.c ${_hl_root}/src/core/provider/namespace.c)

set(CORE_SOURCES
  ${_hl_root}/src/core/bus.c ${_hl_root}/src/core/checkpoint_channel.c
  ${_hl_root}/src/core/cli.c ${_hl_root}/src/core/config.c
  ${_hl_root}/src/core/engine.c ${_hl_root}/src/core/fatal.c
  ${_hl_root}/src/core/host_services.c ${_hl_root}/src/core/launch.c
  ${_hl_root}/src/core/log.c ${_hl_root}/src/core/options.c
  ${_hl_root}/src/core/target/bus.c ${_hl_root}/src/core/target/native.c
  ${_hl_root}/src/core/target/run.c ${_hl_root}/src/core/target/services.c
  ${PROVIDER_SOURCES})

set(IR_SOURCES
  ${_hl_root}/src/translator/arena.c ${_hl_root}/src/translator/digest.c
  ${_hl_root}/src/translator/guest_fetch.c ${_hl_root}/src/translator/guest_memory.c
  ${_hl_root}/src/translator/identity.c ${_hl_root}/src/translator/persist.c
  ${_hl_root}/src/translator/reloc.c ${_hl_root}/src/translator/window.c
  ${_hl_root}/src/translator/guest/x86_64/decode.c
  ${_hl_root}/src/translator/guest/x86_64/address.c
  ${_hl_root}/src/translator/guest/x86_64/glue.c
  ${_hl_root}/src/translator/guest/x86_64/avx.c
  ${_hl_root}/src/translator/host/aarch64/asm.c
  ${_hl_root}/src/translator/guest/x86_64/cpuid.c
  ${_hl_root}/src/translator/guest/x86_64/cmpxchg.c
  ${_hl_root}/src/translator/guest/x86_64/legacy.c
  ${_hl_root}/src/translator/guest/x86_64/rep_runtime.c
  ${_hl_root}/src/translator/guest/x86_64/rep.c
  ${_hl_root}/src/translator/guest/x86_64/rotate.c
  ${_hl_root}/src/translator/guest/x86_64/x87math.c
  ${_hl_root}/src/translator/guest/x86_64/x87state.c
  ${_hl_root}/src/translator/guest/aarch64/signal.c
  ${_hl_root}/src/translator/guest/x86_64/signal.c
  ${_hl_root}/src/translator/guest/x86_64/operand.c
  ${_hl_root}/src/translator/guest/x86_64/flags.c)

if(HL_HOST_ARCH STREQUAL "aarch64")
  list(APPEND IR_SOURCES
    ${_hl_root}/src/translator/guest/x86_64/lower/alu.c
    ${_hl_root}/src/translator/guest/x86_64/lower/crypto.c
    ${_hl_root}/src/translator/guest/x86_64/lower/mov.c
    ${_hl_root}/src/translator/guest/x86_64/lower/repstr.c
    ${_hl_root}/src/translator/guest/x86_64/lower/shift.c
    ${_hl_root}/src/translator/guest/x86_64/lower/sse4x.c
    ${_hl_root}/src/translator/guest/x86_64/lower/trace.c
    ${_hl_root}/src/translator/guest/x86_64/lower/x87.c
    ${_hl_root}/src/translator/guest/x86_64/lower/x87_stack.c)
endif()

set(LINUX_ABI_SOURCES
  ${_hl_root}/src/linux_abi/affinity.c ${_hl_root}/src/linux_abi/container/key.c
  ${_hl_root}/src/linux_abi/container/pidmap.c ${_hl_root}/src/linux_abi/container/ports.c
  ${_hl_root}/src/linux_abi/container/snapshot.c
  ${_hl_root}/src/linux_abi/container/vfs/gmap.c
  ${_hl_root}/src/linux_abi/container/shm.c ${_hl_root}/src/linux_abi/device.c
  ${_hl_root}/src/linux_abi/dns.c ${_hl_root}/src/linux_abi/image.c
  ${_hl_root}/src/linux_abi/logical_vma.c ${_hl_root}/src/linux_abi/fdcache.c
  ${_hl_root}/src/linux_abi/epoll.c ${_hl_root}/src/linux_abi/eventfd.c
  ${_hl_root}/src/linux_abi/fork_codec.c ${_hl_root}/src/linux_abi/inotify.c
  ${_hl_root}/src/linux_abi/pipe.c ${_hl_root}/src/linux_abi/placement.c
  ${_hl_root}/src/linux_abi/errno.c ${_hl_root}/src/linux_abi/limits.c
  ${_hl_root}/src/linux_abi/linux_abi.c ${_hl_root}/src/linux_abi/number.c
  ${_hl_root}/src/linux_abi/open_plan.c ${_hl_root}/src/linux_abi/parse.c
  ${_hl_root}/src/linux_abi/readonly.c ${_hl_root}/src/linux_abi/seccomp_vm.c
  ${_hl_root}/src/linux_abi/shared.c ${_hl_root}/src/linux_abi/stat.c
  ${_hl_root}/src/linux_abi/watch.c ${_hl_root}/src/linux_abi/xattr.c
  ${_hl_root}/src/linux_abi/syscall/misc.c)

set(FAKE_HOST_SOURCES ${_hl_root}/src/host/fake/host.c)

set(COMMON_HOST_SOURCES
  ${_hl_root}/src/host/child.c ${_hl_root}/src/host/fork_wire.c
  ${_hl_root}/src/host/private.c ${_hl_root}/src/host/range.c
  ${_hl_root}/src/host/resolve.c ${_hl_root}/src/host/sync.c)

set(LINUX_HOST_SOURCES
  ${_hl_root}/src/host/linux/directory.c ${_hl_root}/src/host/linux/host.c
  ${_hl_root}/src/host/linux/process.c ${_hl_root}/src/host/linux/range.c
  ${_hl_root}/src/host/linux/system.c ${_hl_root}/src/host/clock.c
  ${_hl_root}/src/host/file.c ${COMMON_HOST_SOURCES})

set(MACOS_HOST_SOURCES
  ${_hl_root}/src/host/macos/directory.c ${_hl_root}/src/host/macos/host.c
  ${_hl_root}/src/host/macos/process.c ${_hl_root}/src/host/macos/range.c
  ${_hl_root}/src/host/macos/system.c)

set(WINDOWS_HOST_SOURCES
  ${_hl_root}/src/host/windows/host.c ${_hl_root}/src/host/windows/memory.c
  ${_hl_root}/src/host/windows/clock.c ${_hl_root}/src/host/windows/fault.c
  ${_hl_root}/src/host/windows/ntpath.c ${_hl_root}/src/host/windows/file.c
  ${_hl_root}/src/host/windows/process.c ${_hl_root}/src/host/windows/range.c
  ${_hl_root}/src/host/windows/system.c ${_hl_root}/src/host/windows/directory.c
  ${_hl_root}/src/host/windows/child.c ${_hl_root}/src/host/windows/fork_wire.c
  ${_hl_root}/src/host/windows/counter.c ${_hl_root}/src/host/windows/event.c
  ${_hl_root}/src/host/windows/stream.c ${_hl_root}/src/host/windows/socket.c
  ${_hl_root}/src/host/windows/private.c ${_hl_root}/src/host/windows/resolve.c
  ${_hl_root}/src/host/windows/math.c ${_hl_root}/src/host/range.c
  ${_hl_root}/src/host/file.c ${_hl_root}/src/host/clock.c
  ${_hl_root}/src/host/sync.c)

add_library(hl-engine STATIC ${CORE_SOURCES})
add_library(hl-translator STATIC ${IR_SOURCES})
add_library(hl-linux-abi STATIC ${LINUX_ABI_SOURCES})
add_library(hl-host-fake STATIC ${FAKE_HOST_SOURCES})
foreach(_lib hl-engine hl-translator hl-linux-abi hl-host-fake)
  target_link_libraries(${_lib} PRIVATE hl_engine_cflags)
  set_target_properties(${_lib} PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
endforeach()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_library(hl-host-linux STATIC ${LINUX_HOST_SOURCES})
  target_link_libraries(hl-host-linux PRIVATE hl_engine_cflags)
  set(_hl_host_library hl-host-linux)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  add_library(hl-host-macos STATIC
    ${MACOS_HOST_SOURCES} ${COMMON_HOST_SOURCES}
    ${_hl_root}/src/host/clock.c ${_hl_root}/src/host/file.c)
  target_link_libraries(hl-host-macos PRIVATE hl_engine_cflags)
  set(_hl_host_library hl-host-macos)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
  add_library(hl-host-windows STATIC ${WINDOWS_HOST_SOURCES})
  target_link_libraries(hl-host-windows PRIVATE hl_engine_cflags)
  set(_hl_host_library hl-host-windows)
endif()
set_target_properties(${_hl_host_library} PROPERTIES
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# The production, macOS and lint phases consume the same source closures.
foreach(_sources IN ITEMS
    CORE_SOURCES IR_SOURCES LINUX_ABI_SOURCES
    FAKE_HOST_SOURCES COMMON_HOST_SOURCES LINUX_HOST_SOURCES
    MACOS_HOST_SOURCES WINDOWS_HOST_SOURCES)
  set(_hl_relative_sources "")
  foreach(_source IN LISTS ${_sources})
    file(RELATIVE_PATH _source "${_hl_root}" "${_source}")
    list(APPEND _hl_relative_sources "${_source}")
  endforeach()
  set(${_sources} "${_hl_relative_sources}" PARENT_SCOPE)
endforeach()
