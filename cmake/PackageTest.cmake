# ---------------------------------------------------------------------------
# Script-mode driver for the `package-test` consumer smoke check.
#
# Mirrors the Makefile target (lines 495-517):
#   1. install the SDK into a throwaway staging prefix;
#   2. drop a foreign file into the staged include dir;
#   3. compile tests/integration/package.c against the STAGED headers/libs only
#      (nothing from the build tree) and run it;
#   4. on aarch64 additionally link tests/integration/activation_package.c
#      against the installed activation archive and run its self-test plus the
#      guest-execution leg;
#   5. prove the staged tree still contains the foreign file, i.e. the install
#      touched only what it owns.
#
# Step 3 is the real assertion: it proves the installed headers + .a + link
# line are self-sufficient for a downstream consumer.
# ---------------------------------------------------------------------------

function(run)
  execute_process(COMMAND ${ARGV} RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "failed (${rc}): ${ARGV}")
  endif()
endfunction()

file(REMOVE_RECURSE "${STAGE}" "${BUILD_DIR}/package-consumer")
file(MAKE_DIRECTORY "${BUILD_DIR}/package-consumer")

run(${CMAKE_COMMAND} --install "${BUILD_DIR}" --prefix "${STAGE}")

file(WRITE "${STAGE}/include/foreign.h" "not owned by hl-engine\n")

run(${CC} -I${STAGE}/include ${SOURCE_DIR}/tests/integration/package.c
    -L${STAGE}/lib -lhl-host-${PACKAGE_HOST} -lhl-engine -lhl-translator -lhl-linux-abi
    -pthread -o ${BUILD_DIR}/package-consumer/package)
run(${BUILD_DIR}/package-consumer/package)

if(HAVE_ACTIVATION AND EXISTS "${STAGE}/lib/libhl-engine-activation.a")
  run(${CC} -I${STAGE}/include ${SOURCE_DIR}/tests/integration/activation_package.c
      -Wl,--whole-archive ${STAGE}/lib/libhl-engine-activation.a -Wl,--no-whole-archive
      -pthread -ldl -lm -latomic
      -o ${BUILD_DIR}/package-consumer/activation-package)
  run(${BUILD_DIR}/package-consumer/activation-package)

  # The guest-execution leg needs the cross-built e2e guests. Skip cleanly when
  # they have not been built (they belong to the test matrix, not the SDK).
  set(guests
    ${BUILD_DIR}/e2e/guest-descendant-aarch64
    ${BUILD_DIR}/e2e/guest-external-term-aarch64
    ${BUILD_DIR}/e2e/guest-domain-aarch64
    ${BUILD_DIR}/e2e/guest-domain-x86_64)
  set(have TRUE)
  foreach(g ${guests})
    if(NOT EXISTS "${g}")
      set(have FALSE)
    endif()
  endforeach()
  if(have)
    run(${BUILD_DIR}/package-consumer/activation-package ${guests})
  else()
    message(STATUS "skip installed-activation guest-exec: e2e guests not built")
  endif()
endif()

# Uninstall parity: the foreign file must survive, the owned headers must not.
file(REMOVE "${STAGE}/include/hl/engine.h")
if(NOT EXISTS "${STAGE}/include/foreign.h")
  message(FATAL_ERROR "package-test: install/uninstall clobbered a foreign file")
endif()
file(REMOVE_RECURSE "${STAGE}")
