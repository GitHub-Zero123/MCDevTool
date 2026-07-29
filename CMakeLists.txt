cmake_minimum_required(VERSION 3.15)

project(MC_DEV_TOOL_ROOT LANGUAGES CXX C)

option(MC_DEV_TOOL_BUILD_TEST "BUILD TEST EXECUTABLE" OFF)

# By Zero123
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    if(MSVC)
        add_compile_options(/utf-8 /EHsc)
        # set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Release>:>")
        if(CMAKE_BUILD_TYPE STREQUAL "Release")
            add_compile_options(/MT)
        endif()
    endif()
endif()

add_subdirectory(libs/nbt)
# set(LIBUV_BUILD_SHARED OFF)
# add_subdirectory(libs/libuv)
# add_subdirectory(libs/uvw)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/libs/nlohmann)
add_subdirectory(libs/cpp-mcp)

add_library(mcdevtool_utils STATIC src/utils.cpp)
set_target_properties(mcdevtool_utils PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_features(mcdevtool_utils PRIVATE cxx_std_23)
target_include_directories(mcdevtool_utils PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_library(mcdevtool_game_discovery STATIC src/game_discovery.cpp)
set_target_properties(mcdevtool_game_discovery PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_features(mcdevtool_game_discovery PRIVATE cxx_std_23)
target_include_directories(mcdevtool_game_discovery PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(mcdevtool_game_discovery PUBLIC mcdevtool_utils)

add_library(mcdevtool OBJECT
    src/env.cpp
    src/level.cpp
    src/addon.cpp
    src/reload.cpp
    src/debug.cpp
    src/style.cpp
)
set_target_properties(mcdevtool PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_features(mcdevtool PRIVATE cxx_std_23)
target_compile_definitions(mcdevtool PRIVATE _CRT_SECURE_NO_WARNINGS)
target_include_directories(mcdevtool PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# target_include_directories(mcdevtool PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/libs/nlohmann)
# target_include_directories(mcdevtool PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/libs/nbt/include)
target_link_libraries(mcdevtool PUBLIC NBT mcdevtool_game_discovery)

add_subdirectory(mods)
add_subdirectory(tools)

# Test Executable
if(MC_DEV_TOOL_BUILD_TEST)
    message(STATUS "Building mcdevtool test executable")
    add_subdirectory(tests)
endif()
