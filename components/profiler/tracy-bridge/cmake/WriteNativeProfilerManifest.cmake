if(NOT DEFINED MCDEV_NATIVE_DLL OR NOT EXISTS "${MCDEV_NATIVE_DLL}")
    message(FATAL_ERROR "MCDEV_NATIVE_DLL does not identify a built bridge DLL")
endif()
if(NOT DEFINED MCDEV_NATIVE_MANIFEST OR MCDEV_NATIVE_MANIFEST STREQUAL "")
    message(FATAL_ERROR "MCDEV_NATIVE_MANIFEST is required")
endif()

file(SHA256 "${MCDEV_NATIVE_DLL}" _mcdev_native_sha256)
set(_mcdev_native_manifest_tmp "${MCDEV_NATIVE_MANIFEST}.tmp")
file(WRITE "${_mcdev_native_manifest_tmp}"
    "{\n"
    "  \"component\": \"native-profiler\",\n"
    "  \"bridge_api\": 1,\n"
    "  \"tracy_protocol\": \"0.11.1\",\n"
    "  \"platform\": \"windows\",\n"
    "  \"arch\": \"x64\",\n"
    "  \"sha256\": \"${_mcdev_native_sha256}\"\n"
    "}\n"
)
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${_mcdev_native_manifest_tmp}"
        "${MCDEV_NATIVE_MANIFEST}"
    RESULT_VARIABLE _mcdev_native_manifest_copy_result
)
file(REMOVE "${_mcdev_native_manifest_tmp}")
if(NOT _mcdev_native_manifest_copy_result EQUAL 0)
    message(FATAL_ERROR "Unable to synchronize the Native profiler component manifest")
endif()
