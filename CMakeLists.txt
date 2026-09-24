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

# One copy of the ASan runtime per output directory, made by a single target
# every executable in that directory depends on. A POST_BUILD copy per
# executable raced when two of them sat in one directory (tests/unit): Ninja
# linked both in parallel and one link failed with "permission denied" on the
# DLL the other was copying at that moment (CI, d60f96a).
function(sovereign_copy_asan_runtime target)
  if (SOVEREIGN_ENABLE_ASAN AND MSVC)
    string(MAKE_C_IDENTIFIER "${CMAKE_CURRENT_BINARY_DIR}" dirId)
    string(MD5 dirHash "${dirId}")
    string(SUBSTRING "${dirHash}" 0 12 dirHash)
    set(copyTarget "sovereign_asan_runtime_${dirHash}")
    if (NOT TARGET ${copyTarget})
      set(copies "")
      foreach(dll IN LISTS SOVEREIGN_ASAN_RUNTIME_DLLS)
        get_filename_component(dllName "${dll}" NAME)
        add_custom_command(
          OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${dllName}"
          COMMAND ${CMAKE_COMMAND} -E copy_if_different "${dll}" "${CMAKE_CURRENT_BINARY_DIR}/${dllName}"
          DEPENDS "${dll}"
          VERBATIM)
        list(APPEND copies "${CMAKE_CURRENT_BINARY_DIR}/${dllName}")
      endforeach()
      add_custom_target(${copyTarget} DEPENDS ${copies})
    endif()
    add_dependencies(${target} ${copyTarget})
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
add_subdirectory(tests/unit)
