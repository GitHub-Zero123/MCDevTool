# mcdk_add_plugin(<target>
#     ID       com.example.my-plugin
#     VERSION  1.0.0
#     SOURCES  src/main.cpp ...
#     [OUTPUT_DIRECTORY <dir>]
# )
#
# 建立一个 MODULE 库，只导出入口符号，产物落到插件目录布局下。
function(mcdk_add_plugin target)
    cmake_parse_arguments(MCDK_PLUGIN "" "ID;VERSION;OUTPUT_DIRECTORY" "SOURCES" ${ARGN})

    if(NOT MCDK_PLUGIN_ID)
        message(FATAL_ERROR "mcdk_add_plugin(${target}): ID is required")
    endif()
    if(NOT MCDK_PLUGIN_SOURCES)
        message(FATAL_ERROR "mcdk_add_plugin(${target}): SOURCES is required")
    endif()
    if(NOT MCDK_PLUGIN_VERSION)
        set(MCDK_PLUGIN_VERSION "0.1.0")
    endif()
    if(NOT MCDK_PLUGIN_OUTPUT_DIRECTORY)
        set(MCDK_PLUGIN_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/${MCDK_PLUGIN_ID}")
    endif()

    add_library(${target} MODULE ${MCDK_PLUGIN_SOURCES})
    target_link_libraries(${target} PRIVATE mcdk::plugin-sdk)
    target_compile_features(${target} PRIVATE cxx_std_17)

    set_target_properties(${target} PROPERTIES
        # 只导出入口符号，其余一律隐藏，避免与宿主静态链接的同名第三方库冲突。
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
        PREFIX ""
        LIBRARY_OUTPUT_DIRECTORY "${MCDK_PLUGIN_OUTPUT_DIRECTORY}"
        RUNTIME_OUTPUT_DIRECTORY "${MCDK_PLUGIN_OUTPUT_DIRECTORY}"
    )

    # CRT 与宿主不一致是允许的：ABI 边界上不存在任何分配器穿越，两侧只释放
    # 自己分配的内存（docs/plugin-system/02-abi-contract.md §6）。这里只在
    # MSVC 上留一行说明，不做强制。
    if(MSVC)
        get_target_property(_mcdk_crt ${target} MSVC_RUNTIME_LIBRARY)
        if(NOT _mcdk_crt)
            set(_mcdk_crt "${CMAKE_MSVC_RUNTIME_LIBRARY} (inherited)")
        endif()
        message(STATUS "mcdk plugin '${MCDK_PLUGIN_ID}' CRT: ${_mcdk_crt} — 与宿主不一致是允许的")
    endif()

    # 插件声明需要用户自己写进 .mcdev.json：宿主不扫描目录，只加载显式声明过的
    # 插件（docs/plugin-system/06-loading.md §1）。这里把该贴的内容直接打出来。
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E echo
            "[mcdk] 插件已构建。把下面这行加进项目的 .mcdev.json 的 plugins 数组："
        COMMAND ${CMAKE_COMMAND} -E echo
            "       { \"enable\": true, \"path\": \"$<TARGET_FILE:${target}>\" }"
        VERBATIM
    )
endfunction()
