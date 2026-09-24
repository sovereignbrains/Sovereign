find_program(SOVEREIGN_GO_EXECUTABLE go REQUIRED)

# sovereign-gocore.dll (see gocore/) needs CGO, which on Windows means a
# MinGW-w64 gcc reachable on PATH — CC isn't hardcoded here so this works
# the same in CI and on a dev machine, as long as whoever runs
# `cmake --build` already has that gcc on PATH (matches how the rest of
# this project's toolchain setup works: presets assume a configured shell,
# not a self-contained one).
# Every .go file of the package: a change to any of them must rebuild the DLL.
file(GLOB SOVEREIGN_GOCORE_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/gocore/*.go")

add_custom_command(
  OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/sovereign-gocore.dll"
  COMMAND ${CMAKE_COMMAND} -E env CGO_ENABLED=1
          ${SOVEREIGN_GO_EXECUTABLE} build -buildmode=c-shared
          -o "${CMAKE_CURRENT_BINARY_DIR}/sovereign-gocore.dll" .
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/gocore"
  DEPENDS ${SOVEREIGN_GOCORE_SOURCES} "${CMAKE_SOURCE_DIR}/gocore/go.mod" "${CMAKE_SOURCE_DIR}/gocore/go.sum"
  COMMENT "Building sovereign-gocore.dll (Go c-shared)"
  VERBATIM
)
add_custom_target(sovereign_gocore_dll DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/sovereign-gocore.dll")

# Everything of the service that doesn't touch the Win32 service/pipe/DLL
# machinery — linked by the service and by tests/unit, which drive it with a
# fake ICore.
add_library(sovereign_service_lib STATIC
  core.h
  control.cpp
  control.h
  log_ring.cpp
  log_ring.h
)
target_include_directories(sovereign_service_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(sovereign_service_lib PUBLIC sovereign_common)

add_executable(sovereign-core
  main.cpp
  pipe_server.cpp
  pipe_server.h
  go_core.cpp
  go_core.h
)

target_link_libraries(sovereign-core PRIVATE sovereign_service_lib sovereign_common advapi32)
sovereign_copy_asan_runtime(sovereign-core)

add_dependencies(sovereign-core sovereign_gocore_dll)
add_custom_command(TARGET sovereign-core POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_CURRENT_BINARY_DIR}/sovereign-gocore.dll"
          "$<TARGET_FILE_DIR:sovereign-core>"
  VERBATIM
)

# wintun.dll (see tools/codegen/wintun.version) is a vendored binary, not
# something CMake builds — it's fetched into vendor/wintun/amd64/ the same
# way vendor/sing-box is (CI step / manual local fetch), never committed.
# sing-tun (inside GoCore's linked sing-box) loads it via LoadLibrary from
# the process's own directory at runtime, so it has to sit next to the exe
# just like sovereign-gocore.dll does.
add_custom_command(TARGET sovereign-core POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_SOURCE_DIR}/vendor/wintun/amd64/wintun.dll"
          "$<TARGET_FILE_DIR:sovereign-core>"
  VERBATIM
)
