-- MCDevTool xmake configuration
-- By Dofes

add_rules("mode.debug", "mode.release")

set_languages("c++20")

if is_plat("windows") then
    if is_mode("release") then
        set_runtimes("MT")
    else
        set_runtimes("MTd")
    end
end

add_repositories("groupmountain-repo https://github.com/GroupMountain/xmake-repo.git")

package("binarystream")
    set_homepage("https://github.com/GlacieTeam/BinaryStream")
    set_license("MPL-2.0")
    add_urls("https://github.com/GlacieTeam/BinaryStream/archive/refs/tags/v$(version).tar.gz")
    add_versions("2.3.2", "bd9fbb46948202a2b9c514d030aa1000988a9773fa4f6f3e98884333734e6349")
    on_install(function (package)
        io.replace("xmake.lua", "set_runtimes%(\"MD\"%)", "-- patched", {plain = false})
        import("package.tools.xmake").install(package)
        os.cp("include/*", package:installdir("include"))
    end)
package_end()

add_requires(
    "binarystream 2.3.2",
    "zlib 1.3.1"
)

option("build_test")
    set_default(false)
    set_showmenu(true)
    set_description("Build test executables")
option_end()

option("build_mcdk")
    set_default(true)
    set_showmenu(true)
    set_description("Build MCDK executable")
option_end()

option("mcdk_enable_cli")
    set_default(true)
    set_showmenu(true)
    set_description("Enable CLI for MCDK")
option_end()


if is_plat("windows") then
    add_cxflags("/utf-8", "/EHsc")
    add_defines("_CRT_SECURE_NO_WARNINGS")
    add_defines("UNICODE", "_UNICODE")
end

if is_plat("linux") then
    add_cxxflags("-stdlib=libc++")
    add_defines("_LIBCPP_STD_VER=23")
end

includes("libs/nbt")

target("mcp")
    set_kind("static")
    set_languages("c++17")
    add_files("libs/cpp-mcp/src/*.cpp")
    add_includedirs(
        "libs/cpp-mcp/include",
        "libs/cpp-mcp/common",
        "libs/nlohmann",
        {public = true}
    )
    if is_plat("windows") then
        add_syslinks("ws2_32", {public = true})
    end
target_end()

target("mcdevtool")
    set_kind("object")
    set_languages("c++23")
    add_files(
        "src/env.cpp",
        "src/level.cpp",
        "src/addon.cpp",
        "src/utils.cpp",
        "src/reload.cpp",
        "src/debug.cpp",
        "src/style.cpp",
        "src/game_discovery.cpp"
    )
    add_includedirs("include", {public = true})
    add_includedirs("libs/nlohmann", {public = true})
    add_includedirs("libs/nbt/include", {public = true})
    add_packages("binarystream", "zlib", {public = true})
    add_deps("NBT")
    
    if is_plat("windows") then
        add_syslinks("user32", "shell32", {public = true})
    end
target_end()

target("mcdev_mod_resource")
    set_kind("object")
    add_includedirs("mods/Resource", {public = true})
    
    on_load(function (target)
        local resfile = path.join(os.projectdir(), "mods/Resource/INCLUDE_MOD.cpp")
        local script = path.join(os.projectdir(), "mods/generate.py")
        
        local need_generate = not os.isfile(resfile)
        if not need_generate then
            local srcdir = path.join(os.projectdir(), "mods/INCLUDE_TEST_MOD")
            if os.isdir(srcdir) then
                local resmtime = os.mtime(resfile)
                -- 递归检查所有源文件的修改时间
                local srcfiles = os.files(path.join(srcdir, "**"))
                for _, srcfile in ipairs(srcfiles) do
                    if os.mtime(srcfile) > resmtime then
                        need_generate = true
                        break
                    end
                end
            end
        end
        
        if need_generate then
            cprint("${green}generating embedded resources...${clear}")
            local oldir = os.cd(path.join(os.projectdir(), "mods"))
            os.exec("python generate.py")
            os.cd(oldir)
        end
        
        if os.isfile(resfile) then
            target:add("files", resfile)
        end
    end)
target_end()

target("MCDevLink")
    set_kind("static")
    set_default(false)
    set_languages("c++20")
    add_files(
        "components/MCDevLink/src/Runtime.cpp",
        "components/MCDevLink/src/Detail/ProtocolFrame.cpp",
        "components/MCDevLink/src/Protocol/Safaia/SafaiaService.cpp"
    )
    add_includedirs("components/MCDevLink/include", {public = true})
    add_includedirs(
        "components/MCDevLink/src",
        "components/MCDevLink/third_party/asio/include",
        "libs/nlohmann"
    )
    add_defines("ASIO_STANDALONE", "ASIO_NO_DEPRECATED")
    if is_plat("windows") then
        add_defines("WIN32_LEAN_AND_MEAN", "NOMINMAX")
        add_syslinks("iphlpapi", "ws2_32", {public = true})
    end
