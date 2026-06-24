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

# target_include_directories(mcdevtool PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/libs/nlohmann)
# target_include_directories(mcdevtool PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/libs/nbt/include)
target_link_libraries(mcdevtool PUBLIC NBT)

add_subdirectory(mods)

# ===================== mcdk_core 库 =====================
# 把原先写死在 tools/mcdk/main.cpp 的启动逻辑封装为可复用 STATIC 库，
# 允许其他业务以纯内存 config 启动开发测试世界，而不依赖磁盘 .mcdev.json。
# 默认全套链（mcdevtool + mcp + MCDEV_MOD_RESOURCE），可通过 option 关闭后两者。
# 注意：必须在 add_subdirectory(tools) 之前定义，因为 tools/mcdk 的 mcdk target 会链接 mcdk_core。
option(MCDK_CORE_ENABLE_MCP "Enable MCP server support in mcdk_core" ON)
option(MCDK_CORE_ENABLE_MOD_RESOURCE "Enable embedded debug mod resource in mcdk_core" ON)

add_library(mcdk_core STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/launch_config.cpp
)
target_compile_features(mcdk_core PUBLIC cxx_std_23)
target_compile_definitions(mcdk_core PRIVATE _CRT_SECURE_NO_WARNINGS)
# 对外暴露：
#   - include/（mcdk_core 公共头 core.h/launcher.h）
#   - tools/mcdk/modules（console.hpp 等类型，供 launcher.h 的 #include "console.hpp" 解析）
target_include_directories(mcdk_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/mcdk/modules
)
# PRIVATE：仅 core.cpp 自身需要，解析 #include "modules/xxx.hpp"（相对 tools/mcdk 父目录）
target_include_directories(mcdk_core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/mcdk
)
# mcdevtool 携带 NBT（含 zlib+binarystream）
target_link_libraries(mcdk_core PUBLIC mcdevtool)

if(MCDK_CORE_ENABLE_MCP)
    target_link_libraries(mcdk_core PUBLIC mcp)
    target_compile_definitions(mcdk_core PUBLIC MCDK_CORE_HAS_MCP=1)
endif()

if(MCDK_CORE_ENABLE_MOD_RESOURCE)
    target_link_libraries(mcdk_core PUBLIC MCDEV_MOD_RESOURCE)
    target_include_directories(mcdk_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/mods/Resource)
    target_compile_definitions(mcdk_core PUBLIC MCDK_CORE_HAS_MOD_RESOURCE=1)
endif()

if(WIN32)
    # style.cpp(dwmapi/gdiplus) / debug.cpp(ws2_32) / env.cpp(kernel32) 依赖
    target_link_libraries(mcdk_core PUBLIC user32 shell32 dwmapi gdiplus ws2_32)
endif()

add_subdirectory(tools)

# Test Executable
if(MC_DEV_TOOL_BUILD_TEST)
    message(STATUS "Building mcdevtool test executable")
    add_subdirectory(tests)
endif()