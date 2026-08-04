//
// Created by Peter Gilbert on 8/4/26.
//
// Procedural cube/plane/sphere/cylinder/capsule geometry so games get basic shapes with no
// asset files. CPU-side data only; ResourceManager uploads it via getMesh("primitive:<shape>").
//

#ifndef YELLOWTAIL_MESHPRIMITIVES_H
#define YELLOWTAIL_MESHPRIMITIVES_H

#include <vector>

#include <SDL3/SDL.h>

#include "Mesh.h"

namespace ytail::primitives {
    struct MeshData {
        std::vector<Vertex> vertices;
        std::vector<Uint32> indices;
    };

    // Centered at the origin. Cube edge and sphere/plane extents fit a 1-unit box at default
    // size, so scaling the entity by N gives an N-unit shape.
    MeshData makeCube(float size = 1.0f);
    MeshData makePlane(float size = 1.0f);
    MeshData makeSphere(float radius = 0.5f, Uint32 rings = 16, Uint32 sectors = 32);
    // Axis along Y. Cylinder is `height` tall; capsule adds a radius hemisphere on each end
    // (total height = height + 2 * radius).
    MeshData makeCylinder(float radius = 0.5f, float height = 1.0f, Uint32 sectors = 32);
    MeshData makeCapsule(float radius = 0.5f, float height = 1.0f, Uint32 rings = 8, Uint32 sectors = 32);
} // ytail::primitives

#endif //YELLOWTAIL_MESHPRIMITIVES_H
