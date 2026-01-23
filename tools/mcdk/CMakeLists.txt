project(MC_DEV_TOOL_MCDK)

option(MC_DEV_TOOL_MCDK_ENABLE_CLI "BUILD CLI EXECUTABLE" OFF)

add_executable(mcdk main.cpp)
target_compile_features(mcdk PRIVATE cxx_std_23)
target_link_libraries(mcdk PRIVATE mcdevtool)
target_link_libraries(mcdk PRIVATE MCDEV_MOD_RESOURCE)
target_include_directories(mcdk PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/mods/Resource
)

if(MC_DEV_TOOL_MCDK_ENABLE_CLI)
    target_compile_definitions(mcdk PRIVATE MCDK_ENABLE_CLI)
    target_sources(mcdk PRIVATE cli.cpp)
endif()

if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_compile_options(mcdk PRIVATE
        -Wno-implicit-const-int-float-conversion
    )
endif()