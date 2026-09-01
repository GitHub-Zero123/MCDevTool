if(NOT DEFINED MCDK_EXE OR NOT DEFINED TEST_ROOT)
    message(FATAL_ERROR "MCDK_EXE and TEST_ROOT are required")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
file(REMOVE
    "${TEST_ROOT}.preview-response.json"
    "${TEST_ROOT}.invalid-preview.json"
    "${TEST_ROOT}.uuid-preview.json"
)
file(MAKE_DIRECTORY "${TEST_ROOT}")
file(WRITE "${TEST_ROOT}/manifest.json" [=[
{
    "format_version": 2,
    "header": {
        "name": "CLI Fixture",
        "description": "CLI protocol test",
        "uuid": "2cfdaf6b-1c99-4a4b-bfa8-218e0f7584a1",
        "version": [1, 2, 3],
        "min_engine_version": [1, 20, 0]
    },
    "modules": [
        {
            "type": "data",
            "uuid": "758a07ab-9417-47ce-bef2-a442bb1cfebe",
            "version": [1, 2, 3]
        }
    ]
}
]=])

execute_process(
    COMMAND "${MCDK_EXE}" project --help
    RESULT_VARIABLE help_result
    OUTPUT_VARIABLE help_output
    ERROR_VARIABLE help_error
)
if(NOT help_result EQUAL 0)
    message(FATAL_ERROR "project --help returned ${help_result}, expected 0: ${help_error}")
endif()
string(FIND "${help_output}" "apply-preview" apply_preview_help)
if(apply_preview_help EQUAL -1)
    message(FATAL_ERROR "project --help does not list apply-preview: ${help_output}")
endif()
string(FIND "${help_output}" "export" export_help)
if(NOT export_help EQUAL -1)
    message(FATAL_ERROR "project --help still lists the removed export command: ${help_output}")
endif()

execute_process(
    COMMAND "${MCDK_EXE}" project bump-version --help
    RESULT_VARIABLE bump_help_result
    OUTPUT_VARIABLE bump_help_output
    ERROR_VARIABLE bump_help_error
)
if(NOT bump_help_result EQUAL 0)
    message(FATAL_ERROR "bump-version --help returned ${bump_help_result}: ${bump_help_error}")
endif()
foreach(required_flag IN ITEMS "--target" "--preview")
    string(FIND "${bump_help_output}" "${required_flag}" required_flag_index)
    if(required_flag_index EQUAL -1)
        message(FATAL_ERROR "bump-version --help does not list ${required_flag}: ${bump_help_output}")
    endif()
endforeach()

execute_process(
    COMMAND "${MCDK_EXE}" project inspect --root "${TEST_ROOT}" --target "${TEST_ROOT}" --json
    RESULT_VARIABLE inspect_result
    OUTPUT_VARIABLE inspect_output
    ERROR_VARIABLE inspect_error
)
if(NOT inspect_result EQUAL 0)
    message(FATAL_ERROR "project inspect returned ${inspect_result}: ${inspect_error}")
endif()
string(JSON inspect_protocol GET "${inspect_output}" protocol_version)
string(JSON inspect_ok GET "${inspect_output}" ok)
string(JSON inspect_operation GET "${inspect_output}" operation)
if(NOT inspect_protocol EQUAL 1
   OR NOT inspect_ok
   OR NOT inspect_operation STREQUAL "inspect")
    message(FATAL_ERROR "unexpected inspect response: ${inspect_output}")
endif()

execute_process(
    COMMAND "${MCDK_EXE}" project regenerate-uuids --root "${TEST_ROOT}" --json
    RESULT_VARIABLE confirm_result
    OUTPUT_VARIABLE confirm_output
    ERROR_VARIABLE confirm_error
)
if(NOT confirm_result EQUAL 1)
    message(FATAL_ERROR "regenerate-uuids without --yes returned ${confirm_result}, expected 1: ${confirm_error}")
endif()
string(JSON confirm_code GET "${confirm_output}" error code)
if(NOT confirm_code STREQUAL "confirmation_required")
    message(FATAL_ERROR "unexpected confirmation response: ${confirm_output}")
endif()

file(READ "${TEST_ROOT}/manifest.json" preview_source_before)
execute_process(
    COMMAND "${MCDK_EXE}" project bump-version
        --root "${TEST_ROOT}"
        --target "${TEST_ROOT}"
        --part patch
        --preview
        --json
    RESULT_VARIABLE preview_result
    OUTPUT_VARIABLE preview_output
    ERROR_VARIABLE preview_error
)
if(NOT preview_result EQUAL 0)
    message(FATAL_ERROR "bump-version --preview returned ${preview_result}: ${preview_error}")
