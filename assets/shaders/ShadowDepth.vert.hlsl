// Shadow pass vertex stage. Same Mesh layout as BlinnPhongLit (so it draws the same buffers), but
// only the clip position matters — normal/uv are ignored.
struct Input
{
    float3 position : TEXCOORD0;   // location 0
    float3 normal   : TEXCOORD1;   // location 1 (unused)
    float2 uv       : TEXCOORD2;   // location 2 (unused)
};

struct Output
{
    float4 position : SV_Position;
};

// lightViewProj * model, pushed per object to vertex slot 0.
cbuffer LightMVP : register(b0, space1)
{
    float4x4 lightMvp;
};

Output main(Input input)
{
    Output output;
    output.position = mul(lightMvp, float4(input.position, 1.0f));
    return output;
}
