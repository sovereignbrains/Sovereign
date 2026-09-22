cmake_minimum_required(VERSION 3.28)
project(sovereign LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Мы не используем C++20/23 модули; авто-сканирование под Ninja+MSVC
# добавляет в compile_commands.json @...modmap response-файлы, которых ещё
# нет на момент конфигурации — clang-tidy на них давится.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

if (MSVC)
  add_compile_options(/W4 /WX /permissive- /EHsc /utf-8)
  add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

option(SOVEREIGN_ENABLE_ASAN "Build with AddressSanitizer" OFF)
if (SOVEREIGN_ENABLE_ASAN AND MSVC)
  add_compile_options(/fsanitize=address)
  # /fsanitize=address линкуется с рантаймом ASan динамически; эта DLL лежит
  # рядом с cl.exe, а не в стандартном PATH — без явного копирования запуск
  # exe вне Developer Command Prompt падает с STATUS_DLL_NOT_FOUND.
  get_filename_component(SOVEREIGN_MSVC_BIN_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
  file(GLOB SOVEREIGN_ASAN_RUNTIME_DLLS
       "${SOVEREIGN_MSVC_BIN_DIR}/clang_rt.asan*dynamic-x86_64.dll")
endif()

function(sovereign_copy_asan_runtime target)
  if (SOVEREIGN_ENABLE_ASAN AND MSVC)
    foreach(dll IN LISTS SOVEREIGN_ASAN_RUNTIME_DLLS)
      add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${dll}" "$<TARGET_FILE_DIR:${target}>"
        VERBATIM)
    endforeach()
  endif()
endfunction()

option(SOVEREIGN_ENABLE_ANALYZE "Build with MSVC /analyze static analysis" OFF)
if (SOVEREIGN_ENABLE_ANALYZE AND MSVC)
  add_compile_options(/analyze /analyze:external-)
endif()

find_package(wil CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_library(sovereign_common INTERFACE)
target_include_directories(sovereign_common INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}/src/common
  ${CMAKE_CURRENT_SOURCE_DIR}/src/generated)
target_link_libraries(sovereign_common INTERFACE WIL::WIL nlohmann_json::nlohmann_json)

add_subdirectory(src/service)
add_subdirectory(src/tray)

enable_testing()
add_subdirectory(tests/golden)
