cmake_minimum_required(VERSION 3.28)
project(sovereign LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (MSVC)
  add_compile_options(/W4 /WX /permissive- /EHsc /utf-8 /Zc:preprocessor)
  add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

option(SOVEREIGN_ENABLE_ASAN "Build with AddressSanitizer" OFF)
if (SOVEREIGN_ENABLE_ASAN AND MSVC)
  add_compile_options(/fsanitize=address)
endif()

find_package(wil CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_library(sovereign_common INTERFACE)
target_include_directories(sovereign_common INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src/common)
target_link_libraries(sovereign_common INTERFACE WIL::WIL nlohmann_json::nlohmann_json)

add_subdirectory(src/service)
add_subdirectory(src/tray)
