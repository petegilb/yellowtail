struct Input
{
    float3 position : TEXCOORD0;   // location 0  (aPos)
    float3 normal   : TEXCOORD1;   // location 1  (aNormal)
    float2 uv       : TEXCOORD2;   // location 2  (aTexCoords)
};

struct Output
{
    float3 fragPos  : TEXCOORD0;
    float3 normal   : TEXCOORD1;
    float2 uv       : TEXCOORD2;
    float4 position : SV_Position; // == gl_Position
};

// Vertex-stage uniform buffers live in space1 for SDL_shadercross.
// Must match the VertexUniform struct in RenderComponent.h member-for-member.
cbuffer Camera : register(b0, space1)
{
    float4x4 mvp;           // projection * view * model  -> clip position
    float4x4 model;         // model alone                -> world-space fragPos
    float4x4 normalMatrix;  // transpose(inverse(model))  -> world-space normal
};

Output main(Input input)
{
    Output output;

    float4 localPos = float4(input.position, 1.0f);

    output.position = mul(mvp, localPos);                        // clip space
    output.fragPos  = mul(model, localPos).xyz;                  // world space (for lighting)
    output.normal   = mul((float3x3)normalMatrix, input.normal);
    output.uv       = input.uv;

    return output;
}
