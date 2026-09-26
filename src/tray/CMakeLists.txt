# The tray's decisions, apart from Win32 - linked by the tray and by
# tests/unit/tray_model_test.cpp.
add_library(sovereign_tray_lib STATIC
  tray_model.cpp
  tray_model.h
)
target_include_directories(sovereign_tray_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(sovereign-tray WIN32
  main.cpp
  icons.cpp
  icons.h
  pipe_client.cpp
  pipe_client.h
  settings.cpp
  settings.h
)

target_link_libraries(sovereign-tray PRIVATE sovereign_tray_lib sovereign_common shell32 user32 gdi32)
sovereign_copy_asan_runtime(sovereign-tray)
