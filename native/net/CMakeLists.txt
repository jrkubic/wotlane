# ABOUTME: wotlane netcode with no game code: the wotm_net library (protocol, lockstep, client), the wotm_server server and tests.
# ABOUTME: Builds standalone (tools\build_net.bat) or inside the runtime (add_subdirectory in PS2Recomp's CMake, for wotm.exe).
cmake_minimum_required(VERSION 3.24)
project(wotm_net C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
    enet
    GIT_REPOSITORY "https://github.com/lsalzman/enet.git"
    GIT_TAG "v1.3.18"
)
FetchContent_MakeAvailable(enet)
if(WIN32)
    target_link_libraries(enet PUBLIC ws2_32 winmm)
endif()

find_package(Threads REQUIRED)   # the client runs on a thread; on Linux g++ needs -pthread

add_library(wotm_net STATIC wotm_net_client.cpp wotm_net_master.cpp wotm_net_server.cpp)
target_include_directories(wotm_net PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" "${enet_SOURCE_DIR}/include")
target_link_libraries(wotm_net PUBLIC enet Threads::Threads)
if(WIN32)
    target_compile_definitions(wotm_net PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

add_executable(wotm_net_test wotm_net_test.cpp)
target_link_libraries(wotm_net_test PRIVATE wotm_net)

add_executable(wotm_server wotm_server.cpp)
target_link_libraries(wotm_server PRIVATE wotm_net)
