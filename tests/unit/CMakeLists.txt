# Unit tests for the service's platform-independent pieces (sovereign_service_lib):
# no DLL, no pipe, no SCM — the control protocol runs against a fake ICore.

add_executable(unit-log-ring log_ring_test.cpp check.h)
target_link_libraries(unit-log-ring PRIVATE sovereign_service_lib)
sovereign_copy_asan_runtime(unit-log-ring)
add_test(NAME unit-log-ring COMMAND unit-log-ring)

add_executable(unit-kill-switch kill_switch_test.cpp check.h)
target_link_libraries(unit-kill-switch PRIVATE sovereign_service_lib)
sovereign_copy_asan_runtime(unit-kill-switch)
add_test(NAME unit-kill-switch COMMAND unit-kill-switch)

add_executable(unit-control control_test.cpp check.h)
target_link_libraries(unit-control PRIVATE sovereign_service_lib)
sovereign_copy_asan_runtime(unit-control)
add_test(NAME unit-control COMMAND unit-control)

# The control plane's threads racing each other (tests/tsan/stress_test.cpp):
# here its invariants under MSVC and ASan; the race detection itself is the
# tsan job's, on Linux.
add_executable(unit-control-concurrency "${CMAKE_SOURCE_DIR}/tests/tsan/stress_test.cpp" check.h)
target_include_directories(unit-control-concurrency PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(unit-control-concurrency PRIVATE sovereign_service_lib)
sovereign_copy_asan_runtime(unit-control-concurrency)
add_test(NAME unit-control-concurrency COMMAND unit-control-concurrency)

# The tray's logic raced by threads (tests/tsan/tray_stress_test.cpp): its
# results under MSVC and ASan here, data races in the tsan job.
add_executable(unit-tray-concurrency "${CMAKE_SOURCE_DIR}/tests/tsan/tray_stress_test.cpp" check.h)
target_include_directories(unit-tray-concurrency PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(unit-tray-concurrency PRIVATE sovereign_tray_lib)
sovereign_copy_asan_runtime(unit-tray-concurrency)
add_test(NAME unit-tray-concurrency COMMAND unit-tray-concurrency)

add_executable(unit-tray-model tray_model_test.cpp check.h)
target_link_libraries(unit-tray-model PRIVATE sovereign_tray_lib)
sovereign_copy_asan_runtime(unit-tray-model)
add_test(NAME unit-tray-model COMMAND unit-tray-model)

add_executable(unit-subscription subscription_test.cpp check.h)
target_link_libraries(unit-subscription PRIVATE sovereign_tray_lib)
sovereign_copy_asan_runtime(unit-subscription)
add_test(NAME unit-subscription COMMAND unit-subscription)

add_executable(unit-app-rules app_rules_test.cpp check.h)
target_link_libraries(unit-app-rules PRIVATE sovereign_tray_lib)
sovereign_copy_asan_runtime(unit-app-rules)
add_test(NAME unit-app-rules COMMAND unit-app-rules)

add_executable(unit-protocol-choice protocol_choice_test.cpp check.h)
target_link_libraries(unit-protocol-choice PRIVATE sovereign_tray_lib)
sovereign_copy_asan_runtime(unit-protocol-choice)
add_test(NAME unit-protocol-choice COMMAND unit-protocol-choice)

add_executable(unit-cache-file cache_file_test.cpp check.h)
target_link_libraries(unit-cache-file PRIVATE sovereign_tray_lib)
sovereign_copy_asan_runtime(unit-cache-file)
add_test(NAME unit-cache-file COMMAND unit-cache-file)

add_executable(unit-delays delays_test.cpp check.h)
target_link_libraries(unit-delays PRIVATE sovereign_tray_lib)
sovereign_copy_asan_runtime(unit-delays)
add_test(NAME unit-delays COMMAND unit-delays)

add_executable(unit-update update_test.cpp check.h)
target_link_libraries(unit-update PRIVATE sovereign_tray_lib)
sovereign_copy_asan_runtime(unit-update)
add_test(NAME unit-update COMMAND unit-update)

add_executable(unit-config-sync config_sync_test.cpp check.h)
target_link_libraries(unit-config-sync PRIVATE sovereign_tray_lib)
sovereign_copy_asan_runtime(unit-config-sync)
add_test(NAME unit-config-sync COMMAND unit-config-sync)

add_executable(unit-exit-ip exit_ip_test.cpp check.h)
target_link_libraries(unit-exit-ip PRIVATE sovereign_tray_lib)
sovereign_copy_asan_runtime(unit-exit-ip)
add_test(NAME unit-exit-ip COMMAND unit-exit-ip)

add_executable(unit-log-level log_level_test.cpp check.h)
target_link_libraries(unit-log-level PRIVATE sovereign_tray_lib)
sovereign_copy_asan_runtime(unit-log-level)
add_test(NAME unit-log-level COMMAND unit-log-level)

add_executable(unit-share-links share_links_test.cpp check.h)
target_link_libraries(unit-share-links PRIVATE sovereign_tray_lib)
sovereign_copy_asan_runtime(unit-share-links)
add_test(NAME unit-share-links COMMAND unit-share-links)

add_executable(unit-import-formats import_formats_test.cpp check.h)
target_link_libraries(unit-import-formats PRIVATE sovereign_tray_lib)
sovereign_copy_asan_runtime(unit-import-formats)
add_test(NAME unit-import-formats COMMAND unit-import-formats)
# The configs the import builds, through the pinned sing-box's own `check`
# (SOVEREIGN_REFERENCE_SINGBOX, from tests/conformance; the build job has it).
if (SOVEREIGN_REFERENCE_SINGBOX)
  add_test(NAME import-configs-singbox-check
           COMMAND unit-import-formats --check "${SOVEREIGN_REFERENCE_SINGBOX}")
endif()

add_executable(unit-combine combine_test.cpp check.h)
target_link_libraries(unit-combine PRIVATE sovereign_tray_lib)
sovereign_copy_asan_runtime(unit-combine)
add_test(NAME unit-combine COMMAND unit-combine)
if (SOVEREIGN_REFERENCE_SINGBOX)
  add_test(NAME combine-configs-singbox-check COMMAND unit-combine --check "${SOVEREIGN_REFERENCE_SINGBOX}")
endif()

add_executable(unit-routing routing_test.cpp check.h)
target_link_libraries(unit-routing PRIVATE sovereign_tray_lib)
sovereign_copy_asan_runtime(unit-routing)
add_test(NAME unit-routing COMMAND unit-routing)
if (SOVEREIGN_REFERENCE_SINGBOX)
  add_test(NAME routing-configs-singbox-check COMMAND unit-routing --check "${SOVEREIGN_REFERENCE_SINGBOX}")
endif()

add_executable(unit-diagnostics diagnostics_test.cpp check.h)
target_link_libraries(unit-diagnostics PRIVATE sovereign_tray_lib)
sovereign_copy_asan_runtime(unit-diagnostics)
add_test(NAME unit-diagnostics COMMAND unit-diagnostics)

add_executable(unit-warp warp_test.cpp check.h)
target_link_libraries(unit-warp PRIVATE sovereign_tray_lib)
sovereign_copy_asan_runtime(unit-warp)
add_test(NAME unit-warp COMMAND unit-warp)

add_executable(unit-flags flags_test.cpp check.h)
target_link_libraries(unit-flags PRIVATE sovereign_tray_lib)
sovereign_copy_asan_runtime(unit-flags)
add_test(NAME unit-flags COMMAND unit-flags)

add_executable(unit-profiles profiles_test.cpp check.h)
target_link_libraries(unit-profiles PRIVATE sovereign_tray_lib)
sovereign_copy_asan_runtime(unit-profiles)
add_test(NAME unit-profiles COMMAND unit-profiles)

# The tray's files in a temporary %LOCALAPPDATA%: the move into profiles.
add_executable(unit-settings settings_test.cpp check.h)
target_link_libraries(unit-settings PRIVATE sovereign_tray_settings)
sovereign_copy_asan_runtime(unit-settings)
add_test(NAME unit-settings COMMAND unit-settings)

# ZXing's writer draws the codes the reader gets back.
add_executable(unit-qr-codes qr_codes_test.cpp check.h)
target_link_libraries(unit-qr-codes PRIVATE sovereign_tray_lib ZXing::ZXing)
sovereign_copy_asan_runtime(unit-qr-codes)
add_test(NAME unit-qr-codes COMMAND unit-qr-codes)

# The pictures QR codes are read from: a file always; the clipboard and the screen
# with a desktop (CI, or SOVEREIGN_DESKTOP_TESTS=1 - see the test).
add_executable(unit-capture capture_test.cpp check.h)
target_link_libraries(unit-capture PRIVATE sovereign_tray_capture ZXing::ZXing)
sovereign_copy_asan_runtime(unit-capture)
add_test(NAME unit-capture COMMAND unit-capture)

add_executable(unit-effective-config effective_config_test.cpp check.h)
target_link_libraries(unit-effective-config PRIVATE sovereign_tray_lib)
sovereign_copy_asan_runtime(unit-effective-config)
add_test(NAME unit-effective-config COMMAND unit-effective-config)

add_executable(unit-log-lines log_lines_test.cpp check.h)
target_link_libraries(unit-log-lines PRIVATE sovereign_tray_lib)
sovereign_copy_asan_runtime(unit-log-lines)
add_test(NAME unit-log-lines COMMAND unit-log-lines)

# gocore-stub.dll: sovereign-gocore.dll's C ABI with no Go runtime behind it, so
# GoCore's C++ side runs under ASan too (why the real DLL can't: issue #9).
add_library(gocore-stub SHARED gocore_stub.cpp)

add_executable(unit-go-core go_core_test.cpp check.h)
target_link_libraries(unit-go-core PRIVATE sovereign_gocore_host)
add_dependencies(unit-go-core gocore-stub)
sovereign_copy_asan_runtime(unit-go-core)
add_test(NAME unit-go-core COMMAND unit-go-core)

# The checks from a console, against the internet as this machine sees it: by hand, not a ctest.
add_executable(diagnose-probe diagnose_probe.cpp)
target_link_libraries(diagnose-probe PRIVATE sovereign_tray_net)
sovereign_copy_asan_runtime(diagnose-probe)
