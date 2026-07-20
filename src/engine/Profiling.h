//
// Created by Peter Gilbert.
//

#ifndef YELLOWTAIL_PROFILING_H
#define YELLOWTAIL_PROFILING_H

// Single include point for Tracy. In non-profiling builds (TRACY_ENABLE off) every
// macro below no-ops, so instrumentation can stay in the code with zero cost.
//   ZoneScoped;          - times the enclosing scope, named after the function
//   ZoneScopedN("name"); - times the enclosing scope with an explicit name
//   FrameMark;           - marks a frame boundary in the main loop
#include <tracy/Tracy.hpp>

#endif //YELLOWTAIL_PROFILING_H
