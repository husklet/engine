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
# line are self-sufficient for a downstream consumer. Its link line is
# hl-engine.pc's Libs: verbatim, so a change to one that is not made in the
# other fails here. libhl-translator.a is not in it and is not installed --
# step 4's activation archive is the complete engine (Phase4Install.cmake).
#
# Steps 4-5 run real guests through the activation API (posix_spawn-self, JIT,
# domain daemons), so this case is not just a link check: it can fail for any
# reason a guest launch can fail, and it must say which.
# ---------------------------------------------------------------------------

set(hl_step 0)

# Run one step. Announces itself first, so a truncated or timed-out log still
# shows how far the driver got, and on failure prints the label, the exact argv,
# the exit status and everything the child wrote -- unprefixed, on stderr.
#
# This exists because the failure was previously mute: CMake's own report is
# "CMake Error at PackageTest.cmake:22 (message): failed (7): <argv>" and ctest
# adds nothing but its generic exit 8, so a CI lane whose log endpoint needs
# admin rights annotated a bare "package.consumer-link (Failed) / exit code 8"
# and named neither the step nor the leg inside the consumer that returned 7.
# The ERROR: prefix is what tools/ci_run.sh's annotation filter selects on.
function(run label)
  math(EXPR hl_step "${hl_step} + 1")
  set(hl_step "${hl_step}" PARENT_SCOPE)
  string(REPLACE ";" " " argv_text "${ARGN}")
  message(NOTICE "package-test: step ${hl_step}: ${label}")
  # ECHO_*_VARIABLE keeps the child's output streaming into the test log while
  # also capturing it, so the banner below can restate it next to the status.
  execute_process(COMMAND ${ARGN} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
                  ECHO_OUTPUT_VARIABLE ECHO_ERROR_VARIABLE)
  if(rc EQUAL 0)
    return()
  endif()
  message(NOTICE "ERROR: package-test: step ${hl_step} FAILED: ${label}")
  message(NOTICE "ERROR: package-test:   status: ${rc}")
  message(NOTICE "ERROR: package-test:   argv:   ${argv_text}")
  # Prefix EVERY captured line, not just the first: ci_run.sh's filter selects
  # whole lines, so an unprefixed continuation line is dropped from the
  # annotation and the reason arrives truncated to its first line.
  string(STRIP "${out}" out)
  string(STRIP "${err}" err)
  foreach(stream stdout stderr)
    if(stream STREQUAL stdout)
      set(text "${out}")
    else()
      set(text "${err}")
    endif()
    if(NOT text STREQUAL "")
      string(REPLACE "\n" "\nERROR: package-test:   ${stream}| " text "${text}")
      message(NOTICE "ERROR: package-test:   ${stream}| ${text}")
    endif()
  endforeach()
  message(FATAL_ERROR "package-test: step ${hl_step} (${label}) failed with status ${rc}")
endfunction()

# Same reporting contract for the driver's own assertions, which have no child
# process to blame.
function(fail reason)
  message(NOTICE "ERROR: package-test: ${reason}")
  message(FATAL_ERROR "package-test: ${reason}")
endfunction()

