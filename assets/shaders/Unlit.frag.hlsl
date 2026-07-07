struct Input
{
    float3 fragPos : TEXCOORD0;
    float3 normal  : TEXCOORD1;
    float2 uv      : TEXCOORD2;
};

// Unlit debug shader: no textures, no lighting, no uniform buffers.
// Outputs the world-space normal as color (remapped from [-1,1] to [0,1]),
// which makes each face of a cube a distinct flat color.
float4 main(Input input) : SV_Target
{
    float3 n = normalize(input.normal);
    return float4(n * 0.5f + 0.5f, 1.0f);
}
