cmake_minimum_required(VERSION 3.24)

project(mcdev-tracy-bridge VERSION 0.1.0 LANGUAGES C CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "mcdev-tracy-bridge currently supports Windows x64 only")
endif()

include(FetchContent)
include(GNUInstallDirs)

option(MCDEV_TRACY_BUILD_TESTS "Build the bridge API tests" ${PROJECT_IS_TOP_LEVEL})

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

if(MSVC)
    # The bridge and every static dependency linked into it must use one CRT mode.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set(_mcdev_tracy_revision 5d542dc09f3d9378d005092a4ad446bd405f819a)
set(_mcdev_capstone_revision 5cca00533dadfe53181f1de3525f859769f69b65)

# The game embeds Tracy 0.11.1. The server protocol must match it exactly.
FetchContent_Declare(
    tracy
    URL https://github.com/wolfpld/tracy/archive/${_mcdev_tracy_revision}.tar.gz
    URL_HASH SHA256=ab47dbd7c7f1364015f0fb2e5afe767bb86997783bbfdd6cb6d366b47f0a94ac
    DOWNLOAD_DIR "${MCDEV_DEPS_DOWNLOAD_CACHE}"
    DOWNLOAD_NAME "tracy-${_mcdev_tracy_revision}.tar.gz"
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE
    SOURCE_SUBDIR mcdev-no-upstream-build
)

set(CAPSTONE_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(CAPSTONE_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(CAPSTONE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CAPSTONE_BUILD_CSTOOL OFF CACHE BOOL "" FORCE)
set(CAPSTONE_BUILD_DIET OFF CACHE BOOL "" FORCE)
set(CAPSTONE_ARCHITECTURE_DEFAULT OFF CACHE BOOL "" FORCE)
set(CAPSTONE_X86_SUPPORT ON CACHE BOOL "" FORCE)
FetchContent_Declare(
    capstone
    URL https://github.com/capstone-engine/capstone/archive/${_mcdev_capstone_revision}.tar.gz
    URL_HASH SHA256=ca0ef4998ec8a682b519a65d75e83e0fff5b0797c6e28942059976379fad5bc4
    DOWNLOAD_DIR "${MCDEV_DEPS_DOWNLOAD_CACHE}"
    DOWNLOAD_NAME "capstone-${_mcdev_capstone_revision}.tar.gz"
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE
)

FetchContent_MakeAvailable(tracy capstone)

include(cmake/TracyServerMinimal.cmake)

add_library(mcdev-tracy-bridge SHARED
    src/api.cpp
    src/capture_session.cpp
    src/result_builder.cpp
)
target_compile_features(mcdev-tracy-bridge PRIVATE cxx_std_20)
target_compile_definitions(mcdev-tracy-bridge PRIVATE
    MCDEV_TRACY_BRIDGE_EXPORTS
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    NO_PARALLEL_SORT
)
target_include_directories(mcdev-tracy-bridge
    PUBLIC include
    PRIVATE src
)
target_link_libraries(mcdev-tracy-bridge PRIVATE mcdev-tracy-server)
set_target_properties(mcdev-tracy-bridge PROPERTIES
    OUTPUT_NAME "mcdev-tracy-bridge"
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    MCDEV_TRACY_LICENSE_FILE "${tracy_SOURCE_DIR}/LICENSE"
    MCDEV_CAPSTONE_LICENSE_FILE "${capstone_SOURCE_DIR}/LICENSE.TXT"
)

add_custom_command(TARGET mcdev-tracy-bridge POST_BUILD
    COMMAND "${CMAKE_COMMAND}"
        "-DMCDEV_NATIVE_DLL=$<TARGET_FILE:mcdev-tracy-bridge>"
        "-DMCDEV_NATIVE_MANIFEST=$<TARGET_FILE_DIR:mcdev-tracy-bridge>/component.json"
        -P "${CMAKE_CURRENT_LIST_DIR}/cmake/WriteNativeProfilerManifest.cmake"
    COMMENT "Generating the Native profiler component manifest"
    VERBATIM
)

if(MSVC)
    target_compile_options(mcdev-tracy-bridge PRIVATE /W4 /permissive- /EHsc /utf-8)
    target_link_options(mcdev-tracy-bridge PRIVATE $<$<CONFIG:Release>:/OPT:REF> $<$<CONFIG:Release>:/OPT:ICF>)
endif()

set(_mcdev_native_component_dir "${CMAKE_INSTALL_BINDIR}/components/native-profiler/windows-x64")
install(TARGETS mcdev-tracy-bridge
    RUNTIME DESTINATION "${_mcdev_native_component_dir}" COMPONENT native-profiler
    LIBRARY DESTINATION "${_mcdev_native_component_dir}" COMPONENT native-profiler
)
install(
    FILES "$<TARGET_FILE_DIR:mcdev-tracy-bridge>/component.json"
    DESTINATION "${_mcdev_native_component_dir}"
    COMPONENT native-profiler
)
install(
    FILES "${tracy_SOURCE_DIR}/LICENSE"
    DESTINATION "${_mcdev_native_component_dir}/licenses"
    RENAME LICENSE-Tracy.txt
    COMPONENT native-profiler
)
install(
    FILES "${capstone_SOURCE_DIR}/LICENSE.TXT"
    DESTINATION "${_mcdev_native_component_dir}/licenses"
    RENAME LICENSE-Capstone.txt
    COMPONENT native-profiler
)

if(MCDEV_TRACY_BUILD_TESTS)
    add_executable(mcdev-tracy-bridge-tests tests/api_tests.cpp)
    target_compile_features(mcdev-tracy-bridge-tests PRIVATE cxx_std_20)
    target_link_libraries(mcdev-tracy-bridge-tests PRIVATE mcdev-tracy-bridge)
    add_test(NAME mcdev-tracy-bridge-api COMMAND mcdev-tracy-bridge-tests)
endif()
