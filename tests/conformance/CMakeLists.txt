# ClientHello parsing and canonicalization against captures of the pinned
# reference sing-box (fixtures/*.hex, recorded by conformance-harness --save).
add_executable(conformance-client-hello client_hello_test.cpp "${CMAKE_SOURCE_DIR}/tests/unit/check.h")
target_include_directories(conformance-client-hello PRIVATE "${CMAKE_SOURCE_DIR}/tests/unit")
target_link_libraries(conformance-client-hello PRIVATE sovereign_conformance)
sovereign_copy_asan_runtime(conformance-client-hello)
add_test(NAME conformance-client-hello
         COMMAND conformance-client-hello "${CMAKE_CURRENT_SOURCE_DIR}/fixtures")

# The harness (harness.cpp): what a client puts on the wire for each case,
# compared with golden/<case>.hex - one ClientHello of the pinned reference.
# Our side is the real sovereign-gocore.dll, hosted in-process; the reference
# side runs only when the release binary is configured (CI downloads it,
# sha256-pinned). Neither runs under ASan: issue #9.
add_executable(conformance-harness harness.cpp)
target_link_libraries(conformance-harness PRIVATE sovereign_conformance sovereign_gocore_host sovereign_service_lib
                      sovereign_tray_lib sovereign_common ws2_32)
add_dependencies(conformance-harness sovereign_gocore_dll)

set(SOVEREIGN_REFERENCE_SINGBOX "" CACHE FILEPATH
    "The pinned official sing-box release binary, the conformance reference; empty skips its tests")
set(conformanceCases anytls-reality-chrome anytls-tls-chrome vless-reality-firefox)
if (NOT SOVEREIGN_ENABLE_ASAN)
  foreach(case IN LISTS conformanceCases)
    add_test(NAME conformance-gocore-${case}
             COMMAND conformance-harness ${case}
                     --gocore "${CMAKE_BINARY_DIR}/src/service/sovereign-gocore.dll"
                     --golden "${CMAKE_CURRENT_SOURCE_DIR}/golden/${case}.hex")
    if (SOVEREIGN_REFERENCE_SINGBOX)
      add_test(NAME conformance-reference-${case}
               COMMAND conformance-harness ${case}
                       --binary "${SOVEREIGN_REFERENCE_SINGBOX}"
                       --golden "${CMAKE_CURRENT_SOURCE_DIR}/golden/${case}.hex")
    endif()
  endforeach()
  # The harness isn't vacuous: REALITY's ClientHello checked against the plain
  # TLS golden one must fail (no X25519MLKEM768 group and share - see #5).
  add_test(NAME conformance-gocore-detects-a-difference
           COMMAND conformance-harness anytls-reality-chrome
                   --gocore "${CMAKE_BINARY_DIR}/src/service/sovereign-gocore.dll"
                   --golden "${CMAKE_CURRENT_SOURCE_DIR}/golden/anytls-tls-chrome.hex")
  # Its specific verdict, not just any failure (a crash would pass WILL_FAIL).
  set_tests_properties(conformance-gocore-detects-a-difference PROPERTIES
                       PASS_REGULAR_EXPRESSION "supported_groups.*FAIL: 5 of 5 ClientHellos differ from the golden one")
endif()
