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
    set_default(false)
    set_showmenu(true)
    set_description("Enable CLI for MCDK")
option_end()

if is_plat("windows") then
    add_cxflags("/utf-8", "/EHsc")
    add_defines("_CRT_SECURE_NO_WARNINGS")
    add_defines("_HAS_CXX23=1")
end

if is_plat("linux") then
    add_cxxflags("-stdlib=libc++")
    add_defines("_LIBCPP_STD_VER=23")
end

includes("libs/nbt")

target("mcdevtool")
    set_kind("object")
    add_files(
        "src/env.cpp",
        "src/level.cpp",
        "src/addon.cpp",
        "src/utils.cpp",
        "src/reload.cpp",
        "src/debug.cpp",
        "src/style.cpp"
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

if has_config("build_mcdk") then
    target("mcdk")
        set_kind("binary")
        add_files("tools/mcdk/main.cpp")
        add_deps("mcdevtool", "mcdev_mod_resource")
        add_includedirs("mods/Resource")
        
        if has_config("mcdk_enable_cli") then
            add_defines("MCDK_ENABLE_CLI")
            add_files("tools/mcdk/cli.cpp")
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
end
