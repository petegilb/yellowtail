struct Input
{
    float4 color   : TEXCOORD0;
    float2 worldXZ : TEXCOORD1;
};

// Fragment-stage uniform buffers live in space3 for SDL_shadercross.
cbuffer GridFade : register(b0, space3)
{
    float2 center;      // camera-snapped grid center on the XZ plane
    float  halfRadius;  // extent * spacing
    float  opacity;
};

float4 main(Input input) : SV_Target0
{
    float distanceToCenter = distance(input.worldXZ, center);
    // Full opacity out to half the radius, then ease to zero at the rim.
    float t = saturate((distanceToCenter - 0.5f * halfRadius) / (0.5f * halfRadius));
    float fade = opacity * (1.0f - t * t);
    return float4(input.color.rgb, input.color.a * fade);
}
