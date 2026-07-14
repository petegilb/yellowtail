//
// Created by PeterPC on 7/14/2026.
//

#ifndef YELLOWTAIL_JOLTDEBUGRENDERER_H
#define YELLOWTAIL_JOLTDEBUGRENDERER_H

// Jolt.h must precede any other Jolt header.
#include <Jolt/Jolt.h>
#include <Jolt/Renderer/DebugRendererSimple.h>

#include <vector>

#include "JoltDebugVertex.h"

// DebugRenderer using the simple example renderer inside Jolt (DebugRendererSimple.h).
// Accumulates line/triangle geometry from physicsSystem.DrawBodies into CPU buffers the GPU
// renderer uploads and draws. Call clear() before each DrawBodies pass.
class JoltDebugRenderer : public JPH::DebugRendererSimple {
public:
	virtual void DrawLine(JPH::RVec3Arg inFrom, JPH::RVec3Arg inTo, JPH::ColorArg inColor) override
	{
		const glm::vec4 color = toGlm(inColor);
		lines.push_back({ toGlm(inFrom), color });
		lines.push_back({ toGlm(inTo), color });
	}

	virtual void DrawTriangle(JPH::RVec3Arg inV1, JPH::RVec3Arg inV2, JPH::RVec3Arg inV3, JPH::ColorArg inColor, ECastShadow inCastShadow) override
	{
		const glm::vec4 color = toGlm(inColor);
		triangles.push_back({ toGlm(inV1), color });
		triangles.push_back({ toGlm(inV2), color });
		triangles.push_back({ toGlm(inV3), color });
	}

	virtual void DrawText3D(JPH::RVec3Arg inPosition, const JPH::string_view &inString, JPH::ColorArg inColor, float inHeight) override
	{
		// Text overlay not supported yet; ignore.
	}

	// Drop last frame's geometry. Call right before physicsSystem.DrawBodies().
	void clear()
	{
		lines.clear();
		triangles.clear();
	}

	// LINELIST vertices (pairs) and TRIANGLELIST vertices (triples) for the GPU path to draw.
	[[nodiscard]] const std::vector<ytail::JoltDebugVertex>& getLines() const { return lines; }
	[[nodiscard]] const std::vector<ytail::JoltDebugVertex>& getTriangles() const { return triangles; }

private:
	static glm::vec3 toGlm(JPH::RVec3Arg v)
	{
		return { v.GetX(), v.GetY(), v.GetZ() };
	}
	static glm::vec4 toGlm(JPH::ColorArg c)
	{
		return { c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, c.a / 255.0f };
	}

	std::vector<ytail::JoltDebugVertex> lines;
	std::vector<ytail::JoltDebugVertex> triangles;
};

#endif //YELLOWTAIL_JOLTDEBUGRENDERER_H
