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
// Shadow map (sun depth) + its comparison sampler for hardware Percentage-Closer Filtering
Texture2D              shadowTex : register(t2, space2);
SamplerComparisonState shadowSmp : register(s2, space2);

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

// Must match ShadowUniform in RenderComponent.h.
cbuffer Shadow : register(b2, space3)
{
    float4x4 lightViewProj; // world -> sun clip space
    float shadowBias;       // depth-compare bias (fights acne)
    int   shadowEnabled;    // 0 = no caster this frame; skip the sample
    float texelSize;        // 1 / shadowMapSize, PCF tap spacing
    float _padShadow;
};

// 0 = shadowed, 1 = lit. 3x3 PCF.
float sampleShadow(float3 worldPos, float NdotL)
{
    float4 lightClip = mul(lightViewProj, float4(worldPos, 1.0f));
    // Ortho w == 1, but divide anyway in case the projection changes.
    float3 proj = lightClip.xyz / lightClip.w;

    // Clip xy [-1,1] -> uv [0,1], y flipped for the top-left texture origin.
    float2 uv = proj.xy * float2(0.5f, -0.5f) + 0.5f;
    float fragDepth = proj.z;

    // Outside the light frustum: nothing to occlude, treat as lit.
    if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f || fragDepth > 1.0f)
        return 1.0f;

    // Slope-scaled bias: grazing angles need more to avoid acne.
    float bias = max(shadowBias * (1.0f - NdotL), shadowBias * 0.1f);
    float compareDepth = fragDepth - bias;

    float sum = 0.0f;
    // unroll is a compiler hint expand the loops into 9 straight-line SampleCmp calls
    // shadowSmp is a comparison sampler with a linear filter, each call returns a value in [0, 1]
    // level 0 is the biggest mipmap (there's no mipmaps tho)
    [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x)
            sum += shadowTex.SampleCmpLevelZero(shadowSmp, uv + float2(x, y) * texelSize, compareDepth);
    return sum / 9.0f;
}

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

        // Only the sun (a directional light) casts the shadow map this frame.
        float shadow = 1.0f;
        if (light.type == LIGHT_DIRECTIONAL && shadowEnabled != 0)
            shadow = sampleShadow(input.fragPos, diff);

        result += (diffuse + specular) * attenuation * shadow;
    }

    return float4(result, 1.0f);
}