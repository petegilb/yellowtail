//
// Created by Peter Gilbert on 8/4/26.
//

#include "MeshPrimitives.h"

#include <cmath>

#include <glm/glm.hpp>

namespace ytail::primitives {
    namespace {
        constexpr float kPi = 3.14159265358979323846f;

        // One quad face from four corners in counter-clockwise order (viewed from outside, so it
        // survives back-face culling). Corners map to uv (0,0),(1,0),(1,1),(0,1).
        void addQuad(MeshData& data, const glm::vec3& normal, const glm::vec3& bottomLeft,
                     const glm::vec3& bottomRight, const glm::vec3& topRight, const glm::vec3& topLeft) {
            const Uint32 base = static_cast<Uint32>(data.vertices.size());
            data.vertices.push_back({ bottomLeft, normal, { 0.0f, 0.0f } });
            data.vertices.push_back({ bottomRight, normal, { 1.0f, 0.0f } });
            data.vertices.push_back({ topRight, normal, { 1.0f, 1.0f } });
            data.vertices.push_back({ topLeft, normal, { 0.0f, 1.0f } });
            data.indices.insert(data.indices.end(),
                                { base, base + 1, base + 2, base, base + 2, base + 3 });
        }
    }

    MeshData makeCube(const float size) {
        const float h = size * 0.5f;
        MeshData data;
        addQuad(data, { 0, 0, 1 },  { -h, -h, h }, { h, -h, h }, { h, h, h }, { -h, h, h });
        addQuad(data, { 0, 0, -1 }, { h, -h, -h }, { -h, -h, -h }, { -h, h, -h }, { h, h, -h });
        addQuad(data, { 1, 0, 0 },  { h, -h, h }, { h, -h, -h }, { h, h, -h }, { h, h, h });
        addQuad(data, { -1, 0, 0 }, { -h, -h, -h }, { -h, -h, h }, { -h, h, h }, { -h, h, -h });
        addQuad(data, { 0, 1, 0 },  { -h, h, h }, { h, h, h }, { h, h, -h }, { -h, h, -h });
        addQuad(data, { 0, -1, 0 }, { -h, -h, -h }, { h, -h, -h }, { h, -h, h }, { -h, -h, h });
        return data;
    }

    MeshData makePlane(const float size) {
        const float h = size * 0.5f;
        MeshData data;
        addQuad(data, { 0, 1, 0 }, { -h, 0, h }, { h, 0, h }, { h, 0, -h }, { -h, 0, -h });
        return data;
    }

    MeshData makeSphere(const float radius, const Uint32 rings, const Uint32 sectors) {
        MeshData data;
        for (Uint32 ring = 0; ring <= rings; ++ring) {
            const float phi = kPi * static_cast<float>(ring) / static_cast<float>(rings);
            const float y = std::cos(phi);
            const float ringRadius = std::sin(phi);
            for (Uint32 sector = 0; sector <= sectors; ++sector) {
                const float theta = 2.0f * kPi * static_cast<float>(sector) / static_cast<float>(sectors);
                const glm::vec3 normal{ ringRadius * std::cos(theta), y, ringRadius * std::sin(theta) };
                data.vertices.push_back({ normal * radius, normal,
                    { static_cast<float>(sector) / static_cast<float>(sectors),
                      static_cast<float>(ring) / static_cast<float>(rings) } });
            }
        }

        const Uint32 stride = sectors + 1;
        for (Uint32 ring = 0; ring < rings; ++ring) {
            for (Uint32 sector = 0; sector < sectors; ++sector) {
                const Uint32 current = ring * stride + sector;
                const Uint32 below = current + stride;
                data.indices.insert(data.indices.end(),
                    { current, current + 1, below, current + 1, below + 1, below });
            }
        }
        return data;
    }

