struct Input
{
    float3 fragPos : TEXCOORD0;
    float3 normal  : TEXCOORD1;
    float2 uv      : TEXCOORD2;
};

// Fragment-stage textures + samplers live in space2 (separate objects in HLSL).
Texture2D    diffuseTex  : register(t0, space2);
SamplerState diffuseSmp  : register(s0, space2);
Texture2D    specularTex : register(t1, space2);
SamplerState specularSmp : register(s1, space2);

// Fragment-stage uniform buffers live in space3.
// Per-frame / per-scene data: camera + scene ambient + the (single) light.
// Must match FrameLightingUniform in C++ member-for-member.
cbuffer FrameLighting : register(b0, space3)
{
    float3 viewPos;     float _pad0;   // camera world position
    float3 ambient;     float _pad1;   // scene ambient (added once, not per-light)
    float3 lightPos;    float _pad2;
    float3 lightColor;  float _pad3;   // light emission (color * intensity)
};

// Per-material data. Must match MaterialUniform in C++ (16-byte packing: the two float2s
// fill the first row, shininess starts the second).
cbuffer Material : register(b1, space3)
{
    float2 uvScale;
    float2 uvOffset;
    float  shininess;
};

float4 main(Input input) : SV_Target
{
    // Per-material UV transform (tiling + shift) applied once, shared by both maps.
    float2 uv = input.uv * uvScale + uvOffset;

    float3 diffuseColor = diffuseTex.Sample(diffuseSmp, uv).rgb;

    // ambient (scene value, not per-light)
    float3 ambientTerm = ambient * diffuseColor;

    // diffuse
    float3 norm     = normalize(input.normal);
    float3 lightDir = normalize(lightPos - input.fragPos);
    float  diff     = max(dot(norm, lightDir), 0.0f);
    float3 diffuse  = lightColor * diff * diffuseColor;

    // specular (Blinn-Phong halfway vector)
    float3 viewDir  = normalize(viewPos - input.fragPos);
    float3 halfway  = normalize(lightDir + viewDir);
    float  spec     = pow(max(dot(norm, halfway), 0.0f), shininess);
    float3 specular = lightColor * spec * specularTex.Sample(specularSmp, uv).rgb;

    float3 result = ambientTerm + diffuse + specular;
    return float4(result, 1.0f);
}