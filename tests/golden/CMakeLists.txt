add_executable(golden-anytls-outbound anytls_outbound_test.cpp)
target_link_libraries(golden-anytls-outbound PRIVATE sovereign_common)

add_test(
  NAME golden-anytls-outbound
  COMMAND golden-anytls-outbound "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/anytls_outbound.golden.json"
)