# Record a fact about the host. Never fails the test: this is context for
# reading a failure, not an assertion about the environment.
function(probe label)
  execute_process(COMMAND ${ARGN} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
                  OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
  string(REPLACE "\n" " / " out "${out}${err}")
  message(NOTICE "package-test: ${label}: ${out} (status ${rc})")
endfunction()

message(NOTICE "package-test: build=${BUILD_DIR} stage=${STAGE} host=${PACKAGE_HOST} "
               "activation=${HAVE_ACTIVATION} darwin=${IS_DARWIN}")

# The consumers below JIT, and the guest-exec legs run a whole engine inside
# them. On a full-security mac (SIP on, as on the macos-26 CI runner) AMFI
# SIGKILLs a process that makes non-MAP_JIT memory executable unless it carries
# packaging/macos/jit.entitlements -- see cc9347a5b. A dev host with SIP
# disabled does NOT enforce that, so a signing defect here is green locally and
# red on the runner, and the log has to say which posture produced the verdict.
if(IS_DARWIN)
  probe("SIP posture" /usr/bin/csrutil status)
endif()

file(REMOVE_RECURSE "${STAGE}" "${BUILD_DIR}/package-consumer")
file(MAKE_DIRECTORY "${BUILD_DIR}/package-consumer")

run("install the SDK into the staging prefix" ${CMAKE_COMMAND} --install "${BUILD_DIR}" --prefix "${STAGE}")

file(WRITE "${STAGE}/include/foreign.h" "not owned by hl-engine\n")
file(STRINGS "${BUILD_DIR}/install_manifest.txt" installed_files)
if(NOT installed_files)
  fail("install manifest ${BUILD_DIR}/install_manifest.txt contains no files")
endif()

run("link the C consumer against the staged SDK"
    ${CC} -I${STAGE}/include ${SOURCE_DIR}/tests/integration/package.c
    -L${STAGE}/lib -lhl-host-${PACKAGE_HOST} -lhl-engine -lhl-linux-abi
    -pthread -o ${BUILD_DIR}/package-consumer/package)
run("run the C consumer" ${BUILD_DIR}/package-consumer/package)

if(HAVE_ACTIVATION AND EXISTS "${STAGE}/lib/libhl-engine-activation.a")
  if(IS_DARWIN)
    run("link the activation consumer against the staged activation archive"
        ${CC} -I${STAGE}/include
        ${SOURCE_DIR}/tests/integration/activation_package.c
        -Wl,-force_load,${STAGE}/lib/libhl-engine-activation.a
        -o ${BUILD_DIR}/package-consumer/activation-package)
    run("codesign the activation consumer with the JIT entitlement"
        ${CODESIGN} -s - --entitlements ${JIT_ENTITLEMENTS} -f
        ${BUILD_DIR}/package-consumer/activation-package)
    # An ad-hoc signature that does not verify, or that carries no JIT
    # entitlements, means every run below dies by SIGKILL on a SIP-on host for a
    # reason no exit status explains. Assert the signature; report the
    # entitlements it actually got.
    run("verify the activation consumer's signature"
        ${CODESIGN} --verify --verbose=2 ${BUILD_DIR}/package-consumer/activation-package)
    probe("activation consumer entitlements"
          ${CODESIGN} -d --entitlements - ${BUILD_DIR}/package-consumer/activation-package)
  else()
    run("link the activation consumer against the staged activation archive"
        ${CC} -I${STAGE}/include
        ${SOURCE_DIR}/tests/integration/activation_package.c
        -Wl,--whole-archive ${STAGE}/lib/libhl-engine-activation.a
        -Wl,--no-whole-archive -pthread -ldl -lm -latomic
        -o ${BUILD_DIR}/package-consumer/activation-package)
  endif()
  run("run the activation consumer's API self-test" ${BUILD_DIR}/package-consumer/activation-package)

  # The guest-execution leg needs the cross-built e2e guests. Skip cleanly when
  # they have not been built (they belong to the test matrix, not the SDK) --
  # but say so, because a silent skip here shrinks the gate to a link check.
  set(guests
    ${BUILD_DIR}/e2e/guest-descendant-aarch64
    ${BUILD_DIR}/e2e/guest-external-term-aarch64
    ${BUILD_DIR}/e2e/guest-domain-aarch64
    ${BUILD_DIR}/e2e/guest-domain-x86_64)
  set(missing "")
  foreach(g ${guests})
    if(NOT EXISTS "${g}")
      list(APPEND missing "${g}")
    endif()
  endforeach()
  if(missing STREQUAL "")
    run("run the activation consumer's guest-execution leg (force-stop, signal forwarding, process domains)"
        ${BUILD_DIR}/package-consumer/activation-package ${guests})
  else()
    string(REPLACE ";" " " missing_text "${missing}")
    message(NOTICE "SKIP package-test: installed-activation guest-exec: e2e guests not built: ${missing_text}")
  endif()

  if(IS_DARWIN)
    set(objc_consumer
      "${BUILD_DIR}/package-consumer/activation-objc-package")
    run("link the Objective-C activation consumer against the staged activation archive"
        ${CC} -I${STAGE}/include
        ${SOURCE_DIR}/tests/integration/activation_objc_package.m
        -Wl,-force_load,${STAGE}/lib/libhl-engine-activation.a
        -framework Foundation -o ${objc_consumer})
    run("codesign the Objective-C activation consumer with the JIT entitlement"
        ${CODESIGN} -s - --entitlements ${JIT_ENTITLEMENTS} -f
        ${objc_consumer})
    run("verify the Objective-C activation consumer's signature"
        ${CODESIGN} --verify --verbose=2 ${objc_consumer})
    probe("Objective-C activation consumer entitlements" ${CODESIGN} -d --entitlements - ${objc_consumer})
    if(EXISTS "${BUILD_DIR}/e2e/guest-exit-aarch64")
      run("run the Objective-C activation consumer's 50 guest spawns"
          ${objc_consumer} ${BUILD_DIR}/e2e/guest-exit-aarch64)
    else()
      message(NOTICE "SKIP package-test: installed Objective-C activation guest-exec: "
                     "${BUILD_DIR}/e2e/guest-exit-aarch64 not built")
    endif()
  endif()
endif()

# Uninstall parity: every manifest-owned file must disappear while the foreign
# file sharing the include prefix survives.
run("uninstall from the staging prefix" ${CMAKE_COMMAND} --build "${BUILD_DIR}" --target uninstall)
foreach(installed IN LISTS installed_files)
  if(EXISTS "${installed}" OR IS_SYMLINK "${installed}")
    fail("uninstall left owned file ${installed}")
  endif()
endforeach()
if(NOT EXISTS "${STAGE}/include/foreign.h")
  fail("install/uninstall clobbered a foreign file (${STAGE}/include/foreign.h)")
endif()
file(REMOVE_RECURSE "${STAGE}")
message(NOTICE "package-test: ${hl_step} steps passed")
