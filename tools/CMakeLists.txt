project(MC_DEV_TOOLS)

option(MC_DEV_TOOL_BUILD_MCDK "BUILD MCDK EXECUTABLE" ON)
option(MC_DEV_TOOL_BUILD_MCDK_STDIO_BRIDGE "BUILD MCDK STDIO MCP BRIDGE EXECUTABLE" ON)

# MCDK Executable
if(MC_DEV_TOOL_BUILD_MCDK)
    add_subdirectory(mcdk)
endif()

# MCDK stdio MCP bridge executable
if(MC_DEV_TOOL_BUILD_MCDK_STDIO_BRIDGE)
    add_subdirectory(mcdk_stdio_bridge)
endif()
