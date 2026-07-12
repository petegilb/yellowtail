// Only TU that defines CLAY_IMPLEMENTATION. Renderer not wired; see README "Clay UI".

#define CLAY_IMPLEMENTATION
#include <clay.h>

#include "clay_impl.h"

#include <stdio.h>

void yt_clay_handle_errors(Clay_ErrorData error_data) {
    // Clay strings are length-prefixed slices, not null-terminated.
    fprintf(stderr, "[clay] %.*s\n",
            (int)error_data.errorText.length,
            error_data.errorText.chars);
}
