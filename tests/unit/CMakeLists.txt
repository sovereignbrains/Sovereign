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
