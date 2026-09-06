// Clipmap vertex stage: geomorph, then displace.
//
// SDL_GPU has no tessellation (vertex and fragment are the only graphics stages), so continuous LOD
// is done here instead. Every vertex carries the position its coarser neighbour would occupy, and
// the morph weight ramps to 1 by the outer edge of each ring, so a ring boundary is watertight and
// crossing one is a lerp rather than a pop.
//
// The weight is baked per vertex rather than derived from camera distance here. The grid is
// camera-centred, so a vertex sits at a fixed offset from the camera and its distance to the ring
// boundary never changes.

struct Input
{
    // The ocean grid has no authored normals or texture coordinates, so the standard mesh layout's
    // channels are repurposed rather than adding a vertex format for one mesh.
    float3 gridPosition : TEXCOORD0;   // .xz = position at this ring's density, .y unused
    float3 parentPosition : TEXCOORD1; // .xz = the coarser ring's position to morph toward
    float2 ringData : TEXCOORD2;       // .x = morph weight 0..1, .y = ring level
};

struct Output
{
    float4 position : SV_Position;
    float3 worldPosition : TEXCOORD0;
    float2 patchUv : TEXCOORD1; // undisplaced world xz, what the fragment stage re-samples with
};

// Vertex-stage textures and samplers live in space0.
Texture2DArray<float4> displacementMap : register(t0, space0);
SamplerState displacementSampler : register(s0, space0);

// Vertex-stage uniform buffers live in space1. Mirrors OceanVertexUniform in OceanRenderer.h.
cbuffer OceanVertex : register(b0, space1)
{
    float4x4 viewProjection;
    float2 gridOrigin;                 // world xz the grid is snapped to, added to every vertex
    float baseCellSize;                // quad size of the finest ring, metres
    float texelsPerPatch;              // displacement map resolution, for the mip maths
    float4 patchSizes;                 // metres one tile of each cascade covers, .w unused
    uint cascadeCount; uint mipCount; float seaLevel; float _pad0;
};

Output main(Input input)
{
    Output output;

    // Morph in grid space, never after displacing. Blending two displaced points would average two
    // different places on the wave and visibly flatten every crest along a ring boundary.
    const float2 gridXz = lerp(input.gridPosition.xz, input.parentPosition.xz, input.ringData.x);
    const float2 worldXz = gridXz + gridOrigin;

    // How much of the wave field this vertex can carry. A ring's quads double in size each level
    // out, and a quad tens of metres across cannot show a two metre wave, it can only alias one.
    // Left to alias, that detail is re-scrambled every time the grid snaps forward to follow the
    // camera, which is exactly what a crawling, jittering horizon is.
    //
    // Cell size rather than camera distance, because the vertex already knows its own ring and
    // the mesh, not the viewer, is what sets the resolving power. Morphed toward the parent
    // ring's cell size so a vertex sitting on a ring boundary picks the same mip from either
    // side and the seam stays watertight.
    const float cellSize = baseCellSize * exp2(input.ringData.y)
                         * lerp(1.0f, 2.0f, input.ringData.x);

    float3 displacement = float3(0.0f, 0.0f, 0.0f);

    for (uint cascade = 0u; cascade < cascadeCount; ++cascade)
    {
        const float texelSize = patchSizes[cascade] / texelsPerPatch;
        // One mip per doubling: the level whose texels are about this vertex's spacing.
        const float mip = clamp(log2(max(cellSize / texelSize, 1.0f)),
                                0.0f, float(mipCount) - 1.0f);

        const float2 uv = worldXz / patchSizes[cascade];
        displacement += displacementMap.SampleLevel(displacementSampler,
                                                    float3(uv, float(cascade)), mip).xyz;
    }

    // Sea level goes into the position rather than into viewProjection, so the world position the
    // fragment stage shades with is the one actually drawn.
    const float3 worldPosition = float3(worldXz.x + displacement.x,
                                        displacement.y + seaLevel,
                                        worldXz.y + displacement.z);

    output.position = mul(viewProjection, float4(worldPosition, 1.0f));
    output.worldPosition = worldPosition;
    // The undisplaced position, so the fragment stage samples the same texel this vertex did
    // rather than chasing the displaced one back through an inversion.
    output.patchUv = worldXz;
    return output;
}
