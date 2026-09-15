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
