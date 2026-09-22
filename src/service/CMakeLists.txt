find_program(SOVEREIGN_GO_EXECUTABLE go REQUIRED)

# sovereign-gocore.dll (see gocore/) needs CGO, which on Windows means a
# MinGW-w64 gcc reachable on PATH — CC isn't hardcoded here so this works
# the same in CI and on a dev machine, as long as whoever runs
# `cmake --build` already has that gcc on PATH (matches how the rest of
# this project's toolchain setup works: presets assume a configured shell,
# not a self-contained one).
add_custom_command(
  OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/sovereign-gocore.dll"
  COMMAND ${CMAKE_COMMAND} -E env CGO_ENABLED=1
          ${SOVEREIGN_GO_EXECUTABLE} build -buildmode=c-shared
          -o "${CMAKE_CURRENT_BINARY_DIR}/sovereign-gocore.dll" .
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/gocore"
  DEPENDS "${CMAKE_SOURCE_DIR}/gocore/main.go" "${CMAKE_SOURCE_DIR}/gocore/go.mod" "${CMAKE_SOURCE_DIR}/gocore/go.sum"
  COMMENT "Building sovereign-gocore.dll (Go c-shared)"
  VERBATIM
)
add_custom_target(sovereign_gocore_dll DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/sovereign-gocore.dll")

add_executable(sovereign-core
  main.cpp
  pipe_server.cpp
  pipe_server.h
  go_core.cpp
  go_core.h
)

target_link_libraries(sovereign-core PRIVATE sovereign_common advapi32)
sovereign_copy_asan_runtime(sovereign-core)

add_dependencies(sovereign-core sovereign_gocore_dll)
add_custom_command(TARGET sovereign-core POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_CURRENT_BINARY_DIR}/sovereign-gocore.dll"
          "$<TARGET_FILE_DIR:sovereign-core>"
  VERBATIM
)
