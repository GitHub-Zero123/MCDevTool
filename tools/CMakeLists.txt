project(MC_DEV_TOOLS)

option(MC_DEV_TOOL_BUILD_MCDK "BUILD MCDK EXECUTABLE" ON)
option(MC_DEV_TOOL_BUILD_MCDK_API "BUILD MCDK C API SHARED LIBRARY" ON)
option(MC_DEV_TOOL_BUILD_MCDK_STDIO_BRIDGE "BUILD MCDK STDIO MCP BRIDGE EXECUTABLE" ON)

# MCDK Executable
if(MC_DEV_TOOL_BUILD_MCDK)
    add_subdirectory(mcdk)
endif()

# MCDK C API shared library
if(MC_DEV_TOOL_BUILD_MCDK_API)
    add_subdirectory(mcdk_api)
endif()

# MCDK stdio MCP bridge executable
if(MC_DEV_TOOL_BUILD_MCDK_STDIO_BRIDGE)
    add_subdirectory(mcdk_stdio_bridge)
endif()
