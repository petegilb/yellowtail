//
// Created for debug gizmo rendering.
//

#include "DebugDraw.h"

#include <cmath>

#include <glm/gtc/constants.hpp>

#include "../utils/MathUtils.h"

namespace ytail {
    void DebugDraw::line(const glm::vec3& a, const glm::vec3& b, const glm::vec4& color) {
        lines.push_back({ a, color });
        lines.push_back({ b, color });
    }

    void DebugDraw::arrow(const glm::vec3& from, const glm::vec3& to, const glm::vec4& color, float headSize) {
        line(from, to, color);

        const glm::vec3 shaft = to - from;
        const float length = glm::length(shaft);
        if (length < 1e-5f) return;

        const glm::vec3 dir = shaft / length;
        const float head = headSize > 0.0f ? headSize : length * 0.2f;

        glm::vec3 u, v;
        math::basisFromAxis(dir, u, v);

        // Four barbs from the tip angled back along the shaft.
        const glm::vec3 base = to - dir * head;
        line(to, base + u * head * 0.5f, color);
        line(to, base - u * head * 0.5f, color);
        line(to, base + v * head * 0.5f, color);
        line(to, base - v * head * 0.5f, color);
    }

    void DebugDraw::circle(const glm::vec3& center, const glm::vec3& axis, float radius,
                           const glm::vec4& color, int segments) {
        if (segments < 3 || radius <= 0.0f) return;

        glm::vec3 u, v;
        math::basisFromAxis(axis, u, v);

        glm::vec3 prev = center + u * radius;
        for (int i = 1; i <= segments; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(segments) * glm::two_pi<float>();
            const glm::vec3 next = center + (u * std::cos(t) + v * std::sin(t)) * radius;
            line(prev, next, color);
            prev = next;
        }
    }

    void DebugDraw::wireSphere(const glm::vec3& center, float radius, const glm::vec4& color, int segments) {
        circle(center, glm::vec3(1.0f, 0.0f, 0.0f), radius, color, segments);
        circle(center, glm::vec3(0.0f, 1.0f, 0.0f), radius, color, segments);
        circle(center, glm::vec3(0.0f, 0.0f, 1.0f), radius, color, segments);
    }

    void DebugDraw::box(const glm::vec3& center, const glm::vec3& halfExtents, const glm::vec4& color) {
        const glm::vec3& h = halfExtents;
        // Eight corners, indexed by sign bits (x = bit0, y = bit1, z = bit2).
        glm::vec3 c[8];
        for (int i = 0; i < 8; ++i) {
            c[i] = center + glm::vec3((i & 1) ? h.x : -h.x,
                                      (i & 2) ? h.y : -h.y,
                                      (i & 4) ? h.z : -h.z);
        }
        // 12 edges: pairs of corners differing in exactly one axis bit.
        static const int edges[12][2] = {
            {0,1},{2,3},{4,5},{6,7},  // along x
            {0,2},{1,3},{4,6},{5,7},  // along y
            {0,4},{1,5},{2,6},{3,7},  // along z
        };
        for (const auto& e : edges) line(c[e[0]], c[e[1]], color);
    }
} // ytail
