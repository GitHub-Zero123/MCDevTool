project(MC_DEV_TOOL_MCDK_STDIO_BRIDGE)

add_executable(mcdk_stdio_bridge main.cpp discovery.cpp)
target_compile_features(mcdk_stdio_bridge PRIVATE cxx_std_20)
target_link_libraries(mcdk_stdio_bridge PRIVATE mcdk_core mcp)
include(GNUInstallDirs)
install(TARGETS mcdk_stdio_bridge
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    COMPONENT mcdk-core
)

target_include_directories(mcdk_stdio_bridge PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../mcdk/include
)

if(MSVC)
    target_compile_definitions(mcdk_stdio_bridge PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
