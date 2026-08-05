#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#  ifdef MCDEV_TRACY_BRIDGE_EXPORTS
#    define MCDEV_TRACY_API __declspec(dllexport)
#  else
#    define MCDEV_TRACY_API __declspec(dllimport)
#  endif
#else
#  define MCDEV_TRACY_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef void* mcdev_tracy_handle;

enum mcdev_tracy_status {
    MCDEV_TRACY_CONNECTING = 1,
    MCDEV_TRACY_CAPTURING = 2,
    MCDEV_TRACY_FINALIZING = 3,
    MCDEV_TRACY_COMPLETED = 4,
    MCDEV_TRACY_FAILED = 5
};

MCDEV_TRACY_API uint32_t mcdev_tracy_api_version(void);
MCDEV_TRACY_API const char* mcdev_tracy_protocol_version(void);

MCDEV_TRACY_API mcdev_tracy_handle mcdev_tracy_start(
    const char* address_utf8,
    uint16_t port,
    uint32_t maximum_seconds,
    uint32_t memory_limit_percent,
    uint32_t maximum_zones,
    const char* trace_path_utf8
);
MCDEV_TRACY_API int32_t mcdev_tracy_get_status(mcdev_tracy_handle handle);
MCDEV_TRACY_API int32_t mcdev_tracy_stop(mcdev_tracy_handle handle);

// Sizes include the trailing NUL. A zero size means no value is available.
MCDEV_TRACY_API size_t mcdev_tracy_result_size(mcdev_tracy_handle handle);
MCDEV_TRACY_API int32_t mcdev_tracy_copy_result(mcdev_tracy_handle handle, char* destination, size_t capacity);
MCDEV_TRACY_API size_t mcdev_tracy_error_size(mcdev_tracy_handle handle);
MCDEV_TRACY_API int32_t mcdev_tracy_copy_error(mcdev_tracy_handle handle, char* destination, size_t capacity);
MCDEV_TRACY_API size_t mcdev_tracy_last_error_size(void);
MCDEV_TRACY_API int32_t mcdev_tracy_copy_last_error(char* destination, size_t capacity);

// Release requests cancellation and waits for this handle's worker thread.
MCDEV_TRACY_API int32_t mcdev_tracy_release(mcdev_tracy_handle handle);
MCDEV_TRACY_API void mcdev_tracy_shutdown_all(void);

#ifdef __cplusplus
}
#endif

