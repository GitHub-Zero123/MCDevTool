cmake_minimum_required(VERSION 3.24)

project(mcdev-lane-shim VERSION 0.1.0 LANGUAGES C CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "mcdev-lane-shim currently supports Windows x64 only")
endif()

include(FetchContent)
include(GNUInstallDirs)

if(NOT MCDEV_DEPS_DOWNLOAD_CACHE)
    if(DEFINED ENV{MCDEV_DEPS_DOWNLOAD_CACHE} AND NOT "$ENV{MCDEV_DEPS_DOWNLOAD_CACHE}" STREQUAL "")
        set(_mcdev_default_download_cache "$ENV{MCDEV_DEPS_DOWNLOAD_CACHE}")
    else()
        set(_mcdev_default_download_cache "${CMAKE_SOURCE_DIR}/.cache/mcdev-deps/downloads")
    endif()
    set(
        MCDEV_DEPS_DOWNLOAD_CACHE
        "${_mcdev_default_download_cache}"
        CACHE PATH
        "Shared cache for immutable third-party source archives"
        FORCE
    )
endif()
file(MAKE_DIRECTORY "${MCDEV_DEPS_DOWNLOAD_CACHE}")
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)

# MinHook v1.3.4。只编译需要的源文件，不走上游的 CMake。
set(_mcdev_minhook_revision c3fcafdc10146beb5919319d0683e44e3c30d537)
FetchContent_Declare(
    minhook
    URL https://github.com/TsudaKageyu/minhook/archive/${_mcdev_minhook_revision}.tar.gz
    URL_HASH SHA256=31757833475be039c16359d4e276e718db6ba2ba61a6b870053a112106015822
    DOWNLOAD_DIR "${MCDEV_DEPS_DOWNLOAD_CACHE}"
    DOWNLOAD_NAME "minhook-${_mcdev_minhook_revision}.tar.gz"
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE
    SOURCE_SUBDIR mcdev-no-upstream-build
)
FetchContent_MakeAvailable(minhook)

add_library(mcdev-lane-shim SHARED
    src/shim.cpp
    "${minhook_SOURCE_DIR}/src/buffer.c"
    "${minhook_SOURCE_DIR}/src/hook.c"
    "${minhook_SOURCE_DIR}/src/trampoline.c"
    "${minhook_SOURCE_DIR}/src/hde/hde64.c"
)
target_compile_features(mcdev-lane-shim PRIVATE cxx_std_20)
target_include_directories(mcdev-lane-shim
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${minhook_SOURCE_DIR}/include"
)
target_link_libraries(mcdev-lane-shim PRIVATE ole32 shell32)
if(MSVC)
    # 注入进游戏进程，不能指望游戏旁边有匹配的 CRT。
    set_target_properties(mcdev-lane-shim PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
set_target_properties(mcdev-lane-shim PROPERTIES
    MCDEV_MINHOOK_LICENSE_FILE "${minhook_SOURCE_DIR}/LICENSE.txt"
)

# 协议头文件也给 mcdk 宿主用。
add_library(mcdev-lane-protocol INTERFACE)
target_include_directories(mcdev-lane-protocol INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")

install(TARGETS mcdev-lane-shim
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT mcdk-core
    LIBRARY DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT mcdk-core
)
install(
    FILES "${minhook_SOURCE_DIR}/LICENSE.txt"
    DESTINATION "${CMAKE_INSTALL_BINDIR}/licenses/lane"
    RENAME LICENSE-MinHook.txt
    COMPONENT mcdk-core
)
