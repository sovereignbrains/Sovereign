add_executable(sovereign-tray WIN32
  main.cpp
)

target_link_libraries(sovereign-tray PRIVATE sovereign_common shell32)
sovereign_copy_asan_runtime(sovereign-tray)
