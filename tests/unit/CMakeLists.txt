# Unit tests for the service's platform-independent pieces (sovereign_service_lib):
# no DLL, no pipe, no SCM — the control protocol runs against a fake ICore.

add_executable(unit-log-ring log_ring_test.cpp check.h)
target_link_libraries(unit-log-ring PRIVATE sovereign_service_lib)
sovereign_copy_asan_runtime(unit-log-ring)
add_test(NAME unit-log-ring COMMAND unit-log-ring)

add_executable(unit-control control_test.cpp check.h)
target_link_libraries(unit-control PRIVATE sovereign_service_lib)
sovereign_copy_asan_runtime(unit-control)
add_test(NAME unit-control COMMAND unit-control)
