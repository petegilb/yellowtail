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

#define MAX_LIGHTS 16
#define LIGHT_POINT       0
#define LIGHT_DIRECTIONAL 1

// One scene light. Must match GpuLight in C++ member-for-member (vec3 + trailing scalar per row).
struct Light
{
    float3 position;  float attenuation; // world position (point); attenuation radius
    float3 direction; int type;          // travel direction (directional); LIGHT_* above
    float3 color;     float _pad;        // emission (color * intensity)
};

// Fragment-stage uniform buffers live in space3.
// Per-frame / per-scene data: camera + scene ambient + the active light set.
// Must match FrameLightingUniform in C++ member-for-member.
cbuffer FrameLighting : register(b0, space3)
{
    float3 viewPos; float _pad0;    // camera world position
    float3 ambient; int lightCount; // scene ambient (added once) + number of active lights
    Light  lights[MAX_LIGHTS];
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

    float3 diffuseColor  = diffuseTex.Sample(diffuseSmp, uv).rgb;
    float3 specularColor = specularTex.Sample(specularSmp, uv).rgb;

    float3 norm    = normalize(input.normal);
    float3 viewDir = normalize(viewPos - input.fragPos);

    // ambient (scene value, added once — not per-light)
    float3 result = ambient * diffuseColor;

    for (int i = 0; i < lightCount; ++i)
    {
        Light light = lights[i];

        float3 lightDir;
        float  attenuation = 1.0f;
        if (light.type == LIGHT_DIRECTIONAL)
        {
            // Parallel rays with no falloff; shade against the reverse of the travel direction.
            lightDir = normalize(-light.direction);
        }
        else
        {
            float3 toLight = light.position - input.fragPos;
            float  dist    = length(toLight);
            lightDir       = toLight / max(dist, 1e-4f);
            // Smooth range-based falloff: full at the source, 0 at the attenuation radius.
            float falloff  = saturate(1.0f - (dist * dist) / (light.attenuation * light.attenuation));
            attenuation    = falloff * falloff;
        }

        // diffuse
        float  diff    = max(dot(norm, lightDir), 0.0f);
        float3 diffuse = light.color * diff * diffuseColor;

        // specular (Blinn-Phong halfway vector)
        float3 halfway  = normalize(lightDir + viewDir);
        float  spec     = pow(max(dot(norm, halfway), 0.0f), shininess);
        float3 specular = light.color * spec * specularColor;

        result += (diffuse + specular) * attenuation;
    }

    return float4(result, 1.0f);
}