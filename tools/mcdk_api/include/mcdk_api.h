#ifndef MCDK_API_H
#define MCDK_API_H

#include <stddef.h>

#if defined(_WIN32)
#    if defined(MCDK_API_BUILD)
#        define MCDK_API_EXPORT __declspec(dllexport)
#    else
#        define MCDK_API_EXPORT __declspec(dllimport)
#    endif
#elif defined(__GNUC__) || defined(__clang__)
#    define MCDK_API_EXPORT __attribute__((visibility("default")))
#else
#    define MCDK_API_EXPORT
#endif

#if defined(__cplusplus)
#    define MCDK_API_NOEXCEPT noexcept
extern "C" {
#else
#    define MCDK_API_NOEXCEPT
#endif

/*
 * Returns all discovered executable paths as null-terminated UTF-8 strings.
 *
 * The paths are ordered by version from newest to oldest. The returned array
 * and strings are owned by the DLL, remain valid until the DLL is unloaded,
 * and must not be modified or freed by the caller. Returns NULL when no game
 * executable is found or discovery fails. When count is not NULL, it always
 * receives the number of returned paths.
 */
MCDK_API_EXPORT const char* const* mcdk_api_get_game_exe_paths(size_t* count) MCDK_API_NOEXCEPT;

#if defined(__cplusplus)
}
#endif

#endif
