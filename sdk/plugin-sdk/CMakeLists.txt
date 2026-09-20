cmake_minimum_required(VERSION 3.24)

# 本目录可以被单独 clone / FetchContent / add_subdirectory，因此顶层构建时自带 project()。
if(PROJECT_IS_TOP_LEVEL OR NOT DEFINED PROJECT_NAME)
    project(mcdk-plugin-sdk VERSION 0.1.0 LANGUAGES C CXX)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(McdkAddPlugin)

# ------------------------------------------------------------------
# 第一层：ABI 头。宿主与 SDK 的唯一真源，物理上只有这一份。
# 纯 C99，除 <stddef.h> / <stdint.h> 外无依赖。
# ------------------------------------------------------------------
add_library(mcdk_plugin_abi INTERFACE)
add_library(mcdk::plugin-abi ALIAS mcdk_plugin_abi)
target_include_directories(mcdk_plugin_abi INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# ------------------------------------------------------------------
# 第二层：C++ SDK。随插件一起从源码编译，因此与插件必然同编译器、同标准库、
# 同 CRT —— 这正是它内部可以自由使用 std::string 等类型的前提。
# 它的职责就是把这些类型在边界上消化成 ABI 形态。
# ------------------------------------------------------------------
add_library(mcdk_plugin_sdk STATIC src/bootstrap.cpp)
add_library(mcdk::plugin-sdk ALIAS mcdk_plugin_sdk)
target_compile_features(mcdk_plugin_sdk PUBLIC cxx_std_17)
target_link_libraries(mcdk_plugin_sdk PUBLIC mcdk::plugin-abi)
set_target_properties(mcdk_plugin_sdk PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

option(MCDK_PLUGIN_SDK_BUILD_CHECKS "Build the ABI conformance checks" OFF)
option(MCDK_PLUGIN_SDK_BUILD_EXAMPLES "Build the example plugins" ${MCDK_PLUGIN_SDK_BUILD_CHECKS})

if(MCDK_PLUGIN_SDK_BUILD_CHECKS)
    # 这两个目标没有运行期行为，它们的价值全在「能不能编译过」上，
    # 因此挂进默认构建而不是 ctest —— ABI 破坏应当在构建期就拦住。

    # 1) abi/ 下除 abi_assert.h 外必须是纯 C。任何 C++ 构造（含不慎混入的
    #    std::string）都会让这个目标编译失败。
    #    注意：该约束只针对 abi/ 这一层，不针对上面的 C++ SDK。
    add_library(mcdk_abi_c99_check STATIC checks/abi_c99_check.c)
    set_target_properties(mcdk_abi_c99_check PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
    target_link_libraries(mcdk_abi_c99_check PRIVATE mcdk::plugin-abi)

    # 2) 结构体布局、平凡可复制性、枚举底层类型的静态断言。
    #    用 #ifdef __cplusplus 藏过上一道检查的 C++ 类型由它拦下。
    add_library(mcdk_abi_layout_check STATIC checks/abi_layout_check.cpp)
    target_compile_features(mcdk_abi_layout_check PRIVATE cxx_std_17)
    target_link_libraries(mcdk_abi_layout_check PRIVATE mcdk::plugin-abi)
endif()

if(MCDK_PLUGIN_SDK_BUILD_EXAMPLES)
    add_subdirectory(examples/01-hello)
endif()
