add_executable(sovereign-tray WIN32
  main.cpp
)

target_link_libraries(sovereign-tray PRIVATE sovereign_common shell32)
