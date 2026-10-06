# tray.rc: the app icon.
enable_language(RC)

# The tray's decisions, apart from Win32 - linked by the tray and by
# tests/unit (tray_model_test, subscription_test, app_rules_test,
# protocol_choice_test, log_lines_test, delays_test, update_test,
# config_sync_test, exit_ip_test, log_level_test, share_links_test,
# import_formats_test, qr_codes_test, profiles_test, effective_config_test).
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
  effective_config.cpp
  effective_config.h
  delays.cpp
  delays.h
  update.cpp
  update.h
  config_sync.cpp
  config_sync.h
  exit_ip.cpp
  exit_ip.h
  log_level.cpp
  log_level.h
  share_links.cpp
  share_links.h
  profiles.cpp
  profiles.h
  combine.cpp
  combine.h
  routing.cpp
  routing.h
  diagnostics.cpp
  diagnostics.h
  support_report.cpp
  support_report.h
  import_formats.cpp
  import_formats.h
  qr_codes.cpp
  qr_codes.h
  flags.h
  warp.cpp
  warp.h
  relay.cpp
  relay.h
  x25519.cpp
  x25519.h
)
target_include_directories(sovereign_tray_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(sovereign_tray_lib PUBLIC sovereign_common PRIVATE ZXing::ZXing yaml-cpp::yaml-cpp)

# The window, drawn with Direct2D - linked by
# the tray and by tests/ui's snapshot tool.
add_library(sovereign_tray_ui STATIC
  main_window.cpp
  main_window.h
  icons.cpp
  icons.h
  ui_content.h
  ui_style.h
)
target_include_directories(sovereign_tray_ui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(sovereign_tray_ui PUBLIC sovereign_tray_lib sovereign_common user32 gdi32 comctl32 ole32 shell32 d2d1 dwrite dwmapi shcore windowscodecs)

# Pictures to read QR codes from: the clipboard, image files (WIC), the screen -
# linked by the tray and by tests/unit's capture test.
add_library(sovereign_tray_capture STATIC
  capture.cpp
  capture.h
)
target_include_directories(sovereign_tray_capture PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(sovereign_tray_capture PUBLIC sovereign_tray_lib sovereign_common user32 gdi32 shell32 shlwapi ole32 windowscodecs)

# The tray's files in %LOCALAPPDATA%\Sovereign (settings, profiles) - linked by
# the tray and by tests/unit's settings test.
add_library(sovereign_tray_settings STATIC
  settings.cpp
  settings.h
)
target_include_directories(sovereign_tray_settings PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(sovereign_tray_settings PUBLIC sovereign_tray_lib sovereign_common crypt32)

# The tray's requests over the network: subscriptions, downloads, the checks -
# linked by the tray and by tests/unit's diagnose-probe.
add_library(sovereign_tray_net STATIC
  fetch.cpp
  fetch.h
  diagnose.cpp
  diagnose.h
)
target_include_directories(sovereign_tray_net PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(sovereign_tray_net PUBLIC sovereign_tray_lib sovereign_common winhttp iphlpapi ws2_32 dnsapi)

add_executable(sovereign-tray WIN32
  main.cpp
  autostart.cpp
  autostart.h
  pipe_client.cpp
  pipe_client.h
  updater.cpp
  updater.h
  resource.h
  tray.rc
)

# The tray's User-Agent names the core it drives: the pinned sing-box version
# (same file the service's gocore build reads).
file(STRINGS "${CMAKE_SOURCE_DIR}/tools/codegen/sing-box.version" SOVEREIGN_TRAY_SINGBOX_TAG LIMIT_COUNT 1)
string(REGEX REPLACE "^v" "" SOVEREIGN_TRAY_SINGBOX_VERSION "${SOVEREIGN_TRAY_SINGBOX_TAG}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/tools/codegen/sing-box.version")
target_compile_definitions(sovereign-tray PRIVATE "SOVEREIGN_SINGBOX_VERSION_W=L\"${SOVEREIGN_TRAY_SINGBOX_VERSION}\""
                                                  "SOVEREIGN_VERSION_W=L\"${SOVEREIGN_VERSION}\"")

target_link_libraries(sovereign-tray PRIVATE sovereign_tray_ui sovereign_tray_capture sovereign_tray_settings sovereign_tray_net sovereign_tray_lib sovereign_common shell32 user32 gdi32 winhttp iphlpapi ole32 uuid ws2_32 dnsapi)
sovereign_copy_asan_runtime(sovereign-tray)
