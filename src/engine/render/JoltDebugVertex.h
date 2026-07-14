//
// Created by Peter Gilbert on 7/14/26.
//

#ifndef YELLOWTAIL_JOLTDEBUGVERTEX_H
#define YELLOWTAIL_JOLTDEBUGVERTEX_H

#include <glm/glm.hpp>

namespace ytail {
    // One vertex of Jolt debug geometry (lines/triangles). Kept Jolt-free so the physics manager can
    // expose spans of these without dragging Jolt headers into its consumers. Layout matches the GPU
    // debug pipeline: FLOAT3 position + FLOAT4 color.
    struct JoltDebugVertex {
        glm::vec3 position;
        glm::vec4 color;
    };
} // ytail

#endif //YELLOWTAIL_JOLTDEBUGVERTEX_H
