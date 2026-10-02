# libFuzzer targets (P3, issue #5), built only with -DSOVEREIGN_ENABLE_FUZZ=ON
# (preset ci-fuzz: MSVC's /fsanitize=fuzzer on top of ASan). Each target
# compiles the code under test into itself, so that code is coverage-
# instrumented as well - a prebuilt library linked in would be invisible to
# libFuzzer's guidance. Seed corpora: corpus/<target>/, run with
# tools/fuzz/run.ps1.
set(fuzzIncludes
  "${CMAKE_SOURCE_DIR}/src/conformance"
  "${CMAKE_SOURCE_DIR}/src/tray"
  "${CMAKE_SOURCE_DIR}/src/service")

if (NOT SOVEREIGN_ENABLE_FUZZ)
  # Without the fuzz preset the targets still compile (not link: libFuzzer
  # provides main) - so a change that breaks them fails every build, and
  # clang-tidy and /analyze see them in compile_commands.json.
  add_library(fuzz-targets-compile-check OBJECT
    client_hello_fuzz.cpp config_fuzz.cpp tray_input_fuzz.cpp control_fuzz.cpp)
  target_include_directories(fuzz-targets-compile-check PRIVATE ${fuzzIncludes})
  target_link_libraries(fuzz-targets-compile-check PRIVATE sovereign_common yaml-cpp::yaml-cpp)
  return()
endif()
if (NOT MSVC)
  message(FATAL_ERROR "SOVEREIGN_ENABLE_FUZZ needs MSVC's /fsanitize=fuzzer")
endif()

function(sovereign_fuzz_target name)
  add_executable(${name} ${ARGN})
  target_compile_options(${name} PRIVATE /fsanitize=fuzzer)
  target_link_libraries(${name} PRIVATE sovereign_common)
  sovereign_copy_asan_runtime(${name})
endfunction()

# The wire: ClientHello parsing and canonicalization.
sovereign_fuzz_target(fuzz-client-hello client_hello_fuzz.cpp
  "${CMAKE_SOURCE_DIR}/src/conformance/client_hello.cpp")
target_include_directories(fuzz-client-hello PRIVATE "${CMAKE_SOURCE_DIR}/src/conformance")

# The config model: generated option structs and the handwritten adapters.
sovereign_fuzz_target(fuzz-config config_fuzz.cpp)

# What the tray does with text it didn't write: subscription bodies, pipe
# responses, the saved config.
sovereign_fuzz_target(fuzz-tray-input tray_input_fuzz.cpp
  "${CMAKE_SOURCE_DIR}/src/tray/subscription.cpp"
  "${CMAKE_SOURCE_DIR}/src/tray/app_rules.cpp"
  "${CMAKE_SOURCE_DIR}/src/tray/protocol_choice.cpp"
  "${CMAKE_SOURCE_DIR}/src/tray/log_lines.cpp"
  "${CMAKE_SOURCE_DIR}/src/tray/cache_file.cpp"
  "${CMAKE_SOURCE_DIR}/src/tray/delays.cpp"
  "${CMAKE_SOURCE_DIR}/src/tray/update.cpp"
  "${CMAKE_SOURCE_DIR}/src/tray/config_sync.cpp"
  "${CMAKE_SOURCE_DIR}/src/tray/exit_ip.cpp"
  "${CMAKE_SOURCE_DIR}/src/tray/log_level.cpp"
  "${CMAKE_SOURCE_DIR}/src/tray/share_links.cpp"
  "${CMAKE_SOURCE_DIR}/src/tray/import_formats.cpp"
  "${CMAKE_SOURCE_DIR}/src/tray/combine.cpp"
  "${CMAKE_SOURCE_DIR}/src/tray/routing.cpp"
  "${CMAKE_SOURCE_DIR}/src/tray/warp.cpp"
  "${CMAKE_SOURCE_DIR}/src/tray/diagnostics.cpp")
target_include_directories(fuzz-tray-input PRIVATE "${CMAKE_SOURCE_DIR}/src/tray")
target_link_libraries(fuzz-tray-input PRIVATE yaml-cpp::yaml-cpp)

# The service's control protocol: what any local user can send a SYSTEM service.
sovereign_fuzz_target(fuzz-control control_fuzz.cpp
  "${CMAKE_SOURCE_DIR}/src/service/control.cpp"
  "${CMAKE_SOURCE_DIR}/src/service/log_ring.cpp"
  "${CMAKE_SOURCE_DIR}/src/service/kill_switch.cpp")
target_include_directories(fuzz-control PRIVATE "${CMAKE_SOURCE_DIR}/src/service")
