find_program(SOVEREIGN_GO_EXECUTABLE go REQUIRED)

# sovereign-gocore.dll (see gocore/) needs CGO, which on Windows means a
# MinGW-w64 gcc reachable on PATH — CC isn't hardcoded here so this works
# the same in CI and on a dev machine, as long as whoever runs
# `cmake --build` already has that gcc on PATH (matches how the rest of
# this project's toolchain setup works: presets assume a configured shell,
# not a self-contained one).
# Every .go file of the package: a change to any of them must rebuild the DLL.
file(GLOB SOVEREIGN_GOCORE_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/gocore/*.go")

# gocore is built the way upstream builds its Windows release of the pinned
# sing-box: the same build tags (release/DEFAULT_BUILD_TAGS_WINDOWS), the same
# linker flags (release/LDFLAGS) and the version stamped in. Without the tags,
# whole features are compiled out — REALITY needs with_utls, the gVisor TUN
# stack with_gvisor, QUIC-based protocols with_quic… — and the box fails at
# runtime with "… is not included in this build". Read from the vendored tree,
# they stay pinned together with the sing-box version instead of drifting in
# a copy here.
set(SOVEREIGN_SINGBOX_DIR "${CMAKE_SOURCE_DIR}/vendor/sing-box")
set(SOVEREIGN_SINGBOX_TAGS_FILE "${SOVEREIGN_SINGBOX_DIR}/release/DEFAULT_BUILD_TAGS_WINDOWS")
set(SOVEREIGN_SINGBOX_LDFLAGS_FILE "${SOVEREIGN_SINGBOX_DIR}/release/LDFLAGS")
set(SOVEREIGN_SINGBOX_VERSION_FILE "${CMAKE_SOURCE_DIR}/tools/codegen/sing-box.version")
if (NOT EXISTS "${SOVEREIGN_SINGBOX_TAGS_FILE}")
  message(FATAL_ERROR "vendor/sing-box is missing — fetch the pinned tag from tools/codegen/sing-box.version first")
endif()
file(READ "${SOVEREIGN_SINGBOX_TAGS_FILE}" SOVEREIGN_GOCORE_TAGS)
string(STRIP "${SOVEREIGN_GOCORE_TAGS}" SOVEREIGN_GOCORE_TAGS)
file(READ "${SOVEREIGN_SINGBOX_LDFLAGS_FILE}" SOVEREIGN_SINGBOX_LDFLAGS)
string(STRIP "${SOVEREIGN_SINGBOX_LDFLAGS}" SOVEREIGN_SINGBOX_LDFLAGS)
file(STRINGS "${SOVEREIGN_SINGBOX_VERSION_FILE}" SOVEREIGN_SINGBOX_TAG LIMIT_COUNT 1)
string(REGEX REPLACE "^v" "" SOVEREIGN_SINGBOX_VERSION "${SOVEREIGN_SINGBOX_TAG}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${SOVEREIGN_SINGBOX_TAGS_FILE}" "${SOVEREIGN_SINGBOX_LDFLAGS_FILE}" "${SOVEREIGN_SINGBOX_VERSION_FILE}")

add_custom_command(
  OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/sovereign-gocore.dll"
  COMMAND ${CMAKE_COMMAND} -E env CGO_ENABLED=1
          ${SOVEREIGN_GO_EXECUTABLE} build -buildmode=c-shared -trimpath
          -tags "${SOVEREIGN_GOCORE_TAGS}"
          "-ldflags=-X 'github.com/sagernet/sing-box/constant.Version=${SOVEREIGN_SINGBOX_VERSION}' ${SOVEREIGN_SINGBOX_LDFLAGS} -s -w -buildid="
          -o "${CMAKE_CURRENT_BINARY_DIR}/sovereign-gocore.dll" .
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/gocore"
  DEPENDS ${SOVEREIGN_GOCORE_SOURCES} "${CMAKE_SOURCE_DIR}/gocore/go.mod" "${CMAKE_SOURCE_DIR}/gocore/go.sum"
          "${SOVEREIGN_SINGBOX_TAGS_FILE}" "${SOVEREIGN_SINGBOX_LDFLAGS_FILE}" "${SOVEREIGN_SINGBOX_VERSION_FILE}"
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

# GoCore - the C++ side of the bridge to sovereign-gocore.dll. A library of
# its own so tests/unit can drive it against gocore-stub.dll under ASan: the
# real DLL's Go runtime can't live in an ASan process (issue #9).
add_library(sovereign_gocore_host STATIC
  go_core.cpp
  go_core.h
)
target_include_directories(sovereign_gocore_host PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(sovereign_gocore_host PUBLIC sovereign_service_lib sovereign_common)

add_executable(sovereign-core
  main.cpp
  pipe_server.cpp
  pipe_server.h
  trace.cpp
  trace.h
)

target_link_libraries(sovereign-core PRIVATE sovereign_gocore_host sovereign_service_lib sovereign_common advapi32)
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
