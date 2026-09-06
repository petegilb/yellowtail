// Ocean shading. All the detail lives here rather than in the geometry: the clipmap is coarse and
// the normal, foam and specular are sampled at full resolution, which is what makes the whole
// thing cheap without looking it.

struct Input
{
    float4 position : SV_Position;
    float3 worldPosition : TEXCOORD0;
    float2 patchUv : TEXCOORD1;
};

// Fragment-stage textures and samplers live in space2. SDL_GPU binds these as texture/sampler
// pairs, so every texture needs its own sampler slot even when they end up being the same sampler.
Texture2DArray<float4> derivativesMap : register(t0, space2);
SamplerState derivativesSampler : register(s0, space2);
Texture2DArray<float> foamMap : register(t1, space2);
SamplerState foamSampler : register(s1, space2);
Texture2D skyTex : register(t2, space2);
SamplerState skySampler : register(s2, space2);

// Fragment-stage uniform buffers live in space3. Mirrors OceanFragmentUniform in OceanRenderer.h.
cbuffer OceanFragment : register(b0, space3)
{
    float3 cameraPosition; float skyYaw;
    float3 sunDirection;   float sunIntensity; // direction the light travels, matching LightComponent
    float3 sunColor;       float roughness;
    float3 shallowColor;   float foamStrength;
    float3 deepColor;      float subsurfaceStrength;
    float3 skyTint;        float fresnelPower;
    float4 patchSizes;
    float4 cascadeFadeDistance;
    uint cascadeCount; float seaLevel; float2 _pad0;
};

// Foam is spray, not water: a bright rough surface with no reflection of its own.
static const float3 kFoamAlbedo = float3(0.88f, 0.92f, 0.95f);

static const float kPi = 3.14159265358979f;

float3 sampleSky(float3 direction)
{
    const float longitude = atan2(direction.z, direction.x) + skyYaw;
    const float2 uv = float2(longitude / (2.0f * kPi) + 0.5f,
                             acos(clamp(direction.y, -1.0f, 1.0f)) / kPi);
    // SampleLevel, not Sample: the panorama's seam wraps within one texel and the screen-space
    // derivative Sample would use to pick a mip blows up across it.
    return skyTex.SampleLevel(skySampler, uv, 0.0f).rgb * skyTint;
}

float4 main(Input input) : SV_Target
{
    const float cameraDistance = length(input.worldPosition - cameraPosition);

    // Slopes add across cascades because each is metres per metre, so one normal comes out of the
    // sum. Summing normals instead would need an unnormalize on every sample.
    float2 slope = float2(0.0f, 0.0f);
    float foam = 0.0f;

    for (uint cascade = 0u; cascade < cascadeCount; ++cascade)
    {
        // Purely a cost saving: a cascade whose waves are a fraction of a pixel across has already
        // been averaged to nothing by its own mips, so sampling it buys no detail. Ramped rather
        // than cut so a cascade fades out instead of vanishing along a line.
        //
        // The geometry does not use this. It picks a mip from its own cell size instead, because
        // what a vertex can represent depends on the mesh rather than on the viewer.
        const float fade = 1.0f - smoothstep(cascadeFadeDistance[cascade] * 0.7f,
                                             cascadeFadeDistance[cascade], cameraDistance);
        if (fade <= 0.0f) continue;

        const float3 uv = float3(input.patchUv / patchSizes[cascade], float(cascade));
        slope += derivativesMap.Sample(derivativesSampler, uv).xy * fade;
        foam += foamMap.Sample(foamSampler, uv) * fade;
    }

    const float3 normal = normalize(float3(-slope.x, 1.0f, -slope.y));
    const float3 viewDirection = normalize(cameraPosition - input.worldPosition);

    // Schlick, with water's 0.02 normal-incidence reflectance. At grazing angles this goes to 1,
    // which is why a distant sea reads as sky and the water under your feet reads as water.
    const float viewDotNormal = saturate(dot(normal, viewDirection));
    const float fresnel = 0.02f + 0.98f * pow(1.0f - viewDotNormal, fresnelPower);

    const float3 reflectionDirection = reflect(-viewDirection, normal);
    const float3 reflection = sampleSky(reflectionDirection);

    // Subsurface: light scattering up through a wave. Keyed off height above the mean surface
    // rather than off slope, because what glows is the thin water at a crest, and a steep wave
    // face in a trough has plenty of ocean behind it. Strongest looking into the sun, which is
    // when the light is coming through the wave toward the eye rather than off it.
    const float3 lightDirection = -normalize(sunDirection);
    const float crest = saturate((input.worldPosition.y - seaLevel) * subsurfaceStrength);
    const float backlight = pow(saturate(dot(viewDirection, -lightDirection)), 4.0f);
    const float scatter = crest * (0.3f + 0.7f * backlight)
                        * saturate(dot(normal, lightDirection) * 0.5f + 0.5f);
    const float3 water = lerp(deepColor, shallowColor, saturate(scatter));

    // Blinn-Phong specular for the sun glint, matching the lit pipeline's model.
    const float3 halfVector = normalize(lightDirection + viewDirection);
    const float shininess = 2.0f / max(roughness * roughness * roughness * roughness, 1e-4f) - 2.0f;
    const float specular = pow(saturate(dot(normal, halfVector)), shininess);

    float3 color = lerp(water, reflection, fresnel);
    color += sunColor * sunIntensity * specular * fresnel;

    // Foam sits on top of everything: it is opaque spray, not a tint on the water below it. Lit
    // diffusely rather than painted the sun's own colour, which blew out to white the moment the
    // sun's intensity went above one.
    const float foamAmount = saturate(foam * foamStrength);
    const float foamLight = saturate(dot(normal, lightDirection)) * 0.6f + 0.4f;
    const float3 foamColor = kFoamAlbedo * (sunColor * sunIntensity * foamLight);
    color = lerp(color, foamColor, foamAmount);

    return float4(color, 1.0f);
}
