# ui-snapshot: the main window's pages drawn into PNGs with sample content
# (no desktop needed). As a test it proves every page lays out and draws
# without failing; CI keeps the pictures as the ui-snapshots artifact.

# tray.rc: the flags sprite the window draws.
add_executable(ui-snapshot ui_snapshot.cpp "${CMAKE_SOURCE_DIR}/src/tray/tray.rc")
target_link_libraries(ui-snapshot PRIVATE sovereign_tray_ui)
sovereign_copy_asan_runtime(ui-snapshot)
add_test(NAME ui-snapshot COMMAND ui-snapshot "${CMAKE_CURRENT_BINARY_DIR}/snapshots")
# Typing a configuration's name in place, in the real window (a desktop: CI,
# or SOVEREIGN_DESKTOP_TESTS=1 - else it says it's skipped).
add_test(NAME ui-edit-in-place COMMAND ui-snapshot --edit-test)
# Every page's layout as text: no clickable items overlapping, nothing past
# the window's edge, at the default width and the narrowest.
add_test(NAME ui-layout COMMAND ui-snapshot --layout-test "${CMAKE_CURRENT_BINARY_DIR}/layout")
