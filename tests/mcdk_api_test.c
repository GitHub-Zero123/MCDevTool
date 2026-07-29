#include "mcdk_api.h"

#include <stddef.h>
#include <string.h>

int main(void) {
    size_t             count = 0;
    const char* const* paths = mcdk_api_get_game_exe_paths(&count);
    if ((count == 0 && paths != NULL) || (count != 0 && paths == NULL)) {
        return 1;
    }

    for (size_t index = 0; index < count; ++index) {
        if (paths[index] == NULL || strlen(paths[index]) == 0) {
            return 2;
        }
    }

    return 0;
}
