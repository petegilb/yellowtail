#include "OceanCommon.hlsli"

// h0(k): the time-independent half of the wave field. Run once at startup and again whenever the
// spectrum parameters move (a sea-state change), never per frame.
//
// Stores h0(k) alongside conj(h0(-k)) so OceanTimeEvolve needs one texel fetch instead of two.
// The -k half cannot be derived from the +k half here, because directional spreading is not
// symmetric about the origin: cos^2s(theta/2) differs at theta and theta + pi.

RWTexture2D<float4> initialSpectrum : register(u0, space1);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint2 bin = id.xy;
    if (bin.x >= OCEAN_FFT_SIZE || bin.y >= OCEAN_FFT_SIZE) return;

    const float deltaK = kTwoPi / patchSize;

    const int2 waveIndex = waveIndexAt(bin);
    const float2 gauss = gaussianPair(waveIndex, cascadeIndex, randomSeed);
    const float2 h0 = gauss * sqrt(binVariance(waveVectorFromIndex(waveIndex), deltaK));

    const int2 mirroredIndex = waveIndexAt(mirrorBin(bin));
    const float2 mirroredGauss = gaussianPair(mirroredIndex, cascadeIndex, randomSeed);
    const float2 h0Minus = mirroredGauss * sqrt(binVariance(waveVectorFromIndex(mirroredIndex), deltaK));

    initialSpectrum[bin] = float4(h0, complexConjugate(h0Minus));
}
