# ABOUTME: Unit tests for the pure parts of the native layer (no game, no window), built separately from the runtime.
# ABOUTME: tools\test_native.bat configures in native\tests\build, builds and runs them.
cmake_minimum_required(VERSION 3.24)
project(wotm_native_tests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(wotm_native_test wotm_native_test.cpp ../wotm_detmath.cpp)
target_include_directories(wotm_native_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
# the same float rules as the runtime's native layer: every operation rounded on its own (wotm_detmath.hpp's bits depend on it)
if(MSVC)
    target_compile_options(wotm_native_test PRIVATE /fp:precise)
else()
    target_compile_options(wotm_native_test PRIVATE -ffp-contract=off -fno-fast-math)
endif()
