//
// Reusable builder for world-space debug line geometry (arrows, wire spheres, boxes, ...).
// Accumulates JoltDebugVertex line pairs that a DebugLineRenderer uploads and draws with the
// DebugLine pipeline. Use it for editor gizmos and one-off visualizations (e.g. sphere casts).
//

#ifndef YELLOWTAIL_DEBUGDRAW_H
#define YELLOWTAIL_DEBUGDRAW_H

#include <vector>

#include <glm/glm.hpp>

#include "JoltDebugVertex.h"

namespace ytail {
    class DebugDraw {
    public:
        void clear() { lines.clear(); }
        [[nodiscard]] bool empty() const { return lines.empty(); }
        // LINELIST vertices (consecutive pairs) ready for DebugLineRenderer::upload.
        [[nodiscard]] const std::vector<JoltDebugVertex>& vertices() const { return lines; }

        // A single segment.
        void line(const glm::vec3& a, const glm::vec3& b, const glm::vec4& color);

        // A shaft from -> to capped with a small four-line arrowhead at the tip.
        // headSize <= 0 sizes the head as a fraction of the shaft length.
        void arrow(const glm::vec3& from, const glm::vec3& to, const glm::vec4& color, float headSize = 0.0f);

        // A circle of `segments` edges in the plane whose normal is `axis`.
        void circle(const glm::vec3& center, const glm::vec3& axis, float radius,
                    const glm::vec4& color, int segments = 32);

        // Three orthogonal circles (XY / XZ / YZ) reading as a wireframe sphere.
        void wireSphere(const glm::vec3& center, float radius, const glm::vec4& color, int segments = 24);

        // The 12 edges of an axis-aligned box.
        void box(const glm::vec3& center, const glm::vec3& halfExtents, const glm::vec4& color);

    private:
        std::vector<JoltDebugVertex> lines;
    };
} // ytail

#endif //YELLOWTAIL_DEBUGDRAW_H
