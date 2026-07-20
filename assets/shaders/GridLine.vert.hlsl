struct Input
{
    float3 position : TEXCOORD0;   // location 0 (world-space)
    float4 color    : TEXCOORD1;   // location 1
};

struct Output
{
    float4 color    : TEXCOORD0;
    float2 worldXZ  : TEXCOORD1;   // world XZ, for the per-fragment radial fade
    float4 position : SV_Position;
};

// Vertex-stage uniform buffers live in space1 for SDL_shadercross.
cbuffer Camera : register(b0, space1)
{
    float4x4 viewProj;
};

Output main(Input input)
{
    Output output;
    output.position = mul(viewProj, float4(input.position, 1.0f));
    output.color    = input.color;
    output.worldXZ  = input.position.xz;
    return output;
}
