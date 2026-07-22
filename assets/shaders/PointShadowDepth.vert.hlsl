// Point-light (omnidirectional) shadow pass, vertex stage. Same Mesh layout as BlinnPhongLit
// so it draws the same buffers; normal/uv are ignored. Outputs clip position (for
// rasterization) plus world position, which the fragment stage turns into linear
// distance-to-light.
struct Input
{
    float3 position : TEXCOORD0;   // location 0
    float3 normal   : TEXCOORD1;   // location 1 (unused)
    float2 uv       : TEXCOORD2;   // location 2 (unused)
};

struct Output
{
    float4 clipPos  : SV_Position;
    float3 worldPos : TEXCOORD0;
};

// Per-object, pushed to vertex slot 0. faceViewProjModel = (face view*proj) * model takes the
// vertex to this cube face's clip space; model takes it to world space for the distance calc.
cbuffer PointLightMVP : register(b0, space1)
{
    float4x4 faceViewProjModel;
    float4x4 model;
};

Output main(Input input)
{
    Output output;
    float4 world    = mul(model, float4(input.position, 1.0f));
    output.worldPos = world.xyz;
    output.clipPos  = mul(faceViewProjModel, float4(input.position, 1.0f));
    return output;
}
