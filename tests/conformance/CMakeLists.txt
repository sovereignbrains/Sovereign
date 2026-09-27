# ClientHello parsing and canonicalization against captures of the pinned
# reference sing-box (fixtures/*.hex, recorded by tools/conformance/capture.py).
add_executable(conformance-client-hello client_hello_test.cpp "${CMAKE_SOURCE_DIR}/tests/unit/check.h")
target_include_directories(conformance-client-hello PRIVATE "${CMAKE_SOURCE_DIR}/tests/unit")
target_link_libraries(conformance-client-hello PRIVATE sovereign_conformance)
sovereign_copy_asan_runtime(conformance-client-hello)
add_test(NAME conformance-client-hello
         COMMAND conformance-client-hello "${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
