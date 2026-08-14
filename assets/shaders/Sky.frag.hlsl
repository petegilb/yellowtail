struct Input
{
    float3 direction : TEXCOORD0;
};

// Fragment-stage textures + samplers live in space2.
// An equirectangular (2:1) panorama: u wraps once around the horizon, v runs from straight up
// at the top row to straight down at the bottom.
Texture2D    skyTex : register(t0, space2);
SamplerState skySmp : register(s0, space2);

// Fragment-stage uniform buffers live in space3.
// Must match SkyUniform in RenderComponent.h (vec3 + trailing scalar fills one 16-byte row).
cbuffer Sky : register(b0, space3)
{
    float3 tint; float yaw;
};

static const float kPi = 3.14159265f;

float4 main(Input input) : SV_Target
{
    const float3 dir = normalize(input.direction);

    // Longitude around Y, latitude from the Y component. Derived from the direction rather than
    // the mesh's UVs so any closed mesh works as the dome and no seam depends on how it unwraps.
    const float longitude = atan2(dir.z, dir.x) + yaw;
    const float2 uv = float2(longitude / (2.0f * kPi) + 0.5f,
                             acos(clamp(dir.y, -1.0f, 1.0f)) / kPi);

    // SampleLevel rather than Sample: longitude wraps from 1 back to 0 within a single texel, and
    // the screen-space derivative Sample uses to pick a mip blows up along that seam.
    return float4(skyTex.SampleLevel(skySmp, uv, 0.0f).rgb * tint, 1.0f);
}