endif()
file(READ "${TEST_ROOT}/manifest.json" preview_source_after)
if(NOT preview_source_after STREQUAL preview_source_before)
    message(FATAL_ERROR "bump-version --preview modified its source manifest")
endif()

string(JSON preview_protocol GET "${preview_output}" protocol_version)
string(JSON preview_ok GET "${preview_output}" ok)
string(JSON preview_operation GET "${preview_output}" operation)
string(JSON preview_id GET "${preview_output}" preview id)
string(JSON preview_mutation GET "${preview_output}" preview operation)
string(JSON preview_version_part GET "${preview_output}" preview version_part)
string(JSON preview_target_type TYPE "${preview_output}" preview target)
string(JSON preview_file_count LENGTH "${preview_output}" preview files)
string(JSON preview_before GET "${preview_output}" preview files 0 before)
string(JSON preview_after GET "${preview_output}" preview files 0 after)
string(JSON preview_opaque GET "${preview_output}" preview opaque_approval)
string(JSON preview_modified_count LENGTH "${preview_output}" modified_files)
string(JSON preview_archive_type TYPE "${preview_output}" archive_path)
if(NOT preview_protocol EQUAL 1
   OR NOT preview_ok
   OR NOT preview_operation STREQUAL "bump-version"
   OR preview_id STREQUAL ""
   OR NOT preview_mutation STREQUAL "bump-version"
   OR NOT preview_version_part STREQUAL "patch"
   OR NOT preview_target_type STREQUAL "STRING"
   OR NOT preview_file_count EQUAL 1
   OR preview_opaque STREQUAL ""
   OR NOT preview_modified_count EQUAL 0
   OR NOT preview_archive_type STREQUAL "NULL")
    message(FATAL_ERROR "unexpected preview response: ${preview_output}")
endif()
string(FIND "${preview_before}" "\"version\": [1, 2, 3]" preview_before_version)
string(FIND "${preview_after}" "\"version\": [1, 2, 4]" preview_after_version)
if(preview_before_version EQUAL -1 OR preview_after_version EQUAL -1)
    message(FATAL_ERROR "preview did not serialize full before/after contents: ${preview_output}")
endif()

set(preview_input "${TEST_ROOT}.preview-response.json")
file(WRITE "${preview_input}" "${preview_output}")
execute_process(
    COMMAND "${MCDK_EXE}" project apply-preview --root "${TEST_ROOT}" --json
    INPUT_FILE "${preview_input}"
    RESULT_VARIABLE apply_result
    OUTPUT_VARIABLE apply_output
    ERROR_VARIABLE apply_error
)
if(NOT apply_result EQUAL 0)
    message(FATAL_ERROR "apply-preview returned ${apply_result}: ${apply_error}")
endif()
string(JSON apply_operation GET "${apply_output}" operation)
string(JSON apply_patch GET "${apply_output}" project manifests 0 version 2)
string(JSON apply_preview_type TYPE "${apply_output}" preview)
string(JSON apply_modified_count LENGTH "${apply_output}" modified_files)
if(NOT apply_operation STREQUAL "apply-preview"
   OR NOT apply_patch EQUAL 4
   OR NOT apply_preview_type STREQUAL "NULL"
   OR NOT apply_modified_count EQUAL 1)
    message(FATAL_ERROR "unexpected apply-preview response: ${apply_output}")
endif()

execute_process(
    COMMAND "${MCDK_EXE}" project apply-preview --root "${TEST_ROOT}" --json
    INPUT_FILE "${preview_input}"
    RESULT_VARIABLE stale_result
    OUTPUT_VARIABLE stale_output
    ERROR_VARIABLE stale_error
)
if(NOT stale_result EQUAL 1)
    message(FATAL_ERROR "reused preview returned ${stale_result}, expected 1: ${stale_error}")
endif()
string(JSON stale_code GET "${stale_output}" error code)
if(NOT stale_code STREQUAL "preview_stale")
    message(FATAL_ERROR "unexpected stale preview response: ${stale_output}")
endif()

set(invalid_preview_input "${TEST_ROOT}.invalid-preview.json")
file(WRITE "${invalid_preview_input}" "{")
execute_process(
    COMMAND "${MCDK_EXE}" project apply-preview --root "${TEST_ROOT}" --json
    INPUT_FILE "${invalid_preview_input}"
    RESULT_VARIABLE invalid_preview_result
    OUTPUT_VARIABLE invalid_preview_output
    ERROR_VARIABLE invalid_preview_error
)
if(NOT invalid_preview_result EQUAL 1)
    message(FATAL_ERROR "invalid preview returned ${invalid_preview_result}, expected 1: ${invalid_preview_error}")
