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

add_executable(hot_reload_filter_test hot_reload_filter_test.cpp)
target_compile_features(hot_reload_filter_test PRIVATE cxx_std_23)
target_link_libraries(hot_reload_filter_test PRIVATE mcdk_core)
add_test(NAME hot-reload-filters COMMAND hot_reload_filter_test)

add_executable(game_environment_test game_environment_test.cpp)
target_compile_features(game_environment_test PRIVATE cxx_std_23)
target_link_libraries(game_environment_test PRIVATE mcdk_core)
add_test(NAME game-environment COMMAND game_environment_test)

add_executable(project_operations_test project_operations_test.cpp)
target_compile_features(project_operations_test PRIVATE cxx_std_23)
target_link_libraries(project_operations_test PRIVATE mcdk_project_test_core)
add_test(NAME project-operations COMMAND project_operations_test)

if(TARGET mcdk AND MC_DEV_TOOL_MCDK_ENABLE_CLI)
    add_test(
        NAME project-cli-protocol
        COMMAND ${CMAKE_COMMAND}
            "-DMCDK_EXE=$<TARGET_FILE:mcdk>"
            "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/cli-project-fixture"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cli_project_test.cmake
    )
endif()

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
