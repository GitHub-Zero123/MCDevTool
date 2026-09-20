cmake_minimum_required(VERSION 3.15)

project(MC_DEV_TOOL_TEST LANGUAGES CXX C)

add_executable(test1 test1.cpp)
target_compile_features(test1 PRIVATE cxx_std_20)


add_executable(test2 test2.cpp)
target_compile_features(test2 PRIVATE cxx_std_23)
target_link_libraries(test2 PRIVATE mcdevtool)

add_executable(test3 test3.cpp)
target_compile_features(test3 PRIVATE cxx_std_23)

add_executable(mcp_server_test mcp_server_test.cpp)
target_compile_features(mcp_server_test PRIVATE cxx_std_23)
target_link_libraries(mcp_server_test PRIVATE mcdevtool mcp)

add_executable(captureTest captureTest.cpp)
target_compile_features(captureTest PRIVATE cxx_std_23)
target_link_libraries(captureTest PRIVATE mcdevtool)

if(WIN32)
    # 需要交互式桌面及可用的 WGC / OpenGL 环境，按需运行。
    add_executable(window_capture_test window_capture_test.cpp)
    target_compile_features(window_capture_test PRIVATE cxx_std_23)
    target_link_libraries(window_capture_test PRIVATE mcdevtool opengl32 gdi32)

    # 会抢占前台并移动鼠标，同样按需手动运行。
    add_executable(window_input_test window_input_test.cpp)
    target_compile_features(window_input_test PRIVATE cxx_std_23)
    target_link_libraries(window_input_test PRIVATE mcdevtool)
endif()

add_executable(hot_reload_filter_test hot_reload_filter_test.cpp)
target_compile_features(hot_reload_filter_test PRIVATE cxx_std_23)
target_link_libraries(hot_reload_filter_test PRIVATE mcdk_core)
add_test(NAME hot-reload-filters COMMAND hot_reload_filter_test)

add_executable(game_environment_test game_environment_test.cpp)
target_compile_features(game_environment_test PRIVATE cxx_std_23)
target_link_libraries(game_environment_test PRIVATE mcdk_core)
add_test(NAME game-environment COMMAND game_environment_test)

add_executable(mc_input_test mc_input_test.cpp)
target_compile_features(mc_input_test PRIVATE cxx_std_23)
target_link_libraries(mc_input_test PRIVATE mcdk_core)
add_test(NAME mc-input COMMAND mc_input_test)

add_executable(rpc_registry_test rpc_registry_test.cpp)
target_compile_features(rpc_registry_test PRIVATE cxx_std_23)
target_link_libraries(rpc_registry_test PRIVATE mcdk_core)
add_test(NAME rpc-registry COMMAND rpc_registry_test)

add_executable(mcp_tool_registry_test mcp_tool_registry_test.cpp)
target_compile_features(mcp_tool_registry_test PRIVATE cxx_std_23)
target_link_libraries(mcp_tool_registry_test PRIVATE mcdk_runtime)
add_test(NAME mcp-tool-registry COMMAND mcp_tool_registry_test)

# 端到端：真的去 LoadLibrary 示例插件，走完整的 C ABI 握手。
# MCDK_ENABLE_PLUGINS=OFF 时插件压根不会被加载，这组测试也就不适用。
if(TARGET hello_plugin AND MCDK_ENABLE_PLUGINS)
    add_executable(plugin_host_test plugin_host_test.cpp)
    target_compile_features(plugin_host_test PRIVATE cxx_std_23)
    target_link_libraries(plugin_host_test PRIVATE mcdk_runtime)
    add_dependencies(plugin_host_test hello_plugin)
    target_compile_definitions(plugin_host_test PRIVATE
        MCDEV_TEST_HELLO_PLUGIN="$<TARGET_FILE:hello_plugin>"
    )
    add_test(NAME plugin-host COMMAND plugin_host_test)
endif()

# 零插件开销基准。它是唯一会因为「变慢了」而失败的测试，故意留在常规 ctest 里
# 而不是另开一个只有 CI 跑的靶——本地改坏了当场就能看到。
add_executable(plugin_event_bench plugin_event_bench.cpp)
target_compile_features(plugin_event_bench PRIVATE cxx_std_23)
target_link_libraries(plugin_event_bench PRIVATE mcdk_runtime)
add_test(NAME plugin-event-bench COMMAND plugin_event_bench)

