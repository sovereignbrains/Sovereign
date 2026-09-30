# ThreadSanitizer build of the service's control plane (P3, issue #5) - its own
# project, configured directly on Linux by the CI's tsan job; the Windows build
# never sees it (TSan doesn't exist for MSVC):
#
#   cmake -S tests/tsan -B build/tsan -G Ninja -DCMAKE_CXX_COMPILER=clang++
#   cmake --build build/tsan && ctest --test-dir build/tsan --output-on-failure
#
# What's in it is exactly what is shared between threads in the service:
# ControlHandler (the pipe thread and the SCM's power-event thread) and LogRing
# (the core's threads append, the pipe thread reads). Their unit tests run too,
# and stress_test.cpp makes all of those threads race each other. The Windows
# jobs hold the code to /W4 /WX; here warnings are shown, not fatal - this
# build is about data races.
cmake_minimum_required(VERSION 3.28)
project(sovereign_tsan LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(nlohmann_json 3.11 REQUIRED)

set(root "${CMAKE_CURRENT_SOURCE_DIR}/../..")
add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g -O1 -Wall -Wextra)
add_link_options(-fsanitize=thread)

add_library(control_plane STATIC
  "${root}/src/service/control.cpp"
  "${root}/src/service/log_ring.cpp")
target_include_directories(control_plane PUBLIC
  "${root}/src/service" "${root}/src/common" "${root}/tests/unit")
target_link_libraries(control_plane PUBLIC nlohmann_json::nlohmann_json)

enable_testing()
foreach(test IN ITEMS control_test log_ring_test)
  add_executable(tsan-${test} "${root}/tests/unit/${test}.cpp")
  target_link_libraries(tsan-${test} PRIVATE control_plane)
  add_test(NAME tsan-${test} COMMAND tsan-${test})
endforeach()

add_executable(tsan-stress stress_test.cpp)
target_link_libraries(tsan-stress PRIVATE control_plane)
add_test(NAME tsan-stress COMMAND tsan-stress)

# Any report fails the test, not just a crash.
set_tests_properties(tsan-control_test tsan-log_ring_test tsan-stress PROPERTIES
  ENVIRONMENT "TSAN_OPTIONS=halt_on_error=1 exitcode=66 second_deadlock_stack=1")
