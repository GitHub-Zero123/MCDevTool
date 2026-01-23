project(MC_DEV_TOOLS)

option(MC_DEV_TOOL_BUILD_MCDK "BUILD MCDK EXECUTABLE" ON)

# MCDK Executable
if(MC_DEV_TOOL_BUILD_MCDK)
    add_subdirectory(mcdk)
endif()
