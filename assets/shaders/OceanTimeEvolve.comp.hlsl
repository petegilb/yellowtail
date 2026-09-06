#include "OceanCommon.hlsli"

// h(k, t) from the stored h0, packed into the two complex slots the IFFT transforms together.
//
// The packing relies on both slots holding hermitian spectra. If F and G are each hermitian then
// IFFT(F + iG) = f + ig with f and g both real, so one complex transform carries two real fields:
//
//   slot A = Dy + i * Dx     ->  real part is the height, imaginary part the x displacement
//   slot B = Dz + i * 0      ->  real part is the z displacement
//
// B's imaginary half is deliberately spare. Filling it needs a second hermitian field worth
// having, which is only true once we want analytic derivatives; until then OceanDerivatives gets
// the same information from finite differences for far less than a third transform.

Texture2DArray<float4> initialSpectrum : register(t0, space0);
RWTexture2D<float4> spectrumOut : register(u0, space1);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint2 bin = id.xy;
    if (bin.x >= OCEAN_FFT_SIZE || bin.y >= OCEAN_FFT_SIZE) return;

    const float2 waveVector = waveVectorFromIndex(waveIndexAt(bin));
    const float k = length(waveVector);

    // The k == 0 bin is mean sea level: no wave, and dividing by it would produce a NaN that the
    // IFFT would smear across the whole tile.
    if (k < 1e-6f)
    {
        spectrumOut[bin] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    const float4 packed = initialSpectrum.Load(int4(int2(bin), int(cascadeIndex), 0));
    const float2 h0 = packed.xy;          // h0(k)
    const float2 h0MinusConj = packed.zw; // conj(h0(-k))

    // Waves at +k and -k counter-rotate, and pairing them is what keeps the height field real.
    const float omega = loopedDispersion(k);
    float sinOmegaT, cosOmegaT;
    sincos(omega * time, sinOmegaT, cosOmegaT);
    const float2 phase = float2(cosOmegaT, sinOmegaT);

    const float2 height = complexMul(h0, phase) + complexMul(h0MinusConj, complexConjugate(phase));

    // Dx spectrum is -i * (kx/|k|) * height, so i * Dx collapses to a real scale on height.
    const float2 direction = waveVector / k;
    const float2 slotA = height * (1.0f + direction.x);
    const float2 slotB = complexMulNegI(height) * direction.y;

    spectrumOut[bin] = float4(slotA, slotB);
}
