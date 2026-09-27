# tray.rc: the app icon.
enable_language(RC)

# The tray's decisions, apart from Win32 - linked by the tray and by
# tests/unit (tray_model_test, subscription_test, app_rules_test,
# protocol_choice_test, log_lines_test, delays_test).
add_library(sovereign_tray_lib STATIC
  tray_model.cpp
  tray_model.h
  subscription.cpp
  subscription.h
  app_rules.cpp
  app_rules.h
  protocol_choice.cpp
  protocol_choice.h
  log_lines.cpp
  log_lines.h
  cache_file.cpp
  cache_file.h
  delays.cpp
  delays.h
)
target_include_directories(sovereign_tray_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(sovereign_tray_lib PUBLIC sovereign_common)

# The windows: the flyout and the main window, drawn with Direct2D - linked by
# the tray and by tests/ui's snapshot tool.
add_library(sovereign_tray_ui STATIC
  flyout.cpp
  flyout.h
  main_window.cpp
  main_window.h
  icons.cpp
  icons.h
  ui_content.h
  ui_style.h
)
target_include_directories(sovereign_tray_ui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(sovereign_tray_ui PUBLIC sovereign_tray_lib sovereign_common user32 gdi32 ole32 d2d1 dwrite dwmapi shcore windowscodecs)

add_executable(sovereign-tray WIN32
  main.cpp
  autostart.cpp
  autostart.h
  fetch.cpp
  fetch.h
  pipe_client.cpp
  pipe_client.h
  settings.cpp
  settings.h
  resource.h
  tray.rc
)

# The tray's User-Agent names the core it drives: the pinned sing-box version
# (same file the service's gocore build reads).
file(STRINGS "${CMAKE_SOURCE_DIR}/tools/codegen/sing-box.version" SOVEREIGN_TRAY_SINGBOX_TAG LIMIT_COUNT 1)
string(REGEX REPLACE "^v" "" SOVEREIGN_TRAY_SINGBOX_VERSION "${SOVEREIGN_TRAY_SINGBOX_TAG}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/tools/codegen/sing-box.version")
target_compile_definitions(sovereign-tray PRIVATE "SOVEREIGN_SINGBOX_VERSION_W=L\"${SOVEREIGN_TRAY_SINGBOX_VERSION}\"")

target_link_libraries(sovereign-tray PRIVATE sovereign_tray_ui sovereign_tray_lib sovereign_common shell32 user32 gdi32 winhttp iphlpapi ole32 uuid)
sovereign_copy_asan_runtime(sovereign-tray)