add_executable(profiler_foundation_test profiler_foundation_test.cpp)
target_compile_features(profiler_foundation_test PRIVATE cxx_std_23)
target_link_libraries(profiler_foundation_test PRIVATE mcdk_core)
add_test(NAME profiler-foundation COMMAND profiler_foundation_test)
if(TARGET mcdk_native_profiler_component)
    add_dependencies(profiler_foundation_test mcdk_native_profiler_component)
    target_compile_definitions(profiler_foundation_test PRIVATE
        MCDEV_TEST_MCDK_DIR="$<TARGET_FILE_DIR:mcdk>"
    )
endif()

add_executable(host_bridge_test host_bridge_test.cpp)
target_compile_features(host_bridge_test PRIVATE cxx_std_23)
target_link_libraries(host_bridge_test PRIVATE mcdk_runtime)
if(WIN32)
    target_link_libraries(host_bridge_test PRIVATE ws2_32)
endif()

if(TARGET mcdk-api)
    add_executable(mcdk_api_test mcdk_api_test.c)
    target_link_libraries(mcdk_api_test PRIVATE mcdk-api)
endif()

# ABI 一致性套件：异常屏障双向验证 + 多实例回归。
if(TARGET abi_conformance_plugin AND MCDK_ENABLE_PLUGINS)
    add_executable(plugin_abi_conformance_test plugin_abi_conformance_test.cpp)
    target_compile_features(plugin_abi_conformance_test PRIVATE cxx_std_23)
    target_link_libraries(plugin_abi_conformance_test PRIVATE mcdk_runtime)
    add_dependencies(plugin_abi_conformance_test abi_conformance_plugin)
    target_compile_definitions(plugin_abi_conformance_test PRIVATE
        MCDEV_TEST_CONFORMANCE_PLUGIN="$<TARGET_FILE:abi_conformance_plugin>"
    )
    add_test(NAME plugin-abi-conformance COMMAND plugin_abi_conformance_test)

    # 同时加载全部工具链产物。本地只有一种工具链，CI 通过
    # MCDEV_TEST_PLUGIN_PATHS 把五种产物的路径一起交进来。
    add_executable(plugin_load_all_test plugin_load_all_test.cpp)
    target_compile_features(plugin_load_all_test PRIVATE cxx_std_23)
    target_link_libraries(plugin_load_all_test PRIVATE mcdk_runtime)
    add_dependencies(plugin_load_all_test abi_conformance_plugin)
    target_compile_definitions(plugin_load_all_test PRIVATE
        MCDEV_TEST_CONFORMANCE_PLUGIN="$<TARGET_FILE:abi_conformance_plugin>"
    )
    add_test(NAME plugin-load-all COMMAND plugin_load_all_test)
endif()

# 加载器模式：一个 DLL 充当其他插件的宿主（多实例 + 动态身份）。
if(TARGET script_host_plugin AND MCDK_ENABLE_PLUGINS)
    add_executable(plugin_loader_pattern_test plugin_loader_pattern_test.cpp)
    target_compile_features(plugin_loader_pattern_test PRIVATE cxx_std_23)
    target_link_libraries(plugin_loader_pattern_test PRIVATE mcdk_runtime)
    add_dependencies(plugin_loader_pattern_test script_host_plugin)
    target_compile_definitions(plugin_loader_pattern_test PRIVATE
        MCDEV_TEST_LOADER_PLUGIN="$<TARGET_FILE:script_host_plugin>"
    )
    add_test(NAME plugin-loader-pattern COMMAND plugin_loader_pattern_test)

    # 清单解析与依赖拓扑排序。复用 02-loader：它按 config 报出动态身份，
    # 同一个 DLL 声明多次就能扮演多个身份不同的插件。
    add_executable(plugin_manifest_test plugin_manifest_test.cpp)
    target_compile_features(plugin_manifest_test PRIVATE cxx_std_23)
    target_link_libraries(plugin_manifest_test PRIVATE mcdk_runtime)
    add_dependencies(plugin_manifest_test script_host_plugin)
    target_compile_definitions(plugin_manifest_test PRIVATE
        MCDEV_TEST_SCRIPT_HOST_PLUGIN="$<TARGET_FILE:script_host_plugin>"
    )
    add_test(NAME plugin-manifest COMMAND plugin_manifest_test)
endif()

# 游戏生命周期状态机。纯逻辑，不需要游戏也不需要插件。
add_executable(game_lifecycle_test game_lifecycle_test.cpp)
target_compile_features(game_lifecycle_test PRIVATE cxx_std_23)
target_link_libraries(game_lifecycle_test PRIVATE mcdk_runtime)
add_test(NAME game-lifecycle COMMAND game_lifecycle_test)