endif()
string(JSON invalid_preview_code GET "${invalid_preview_output}" error code)
if(NOT invalid_preview_code STREQUAL "invalid_preview")
    message(FATAL_ERROR "unexpected invalid preview response: ${invalid_preview_output}")
endif()

execute_process(
    COMMAND "${MCDK_EXE}" project regenerate-uuids
        --root "${TEST_ROOT}"
        --target "${TEST_ROOT}"
        --preview
        --json
    RESULT_VARIABLE uuid_preview_result
    OUTPUT_VARIABLE uuid_preview_output
    ERROR_VARIABLE uuid_preview_error
)
if(NOT uuid_preview_result EQUAL 0)
    message(FATAL_ERROR "regenerate-uuids --preview returned ${uuid_preview_result}: ${uuid_preview_error}")
endif()
string(JSON uuid_preview_operation GET "${uuid_preview_output}" preview operation)
string(JSON uuid_preview_part_type TYPE "${uuid_preview_output}" preview version_part)
if(NOT uuid_preview_operation STREQUAL "regenerate-uuids" OR NOT uuid_preview_part_type STREQUAL "NULL")
    message(FATAL_ERROR "unexpected UUID preview response: ${uuid_preview_output}")
endif()
string(JSON uuid_preview_object GET "${uuid_preview_output}" preview)
set(uuid_preview_input "${TEST_ROOT}.uuid-preview.json")
file(WRITE "${uuid_preview_input}" "${uuid_preview_object}")
execute_process(
    COMMAND "${MCDK_EXE}" project apply-preview --root "${TEST_ROOT}" --json
    INPUT_FILE "${uuid_preview_input}"
    RESULT_VARIABLE uuid_apply_result
    OUTPUT_VARIABLE uuid_apply_output
    ERROR_VARIABLE uuid_apply_error
)
if(NOT uuid_apply_result EQUAL 0)
    message(FATAL_ERROR "apply-preview object returned ${uuid_apply_result}: ${uuid_apply_error}")
endif()
string(JSON uuid_apply_operation GET "${uuid_apply_output}" operation)
if(NOT uuid_apply_operation STREQUAL "apply-preview")
    message(FATAL_ERROR "unexpected UUID apply response: ${uuid_apply_output}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "MCDK_TEST_FAIL_PROJECT_COMMIT_AFTER=1"
        "MCDK_TEST_MUTATE_PROJECT_SOURCE_BEFORE_COMMIT=1"
        "MCDK_TEST_MUTATE_PROJECT_SOURCE_AFTER_FIRST_CLAIM=1"
        "MCDK_TEST_PROBE_PROJECT_FILE_GUARDS=1"
        "${MCDK_EXE}" project bump-version --root "${TEST_ROOT}" --json
    RESULT_VARIABLE bump_result
    OUTPUT_VARIABLE bump_output
    ERROR_VARIABLE bump_error
)
if(NOT bump_result EQUAL 0)
    message(FATAL_ERROR "bump-version returned ${bump_result}: ${bump_error}")
endif()
string(JSON bumped_patch GET "${bump_output}" project manifests 0 version 2)
if(NOT bumped_patch EQUAL 5)
    message(FATAL_ERROR "default bump was not patch: ${bump_output}")
endif()
file(READ "${TEST_ROOT}/manifest.json" production_hook_content)
string(FIND "${production_hook_content}" "simulated external save" production_hook_marker)
if(NOT production_hook_marker EQUAL -1)
    message(FATAL_ERROR "production mcdk honored a project test mutation hook")
endif()

execute_process(
    COMMAND "${MCDK_EXE}" project regenerate-uuids --root "${TEST_ROOT}" --yes --json
    RESULT_VARIABLE uuid_result
    OUTPUT_VARIABLE uuid_output
    ERROR_VARIABLE uuid_error
)
if(NOT uuid_result EQUAL 0)
    message(FATAL_ERROR "regenerate-uuids --yes returned ${uuid_result}: ${uuid_error}")
endif()
string(JSON uuid_operation GET "${uuid_output}" operation)
if(NOT uuid_operation STREQUAL "regenerate-uuids")
    message(FATAL_ERROR "unexpected UUID response: ${uuid_output}")
endif()

file(REMOVE "${preview_input}" "${invalid_preview_input}" "${uuid_preview_input}")
