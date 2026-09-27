add_executable(golden-anytls-outbound anytls_outbound_test.cpp)
target_link_libraries(golden-anytls-outbound PRIVATE sovereign_common)
sovereign_copy_asan_runtime(golden-anytls-outbound)

add_test(
  NAME golden-anytls-outbound
  COMMAND golden-anytls-outbound "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/anytls_outbound.golden.json"
)

# Every nested TLS object and field shape of OutboundTLSOptions (tools/goldengen
# -fixture anytls_outbound_tls_full): the generated TLS structs against the real marshaler.
add_test(
  NAME golden-anytls-outbound-tls-full
  COMMAND golden-anytls-outbound "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/anytls_outbound_tls_full.golden.json"
)

add_executable(golden-anytls-inbound-padding anytls_inbound_padding_test.cpp)
target_link_libraries(golden-anytls-inbound-padding PRIVATE sovereign_common)
sovereign_copy_asan_runtime(golden-anytls-inbound-padding)

add_test(
  NAME golden-anytls-inbound-padding-single
  COMMAND golden-anytls-inbound-padding "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/anytls_inbound_padding_single.golden.json"
)
add_test(
  NAME golden-anytls-inbound-padding-array
  COMMAND golden-anytls-inbound-padding "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/anytls_inbound_padding_array.golden.json"
)

# badoption.Duration both ways against the pinned sing code (tools/goldengen -fixture durations).
add_executable(golden-duration duration_test.cpp)
target_link_libraries(golden-duration PRIVATE sovereign_common)
sovereign_copy_asan_runtime(golden-duration)
add_test(
  NAME golden-duration
  COMMAND golden-duration "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/durations.golden.json"
)
