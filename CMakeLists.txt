cmake_minimum_required(VERSION 3.15)

project(MC_DEV_TOOL_ROOT LANGUAGES CXX C)

option(MC_DEV_TOOL_BUILD_TEST "BUILD TEST EXECUTABLE" OFF)
option(MC_DEV_TOOL_BUILD_MCDK "BUILD MCDK EXECUTABLE" ON)
option(MC_DEV_TOOL_MCDK_ENABLE_CLI "BUILD CLI EXECUTABLE" OFF)

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

add_library(mcdevtool OBJECT
    src/env.cpp
    src/level.cpp
    src/addon.cpp
    src/utils.cpp
    src/reload.cpp
    src/debug.cpp
    src/style.cpp
)
target_compile_features(mcdevtool PRIVATE cxx_std_23)
target_compile_definitions(mcdevtool PRIVATE _CRT_SECURE_NO_WARNINGS)
target_include_directories(mcdevtool PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(mcdevtool PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/libs/nlohmann)
# target_include_directories(mcdevtool PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/libs/nbt/include)
target_link_libraries(mcdevtool PUBLIC NBT)

# MCDK Executable
if(MC_DEV_TOOL_BUILD_MCDK)
    add_executable(mcdk mcdk.cpp
        mods/Resource/INCLUDE_MOD.cpp
    )
    target_compile_features(mcdk PRIVATE cxx_std_23)
    target_link_libraries(mcdk PRIVATE mcdevtool)
    target_include_directories(mcdk PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/mods/Resource
    )

    if(MC_DEV_TOOL_MCDK_ENABLE_CLI)
        target_compile_definitions(mcdk PRIVATE MCDK_ENABLE_CLI)
        target_sources(mcdk PRIVATE mcdk_cli.cpp)
    endif()

    if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        target_compile_options(mcdk PRIVATE
            -Wno-implicit-const-int-float-conversion
        )
    endif()
endif()

# Test Executable
if(MC_DEV_TOOL_BUILD_TEST)
    add_subdirectory(tests)
endif()
