project(MC_DEV_TOOL_MCDK_STDIO_BRIDGE)

add_executable(mcdk_stdio_bridge main.cpp)
target_compile_features(mcdk_stdio_bridge PRIVATE cxx_std_20)
target_link_libraries(mcdk_stdio_bridge PRIVATE mcp)

target_include_directories(mcdk_stdio_bridge PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../mcdk/modules
)

if(MSVC)
    target_compile_definitions(mcdk_stdio_bridge PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
