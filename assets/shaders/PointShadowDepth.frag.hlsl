// Point-light (omnidirectional) shadow pass, fragment stage. Writes LINEAR distance-to-light
// (normalized by the far plane) into depth. Storing linear distance rather than the
// projection's non-linear z lets the lit shader sample the cube by the world-space
// light->fragment direction and compare distances directly, with no per-face reconstruction.
// Writing SV_Depth disables early-Z, which is fine for a depth-only pass.
struct Input
{
    float4 clipPos  : SV_Position;
    float3 worldPos : TEXCOORD0;
};

// Per-light, pushed to fragment slot 0. farPlane == the light's attenuation radius.
cbuffer PointLightDepth : register(b0, space3)
{
    float3 lightPos;
    float  farPlane;
};

float main(Input input) : SV_Depth
{
    float dist = length(input.worldPos - lightPos);
    return saturate(dist / farPlane);
}
