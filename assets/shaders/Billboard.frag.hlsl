struct Input
{
    float3 fragPos : TEXCOORD0;
    float3 normal  : TEXCOORD1;
    float2 uv      : TEXCOORD2;
};

// Icon texture + sampler (fragment textures live in space2).
Texture2D    iconTex : register(t0, space2);
SamplerState iconSmp : register(s0, space2);

// Camera-facing sprite: sample the icon and let the pipeline's alpha blend do the rest.
// The discards below pin the unused vert outputs into the DXIL signature (see docs/shaders.md).
float4 main(Input input) : SV_Target
{
    if (input.fragPos.x > 1e30f) discard;
    if (input.normal.x  > 1e30f) discard;

    float4 color = iconTex.Sample(iconSmp, input.uv);
    if (color.a < 0.01f) discard;   // skip fully transparent texels
    return color;
}
