# The tray's decisions, apart from Win32 - linked by the tray and by
# tests/unit (tray_model_test, subscription_test, app_rules_test).
add_library(sovereign_tray_lib STATIC
  tray_model.cpp
  tray_model.h
  subscription.cpp
  subscription.h
  app_rules.cpp
  app_rules.h
)
target_include_directories(sovereign_tray_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(sovereign_tray_lib PUBLIC sovereign_common)

add_executable(sovereign-tray WIN32
  main.cpp
  fetch.cpp
  fetch.h
  icons.cpp
  icons.h
  pipe_client.cpp
  pipe_client.h
  settings.cpp
  settings.h
)

# The tray's User-Agent names the core it drives: the pinned sing-box version
# (same file the service's gocore build reads).
file(STRINGS "${CMAKE_SOURCE_DIR}/tools/codegen/sing-box.version" SOVEREIGN_TRAY_SINGBOX_TAG LIMIT_COUNT 1)
string(REGEX REPLACE "^v" "" SOVEREIGN_TRAY_SINGBOX_VERSION "${SOVEREIGN_TRAY_SINGBOX_TAG}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/tools/codegen/sing-box.version")
target_compile_definitions(sovereign-tray PRIVATE "SOVEREIGN_SINGBOX_VERSION_W=L\"${SOVEREIGN_TRAY_SINGBOX_VERSION}\"")

target_link_libraries(sovereign-tray PRIVATE sovereign_tray_lib sovereign_common shell32 user32 gdi32 winhttp iphlpapi ole32 uuid)
sovereign_copy_asan_runtime(sovereign-tray)
