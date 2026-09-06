#include "OceanCommon.hlsli"

// Turns the raw IFFT output into everything the draw and the physics read: world displacement,
// height slopes, the folding measure that drives foam, and the foam itself.
//
// Finite differences rather than two more inverse transforms. Four extra taps of a texture already
// in cache costs a fraction of a third and fourth IFFT, and at these patch sizes the result is
// indistinguishable. If close-up normals ever read soft, the upgrade is to transform the analytic
// derivatives in a second packed pair and drop the differencing here.

Texture2DArray<float4> spectrumIn : register(t0, space0);
Texture2DArray<float> foamIn : register(t1, space0);

RWTexture2D<float4> displacementOut : register(u0, space1);
RWTexture2D<float4> derivativesOut : register(u1, space1);
RWTexture2D<float> foamOut : register(u2, space1);

// The IFFT ran over bins centred on zero, so its output carries an alternating sign the shift has
// to be undone with. This is the fftshift, folded into the one pass that already touches every
// texel.
float unshift(uint2 texel)
{
    return ((texel.x + texel.y) & 1u) != 0u ? -1.0f : 1.0f;
}

// Slot A holds the height in its real part and the x displacement in its imaginary part; slot B
// holds the z displacement. Choppiness scales the horizontal pair here, before anything measures
// the folding, so foam responds to the displacement actually drawn.
float3 displacementAt(uint2 texel)
{
    const float4 packed = spectrumIn.Load(int4(int2(texel), int(cascadeIndex), 0));
    return float3(packed.y * choppiness, packed.x, packed.z * choppiness) * unshift(texel);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint2 texel = id.xy;
    if (texel.x >= OCEAN_FFT_SIZE || texel.y >= OCEAN_FFT_SIZE) return;

    const uint mask = OCEAN_FFT_SIZE - 1u;
    const uint2 left = uint2((texel.x - 1u) & mask, texel.y);
    const uint2 right = uint2((texel.x + 1u) & mask, texel.y);
    const uint2 down = uint2(texel.x, (texel.y - 1u) & mask);
    const uint2 up = uint2(texel.x, (texel.y + 1u) & mask);

    const float3 centre = displacementAt(texel);
    const float3 dLeft = displacementAt(left);
    const float3 dRight = displacementAt(right);
    const float3 dDown = displacementAt(down);
    const float3 dUp = displacementAt(up);

    // Central differences. The tile wraps, so the neighbour lookups above wrap with it and the
    // seam needs no special case.
    const float texelSize = patchSize / float(OCEAN_FFT_SIZE);
    const float inverseSpan = 1.0f / (2.0f * texelSize);

    const float3 ddx = (dRight - dLeft) * inverseSpan;
    const float3 ddz = (dUp - dDown) * inverseSpan;

    // Slopes are metres per metre in every cascade, so the fragment shader can add them across
    // cascades and build one normal from the sum. Storing normals per cascade instead would need
    // an unnormalize-add-renormalize on every sample.
    const float2 slope = float2(ddx.y, ddz.y);

    // Determinant of the horizontal displacement's jacobian. It drops below zero exactly where
    // the surface folds back over itself, which is where a real wave is breaking.
    const float jacobian = (1.0f + ddx.x) * (1.0f + ddz.z) - ddx.z * ddz.x;

    displacementOut[texel] = float4(centre, 0.0f);
    derivativesOut[texel] = float4(slope, jacobian, 0.0f);

    // Foam builds where the surface folds and fades everywhere else. Both rates run on real frame
    // time, not the fixed tick: this is decoration, nothing samples it back.
    const float previous = foamIn.Load(int4(int2(texel), int(cascadeIndex), 0));
    const float decayed = previous * exp(-foamDecayRate * deltaTime);
    const float injected = saturate(foamThreshold - jacobian) * foamGrowRate * deltaTime;
    foamOut[texel] = saturate(decayed + injected);
}