target_end()

if has_config("build_mcdk") then
    target("mcdk_core")
        set_kind("static")
        set_languages("c++23")
        add_files(
            "tools/mcdk/src/config.cpp",
            "tools/mcdk/src/env.cpp",
            "tools/mcdk/src/game_environment.cpp",
            "tools/mcdk/src/hotreload.cpp",
            "tools/mcdk/src/ipc_code_execution.cpp",
            "tools/mcdk/src/json_diagnostics.cpp",
            "tools/mcdk/src/jsonui_debugger.cpp",
            "tools/mcdk/src/jsonui_reload_support.cpp",
            "tools/mcdk/src/level.cpp",
            "tools/mcdk/src/log_buffer.cpp",
            "tools/mcdk/src/mcp_tool_definitions.cpp",
            "tools/mcdk/src/mc_profiler_mcp.cpp",
            "tools/mcdk/src/mod_dir_config.cpp",
            "tools/mcdk/src/mod_register.cpp",
            "tools/mcdk/src/performance/native_bridge_loader.cpp",
            "tools/mcdk/src/performance/profiler_runtime_owner.cpp",
            "tools/mcdk/src/performance/profiler_service.cpp",
            "tools/mcdk/src/performance/profiler_types.cpp",
            "tools/mcdk/src/project_operations.cpp",
            "tools/mcdk/src/reload_code.cpp",
            "tools/mcdk/src/rpc_registry.cpp",
            "tools/mcdk/src/style_processor.cpp",
            "tools/mcdk/src/utils.cpp",
            "tools/mcdk/src/world_project.cpp"
        )
        add_includedirs("tools/mcdk/include", {public = true})
        add_deps("mcdevtool", "mcp", "mcdev_mod_resource")
        if is_plat("windows") then
            add_syslinks("bcrypt", "gdi32", "iphlpapi", "ws2_32", {public = true})
        end
    target_end()

    if has_config("build_test") then
        target("mcdk_project_test_core")
            set_kind("static")
            set_languages("c++23")
            add_files(
                "tools/mcdk/src/project_operations.cpp"
            )
            add_includedirs("tools/mcdk/include", {public = true})
            add_defines("MCDK_PROJECT_TEST_HOOKS")
            add_deps("mcdevtool")
            if is_plat("windows") then
                add_syslinks("bcrypt", {public = true})
            end
        target_end()
    end

    target("mcdk_runtime")
        set_kind("static")
        set_languages("c++23")
        add_files(
            "tools/mcdk/src/application.cpp",
            "tools/mcdk/src/console/console_output.cpp",
            "tools/mcdk/src/game_process.cpp",
            "tools/mcdk/src/game_process/logging.cpp",
            "tools/mcdk/src/game_process/platform.cpp",
            "tools/mcdk/src/host_bridge.cpp",
            "tools/mcdk/src/mcp_server.cpp"
        )
        add_deps("mcdk_core", "mcp", "MCDevLink")
        if is_plat("windows") then
            add_syslinks("ws2_32")
        end
    target_end()

    target("mcdk")
        set_kind("binary")
        set_languages("c++23")
        add_files("tools/mcdk/main.cpp")
        add_deps("mcdk_runtime")
        add_includedirs("tools/mcdk/libs")

        if has_config("mcdk_enable_cli") then
            add_defines("MCDK_ENABLE_CLI")
            add_files("tools/mcdk/src/cli.cpp")
        end

        if is_plat("windows") then
            on_load(function (target)
                if target:toolchain("clang") or target:toolchain("clang-cl") then
                    target:add("cxflags", "-Wno-implicit-const-int-float-conversion")
                end
            end)
        end
    target_end()
end

if has_config("build_test") then
    target("test1")
        set_kind("binary")
        set_languages("c++20")
        add_files("tests/test1.cpp")
    target_end()

    target("test2")
        set_kind("binary")
        add_files("tests/test2.cpp")
        add_deps("mcdevtool")
    target_end()

    target("test3")
        set_kind("binary")
        add_files("tests/test3.cpp")
    target_end()

    if has_config("build_mcdk") then
        target("rpc_registry_test")
            set_kind("binary")
            set_languages("c++23")
            add_files("tests/rpc_registry_test.cpp")
            add_deps("mcdk_core")
        target_end()

        target("host_bridge_test")
            set_kind("binary")
            set_languages("c++23")
            add_files("tests/host_bridge_test.cpp")
            add_deps("mcdk_runtime")
            if is_plat("windows") then
                add_syslinks("ws2_32")
            end
        target_end()

        target("project_operations_test")
            set_kind("binary")
            set_languages("c++23")
            add_files("tests/project_operations_test.cpp")
            add_deps("mcdk_project_test_core")
        target_end()

    end

end
