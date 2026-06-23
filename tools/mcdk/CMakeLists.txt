project(MC_DEV_TOOL_MCDK)

option(MC_DEV_TOOL_MCDK_ENABLE_CLI "BUILD CLI EXECUTABLE" OFF)

# add_subdirectory(libs/cpp-mcp)

add_executable(mcdk main.cpp)
target_compile_features(mcdk PRIVATE cxx_std_23)
# mcdk_core 已 PUBLIC 转发 mcdevtool + mcp + MCDEV_MOD_RESOURCE + include/modules 目录
target_link_libraries(mcdk PRIVATE mcdk_core)

if(MC_DEV_TOOL_MCDK_ENABLE_CLI)
    target_compile_definitions(mcdk PRIVATE MCDK_ENABLE_CLI)
    target_sources(mcdk PRIVATE cli.cpp)
endif()

if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_compile_options(mcdk PRIVATE
        -Wno-implicit-const-int-float-conversion
    )
endif()