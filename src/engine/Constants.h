//
// Created by Peter Gilbert on 7/16/26.
//

#ifndef YELLOWTAIL_CONSTANTS_H
#define YELLOWTAIL_CONSTANTS_H
#include <glm/vec3.hpp>

namespace ytail::constant {
    // Engine world-space basis. Right-handed, Y up, looking down -Z.
    inline constexpr glm::vec3 WorldRight{1.f, 0.f, 0.f};
    inline constexpr glm::vec3 WorldUp{0.f, 1.f, 0.f};
    inline constexpr glm::vec3 WorldForward{0.f, 0.f, -1.f};
} // ytail::constant

#endif //YELLOWTAIL_CONSTANTS_H
