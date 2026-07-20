//
// Small geometry/math helpers not covered by GLM.
//

#ifndef YELLOWTAIL_MATHUTILS_H
#define YELLOWTAIL_MATHUTILS_H

#include <cmath>

#include <glm/glm.hpp>

namespace ytail::math {
    // Two unit vectors spanning the plane perpendicular to axis, forming the right-handed
    // basis (outU, outV, normalize(axis)). axis need not be normalized.
    inline void basisFromAxis(const glm::vec3& axis, glm::vec3& outU, glm::vec3& outV) {
        const glm::vec3 n = glm::normalize(axis);
        // Pick a reference not parallel to n so the cross product is well-conditioned.
        const glm::vec3 ref = std::abs(n.y) < 0.99f ? glm::vec3(0.0f, 1.0f, 0.0f)
                                                    : glm::vec3(1.0f, 0.0f, 0.0f);
        outU = glm::normalize(glm::cross(ref, n));
        outV = glm::cross(n, outU);
    }
} // ytail::math

#endif //YELLOWTAIL_MATHUTILS_H
