add_executable(sovereign-core
  main.cpp
  pipe_server.cpp
  pipe_server.h
)

target_link_libraries(sovereign-core PRIVATE sovereign_common advapi32)
