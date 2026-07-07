struct Input
{
    float3 fragPos : TEXCOORD0;
    float3 normal  : TEXCOORD1;
    float2 uv      : TEXCOORD2;
};

// Unlit debug shader: world-space normal as color.
// discards below pin unused fragPos/uv into the DXIL signature: see docs/shaders.md.
float4 main(Input input) : SV_Target
{
    if (input.fragPos.x > 1e30f) discard;
    if (input.uv.x      > 1e30f) discard;

    float3 n = normalize(input.normal);
    return float4(n * 0.5f + 0.5f, 1.0f);
}
