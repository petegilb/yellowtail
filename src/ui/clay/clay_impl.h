#pragma once
// C-linkage entry points into Clay. Full usage in README "Clay UI".

#include <clay.h>

#ifdef __cplusplus
extern "C" {
#endif

// Pass to Clay_Initialize via Clay_ErrorHandler. Logs errors to stderr.
void yt_clay_handle_errors(Clay_ErrorData error_data);

#ifdef __cplusplus
}
#endif
