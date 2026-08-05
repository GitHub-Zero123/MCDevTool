project(MC_DEV_TOOL_MCDK)

option(MC_DEV_TOOL_MCDK_ENABLE_CLI "BUILD CLI EXECUTABLE" OFF)

add_library(mcdev_profiler_core STATIC
    src/performance/profiler_runtime_owner.cpp
    src/performance/profiler_types.cpp
)
target_compile_features(mcdev_profiler_core PUBLIC cxx_std_23)
target_include_directories(mcdev_profiler_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_library(mcdk_core STATIC
    src/config.cpp
    src/env.cpp
    src/hotreload.cpp
    src/ipc_code_execution.cpp
    src/json_diagnostics.cpp
    src/jsonui_debugger.cpp
    src/jsonui_reload_support.cpp
    src/level.cpp
    src/log_buffer.cpp
    src/mcp_tool_definitions.cpp
    src/mod_dir_config.cpp
    src/mod_register.cpp
    src/mc_profiler_mcp.cpp
    src/reload_code.cpp
    src/rpc_registry.cpp
    src/style_processor.cpp
    src/utils.cpp
    src/world_project.cpp
)
target_compile_features(mcdk_core PUBLIC cxx_std_23)
target_include_directories(mcdk_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(mcdk_core PUBLIC mcdevtool mcdev_profiler_core mcp PRIVATE MCDEV_MOD_RESOURCE)

add_library(mcdk_runtime STATIC
    src/application.cpp
    src/game_process.cpp
    src/host_bridge.cpp
    src/mcp_server.cpp
)
target_compile_features(mcdk_runtime PUBLIC cxx_std_23)
target_link_libraries(mcdk_runtime PUBLIC mcdk_core PRIVATE mcp)
if(WIN32)
    target_link_libraries(mcdk_runtime PRIVATE ws2_32)
endif()

add_executable(mcdk main.cpp)
target_compile_features(mcdk PRIVATE cxx_std_23)
target_include_directories(mcdk PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/libs)
target_link_libraries(mcdk PRIVATE mcdk_runtime)

if(MC_DEV_TOOL_MCDK_ENABLE_CLI)
    target_compile_definitions(mcdk PRIVATE MCDK_ENABLE_CLI)
    target_sources(mcdk PRIVATE src/cli.cpp)
endif()

if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_compile_options(mcdk PRIVATE
        -Wno-implicit-const-int-float-conversion
    )
endif()
