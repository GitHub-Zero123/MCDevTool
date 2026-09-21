project(MC_DEV_TOOL_MCDK)

option(MC_DEV_TOOL_MCDK_ENABLE_CLI "BUILD CLI EXECUTABLE" OFF)

set(_mcdk_native_profiler_supported OFF)
if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(_mcdk_native_profiler_supported ON)
    add_subdirectory(
        "${CMAKE_CURRENT_LIST_DIR}/../../components/profiler/tracy-bridge"
        "${CMAKE_CURRENT_BINARY_DIR}/native-profiler-build"
    )
else()
    message(STATUS "mcdk native profiler component is unavailable on this platform (requires Windows x64)")
endif()

add_library(mcdev_profiler_core STATIC
    src/performance/native_bridge_loader.cpp
    src/performance/profiler_runtime_owner.cpp
    src/performance/profiler_service.cpp
    src/performance/profiler_types.cpp
)
target_compile_features(mcdev_profiler_core PUBLIC cxx_std_23)
target_include_directories(mcdev_profiler_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(WIN32)
    target_link_libraries(mcdev_profiler_core PUBLIC bcrypt iphlpapi ws2_32)
endif()

# Tool descriptions are shared with the forwarding-only stdio bridge.
# 只读清单，不加载任何动态库。mcdk 与 mcdk_stdio_bridge 共用——两份解析必然分叉，
# 而分叉的表现是 tools/list 列出的工具和实际注册的对不上。
add_library(mcdk_plugin_manifest STATIC
    src/plugin_host/manifest.cpp
    src/plugin_host/declarations.cpp
)
target_compile_features(mcdk_plugin_manifest PUBLIC cxx_std_23)
target_include_directories(mcdk_plugin_manifest PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(mcdk_plugin_manifest PUBLIC mcp mcdk::plugin-abi)

add_library(mcdk_mcp_tools STATIC src/mcp_tool_definitions.cpp)
target_compile_features(mcdk_mcp_tools PUBLIC cxx_std_20)
target_include_directories(mcdk_mcp_tools PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(mcdk_mcp_tools PUBLIC mcp)

add_library(mcdk_core STATIC
    src/config.cpp
    src/env.cpp
    src/game_environment.cpp
    src/hotreload.cpp
    src/ipc_code_execution.cpp
    src/json_diagnostics.cpp
    src/jsonui_debugger.cpp
    src/jsonui_reload_support.cpp
    src/level.cpp
    src/log_buffer.cpp
    src/mod_dir_config.cpp
    src/mod_register.cpp
    src/mc_input_mcp.cpp
    src/mc_profiler_mcp.cpp
    src/reload_code.cpp
    src/rpc_registry.cpp
    src/style_processor.cpp
    src/utils.cpp
    src/world_project.cpp
)
target_compile_features(mcdk_core PUBLIC cxx_std_23)
target_include_directories(mcdk_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(mcdk_core PUBLIC mcdevtool mcdev_profiler_core mcdk_mcp_tools mcdk_plugin_manifest mcp PRIVATE MCDEV_MOD_RESOURCE)

add_library(mcdk_runtime STATIC
    src/application.cpp
    src/console/console_output.cpp
    src/game_process.cpp
    src/game_process/logging.cpp
    src/game_process/platform.cpp
    src/host_bridge.cpp
    src/mcp_server.cpp
    src/plugin_host/abi_layout_check.cpp
    src/plugin_host/event_bus.cpp
    src/plugin_host/guard.cpp
    src/plugin_host/images.cpp
    src/plugin_cli.cpp
    src/plugin_host/host.cpp
    src/plugin_host/registry.cpp
    src/plugin_host/session_binding.cpp
    src/plugin_host/interfaces/console.cpp
    src/plugin_host/interfaces/core.cpp
    src/plugin_host/interfaces/events.cpp
    src/plugin_host/interfaces/game.cpp
    src/plugin_host/interfaces/info.cpp
    src/plugin_host/interfaces/log.cpp
    src/plugin_host/interfaces/mcp.cpp
    src/runtime/game_lifecycle.cpp
    src/runtime/mcp_tool_registry.cpp
    src/runtime/session.cpp
)
target_compile_features(mcdk_runtime PUBLIC cxx_std_23)

# 插件系统的总开关。关闭时所有发射点在预处理阶段就消失，连订阅计数的那次
# 原子读也不剩。PUBLIC：events.hpp 是公开头，宏的展开必须在所有使用方一致。
option(MCDK_ENABLE_PLUGINS "Enable the mcdk plugin system" ON)
target_compile_definitions(mcdk_runtime PUBLIC MCDK_ENABLE_PLUGINS=$<BOOL:${MCDK_ENABLE_PLUGINS}>)

# 插件 ABI 头的唯一真源在 sdk/plugin-sdk，宿主与插件 SDK 共用同一份。
target_link_libraries(mcdk_runtime PUBLIC mcdk_core mcdk::plugin-abi mcdk_plugin_manifest PRIVATE mcp MCDevLink::MCDevLink)
if(WIN32)
    target_link_libraries(mcdk_runtime PRIVATE ws2_32)
endif()

add_executable(mcdk main.cpp)
target_compile_features(mcdk PRIVATE cxx_std_23)
target_include_directories(mcdk PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/libs)
target_link_libraries(mcdk PRIVATE mcdk_runtime)
include(GNUInstallDirs)
install(TARGETS mcdk
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    COMPONENT mcdk-core
)

if(_mcdk_native_profiler_supported)
    set_target_properties(mcdev-tracy-bridge PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:mcdk>"
    )
    add_custom_target(mcdk_native_profiler_component
        COMMAND "${CMAKE_COMMAND}" -E make_directory
            "$<TARGET_FILE_DIR:mcdk>/licenses/native-profiler"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_PROPERTY:mcdev-tracy-bridge,MCDEV_TRACY_LICENSE_FILE>"
            "$<TARGET_FILE_DIR:mcdk>/licenses/native-profiler/LICENSE-Tracy.txt"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_PROPERTY:mcdev-tracy-bridge,MCDEV_CAPSTONE_LICENSE_FILE>"
            "$<TARGET_FILE_DIR:mcdk>/licenses/native-profiler/LICENSE-Capstone.txt"
        DEPENDS mcdev-tracy-bridge
        COMMENT "Synchronizing Native profiler licenses beside mcdk"
        VERBATIM
    )
    add_dependencies(mcdk mcdk_native_profiler_component)
endif()

if(MC_DEV_TOOL_MCDK_ENABLE_CLI)
    target_compile_definitions(mcdk PRIVATE MCDK_ENABLE_CLI)
    target_sources(mcdk PRIVATE src/cli.cpp)
endif()

if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_compile_options(mcdk PRIVATE
        -Wno-implicit-const-int-float-conversion
    )
endif()
