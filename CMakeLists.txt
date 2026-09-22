cmake_minimum_required(VERSION 3.20)
project(mslang VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(MSLANG_BUILD_TESTS "Build tests" ON)
option(MSLANG_BUILD_SHARED "Build shared library" OFF)
option(MSLANG_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(MSLANG_STRICT_WARNINGS "Treat warnings as errors" ON)

if(MSLANG_BUILD_SHARED)
  set(MSLANG_LIBRARY_TYPE SHARED)
else()
  set(MSLANG_LIBRARY_TYPE STATIC)
endif()

configure_file(
    "${PROJECT_SOURCE_DIR}/include/mslang/version.h.in"
    "${PROJECT_BINARY_DIR}/include/mslang/version.h"
    @ONLY)

function(mslang_apply_warnings target)
  if(MSLANG_STRICT_WARNINGS)
    if(MSVC)
      target_compile_options(${target} PRIVATE /W4 /WX)
    else()
      target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
  endif()
endfunction()

function(mslang_apply_asan target)
  if(MSLANG_ENABLE_ASAN AND NOT MSVC)
    target_compile_options(${target} PRIVATE -fsanitize=address)
    target_link_options(${target} PRIVATE -fsanitize=address)
  endif()
endfunction()

add_library(mslang ${MSLANG_LIBRARY_TYPE}
    src/ms_version.c
    src/core/ms_common.c
    src/core/ms_memory.c
    src/core/ms_diag.c)
set_target_properties(mslang PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
target_include_directories(mslang
    PUBLIC
        "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_BINARY_DIR}/include"
    PRIVATE
        "${PROJECT_SOURCE_DIR}/src")
mslang_apply_warnings(mslang)
mslang_apply_asan(mslang)

add_executable(mslang-cli src/cli/main.c)
target_link_libraries(mslang-cli PRIVATE mslang)
set_target_properties(mslang-cli PROPERTIES OUTPUT_NAME mslang)
mslang_apply_warnings(mslang-cli)
mslang_apply_asan(mslang-cli)

add_executable(embed-example examples/embed_demo.c)
target_link_libraries(embed-example PRIVATE mslang)
mslang_apply_warnings(embed-example)
mslang_apply_asan(embed-example)

if(MSLANG_BUILD_TESTS)
  enable_testing()
  function(mslang_add_c_test name)
    add_executable(${name} "tests/c/${name}.c")
    target_include_directories(${name} PRIVATE "${PROJECT_SOURCE_DIR}/tests/c")
    target_include_directories(${name} PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(${name} PRIVATE mslang)
    mslang_apply_warnings(${name})
    mslang_apply_asan(${name})
    if(MSVC)
      # do { ... } while (0) in ms_test.h triggers C4127 (constant conditional).
      target_compile_options(${name} PRIVATE /wd4127)
    endif()
    add_test(NAME ${name} COMMAND ${name})
  endfunction()
  mslang_add_c_test(test_ms_test)
  mslang_add_c_test(test_common)
  mslang_add_c_test(test_memory)
  mslang_add_c_test(test_diag)
endif()