    MeshData makeCylinder(const float radius, const float height, const Uint32 sectors) {
        MeshData data;
        const float halfHeight = height * 0.5f;

        // Side wall: radial normals, with a duplicated seam column so uv wraps cleanly. Each
        // sector column stores its bottom vertex then its top vertex.
        const Uint32 sideBase = static_cast<Uint32>(data.vertices.size());
        for (Uint32 sector = 0; sector <= sectors; ++sector) {
            const float theta = 2.0f * kPi * static_cast<float>(sector) / static_cast<float>(sectors);
            const float cx = std::cos(theta);
            const float cz = std::sin(theta);
            const glm::vec3 normal{ cx, 0.0f, cz };
            const float u = static_cast<float>(sector) / static_cast<float>(sectors);
            data.vertices.push_back({ { radius * cx, -halfHeight, radius * cz }, normal, { u, 0.0f } });
            data.vertices.push_back({ { radius * cx, halfHeight, radius * cz }, normal, { u, 1.0f } });
        }
        for (Uint32 sector = 0; sector < sectors; ++sector) {
            const Uint32 bottom = sideBase + sector * 2;
            const Uint32 top = bottom + 1;
            const Uint32 bottomNext = bottom + 2;
            const Uint32 topNext = bottom + 3;
            data.indices.insert(data.indices.end(), { bottom, top, topNext, bottom, topNext, bottomNext });
        }

        // End caps: a center vertex fanned out to a ring. Top faces +Y, bottom -Y.
        const auto addCap = [&](const float y, const glm::vec3& normal, const bool facingUp) {
            const Uint32 center = static_cast<Uint32>(data.vertices.size());
            data.vertices.push_back({ { 0.0f, y, 0.0f }, normal, { 0.5f, 0.5f } });
            const Uint32 ringBase = static_cast<Uint32>(data.vertices.size());
            for (Uint32 sector = 0; sector < sectors; ++sector) {
                const float theta = 2.0f * kPi * static_cast<float>(sector) / static_cast<float>(sectors);
                const float cx = std::cos(theta);
                const float cz = std::sin(theta);
                data.vertices.push_back({ { radius * cx, y, radius * cz }, normal,
                    { cx * 0.5f + 0.5f, cz * 0.5f + 0.5f } });
            }
            for (Uint32 sector = 0; sector < sectors; ++sector) {
                const Uint32 current = ringBase + sector;
                const Uint32 next = ringBase + (sector + 1) % sectors;
                if (facingUp) data.indices.insert(data.indices.end(), { center, next, current });
                else data.indices.insert(data.indices.end(), { center, current, next });
            }
        };
        addCap(halfHeight, { 0.0f, 1.0f, 0.0f }, true);
        addCap(-halfHeight, { 0.0f, -1.0f, 0.0f }, false);
        return data;
    }

    MeshData makeCapsule(const float radius, const float height, const Uint32 rings, const Uint32 sectors) {
        MeshData data;
        const float halfHeight = height * 0.5f;
        const Uint32 stride = sectors + 1;
        const Uint32 totalRows = 2 * (rings + 1);

        // Latitude rows from the top pole down to the bottom pole. The top hemisphere's equator
        // (at +halfHeight) and the bottom's (at -halfHeight) both sit at the full radius, so the
        // rows between them form the straight cylinder wall with matching radial normals.
        Uint32 rowIndex = 0;
        const auto addRow = [&](const float centerY, const float normalY, const float ringScale) {
            const float v = static_cast<float>(rowIndex) / static_cast<float>(totalRows - 1);
            for (Uint32 sector = 0; sector <= sectors; ++sector) {
                const float theta = 2.0f * kPi * static_cast<float>(sector) / static_cast<float>(sectors);
                const glm::vec3 normal{ ringScale * std::cos(theta), normalY, ringScale * std::sin(theta) };
                const glm::vec3 position{ radius * normal.x, centerY + radius * normalY, radius * normal.z };
                data.vertices.push_back({ position, normal, { static_cast<float>(sector) / static_cast<float>(sectors), v } });
            }
            ++rowIndex;
        };

        for (Uint32 ring = 0; ring <= rings; ++ring) {
            const float phi = 0.5f * kPi * static_cast<float>(ring) / static_cast<float>(rings);
            addRow(halfHeight, std::cos(phi), std::sin(phi));
        }
        for (Uint32 ring = 0; ring <= rings; ++ring) {
            const float phi = 0.5f * kPi * static_cast<float>(ring) / static_cast<float>(rings);
            addRow(-halfHeight, -std::sin(phi), std::cos(phi));
        }

        for (Uint32 row = 0; row + 1 < totalRows; ++row) {
            for (Uint32 sector = 0; sector < sectors; ++sector) {
                const Uint32 current = row * stride + sector;
                const Uint32 below = current + stride;
                data.indices.insert(data.indices.end(),
                    { current, current + 1, below, current + 1, below + 1, below });
            }
        }
        return data;
    }
} // ytail::primitives
