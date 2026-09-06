#ifndef OCEAN_COMMON_HLSLI
#define OCEAN_COMMON_HLSLI

// Shared by every ocean compute shader. The spectrum half is a transliteration of
// engine/src/engine/ocean/OceanSpectrum.cpp; the CPU sampler and the drawn surface only agree
// while these two stay in step, so change them together.

static const float kPi = 3.14159265358979f;
static const float kTwoPi = 6.28318530717959f;
static const float kGravity = 9.81f;

// FFT size, fixed at compile time because the IFFT sizes its groupshared buffer and its thread
// count from it. Mirrored by OceanRenderer::Resolution; changing one means changing both.
#define OCEAN_FFT_SIZE 256u
#define OCEAN_FFT_LOG2 8u

// Compute uniform buffers live in space2 (SDL_GPU compute convention: t/space0 read-only,
// u/space1 read-write, b/space2 uniforms). Mirrors OceanComputeUniform in OceanRenderer.h.
cbuffer OceanCompute : register(b0, space2)
{
    // --- SpectrumParams ---
    float windSpeed;
    float windDirection;
    float fetch;
    float peakEnhancement;

    float spreadBlend;
    float swell;
    float depth;
    float shortWavesFade;

    float amplitude;
    float loopPeriod;
    float _spectrumPad0;
    float _spectrumPad1;

    // --- CascadeParams for the cascade this dispatch is working on ---
    float patchSize;
    float cutoffLow;
    float cutoffHigh;
    float _cascadePad0;

    // --- per-dispatch ---
    uint cascadeIndex;  // which array layer the read-only bindings should sample
    uint randomSeed;    // shifts every gaussian, so a new seed is a whole new ocean
    uint fftAxis;       // 0 = transform rows, 1 = transform columns
    uint _dispatchPad0;

    float time;         // simulation seconds, wrapped into the spectrum's loop period
    float deltaTime;    // real frame seconds, foam accumulation only
    float choppiness;   // horizontal displacement scale, applied before the jacobian
    float foamThreshold;

    float foamDecayRate;
    float foamGrowRate;
    float _pad0;
    float _pad1;
};

// Bin index -> signed wavenumber index. Bin i carries index (i - N/2), so the spectrum sits centred
// on zero and the IFFT output needs the (-1)^(x+y) unshift that OceanDerivatives applies.
int2 waveIndexAt(uint2 bin)
{
    return int2(bin) - int(OCEAN_FFT_SIZE) / 2;
}

float2 waveVectorFromIndex(int2 waveIndex)
{
    return float2(waveIndex) * (kTwoPi / patchSize);
}

// The bin holding -k for the bin holding +k. The zeroth bin is its own mirror, which is the
// Nyquist wavenumber being self-conjugate rather than a bug.
uint2 mirrorBin(uint2 bin)
{
    return uint2((OCEAN_FFT_SIZE - bin.x) % OCEAN_FFT_SIZE,
                 (OCEAN_FFT_SIZE - bin.y) % OCEAN_FFT_SIZE);
}

// --- complex arithmetic, stored as float2(real, imaginary) ---

float2 complexMul(float2 a, float2 b)
{
    return float2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x);
}

// Multiplying by -i, which is how a height spectrum becomes a horizontal displacement one.
float2 complexMulNegI(float2 a)
{
    return float2(a.y, -a.x);
}

float2 complexConjugate(float2 a)
{
    return float2(a.x, -a.y);
}

// --- deterministic gaussian noise ---

// Integer avalanche hash. Exact in both HLSL and C++, which is what lets the CPU sampler seed
// itself identically without uploading a noise texture.
uint hashUint(uint x)
{
    x ^= x >> 17; x *= 0xed5ad4bbu;
    x ^= x >> 11; x *= 0xac4c1b51u;
    x ^= x >> 15; x *= 0x31848babu;
    x ^= x >> 14;
    return x;
}

float hashToUnitFloat(uint h)
{
    // Top 24 bits into [0,1). Dropping the low bits keeps the result identical whatever the
    // float rounding mode, since 24 bits is exactly a float's mantissa.
    return float(h >> 8) * (1.0f / 16777216.0f);
}

// Box-Muller: two independent standard normals from one seed.
//
// Seeded by the signed wavenumber index, never the texture bin. The CPU sampler runs a smaller
// grid over the same patch size, so a given wave lands on a different bin there but the same
// wavenumber; seeding on the wavenumber is what makes both grids draw the same ocean.
float2 gaussianPair(int2 waveIndex, uint cascade, uint seed)
{
    const uint base = hashUint(uint(waveIndex.x) * 73856093u ^ uint(waveIndex.y) * 19349663u
                             ^ cascade * 83492791u ^ seed);
    const float u1 = max(hashToUnitFloat(base), 1e-7f);
    const float u2 = hashToUnitFloat(hashUint(base));

    const float radius = sqrt(-2.0f * log(u1));
    const float theta = kTwoPi * u2;
    return float2(radius * cos(theta), radius * sin(theta));
}

// --- spectrum ---

float peakOmega()
{
    return 22.0f * pow(kGravity * kGravity / max(windSpeed * fetch, 1e-3f), 1.0f / 3.0f);
}

float jonswapAlpha()
{
    return 0.076f * pow(windSpeed * windSpeed / max(fetch * kGravity, 1e-3f), 0.22f);
}

// Finite-depth dispersion. tanh saturates well before kh = 20, so the clamp only guards the
// exponentials inside it.
float dispersion(float k)
{
    return sqrt(kGravity * k * tanh(min(k * depth, 20.0f)));
}

// Dispersion snapped to a multiple of 2*pi/loopPeriod, so every wave completes a whole number of
// cycles per loop and the field repeats exactly. Time evolution uses this; the spectrum itself uses
// the unsnapped value, since that is a change of variables rather than a phase.
float loopedDispersion(float k)
{
    const float omega = dispersion(k);
    const float baseOmega = kTwoPi / max(loopPeriod, 1e-3f);
    // Never round down to zero: that would freeze the longest waves solid.
    return max(round(omega / baseOmega), 1.0f) * baseOmega;
}

float dispersionDerivative(float k)
{
    const float kh = min(k * depth, 20.0f);
    const float tanhKh = tanh(kh);
    const float omega = sqrt(kGravity * k * tanhKh);
    if (omega < 1e-6f) return 0.0f;
    const float sech2 = 1.0f - tanhKh * tanhKh;
    return kGravity * (tanhKh + kh * sech2) / (2.0f * omega);
}

// Kitaigorodskii depth attenuation, the TMA half of the spectrum. Exactly 1 in deep water.
float depthAttenuation(float omega)
{
    const float omegaH = omega * sqrt(depth / kGravity);
    if (omegaH <= 1.0f) return 0.5f * omegaH * omegaH;
    if (omegaH < 2.0f)
    {
        const float t = 2.0f - omegaH;
        return 1.0f - 0.5f * t * t;
    }
    return 1.0f;
}

float jonswap(float omega)
{
    if (omega < 1e-4f) return 0.0f;

    const float peak = peakOmega();
    const float sigma = omega <= peak ? 0.07f : 0.09f;
    const float delta = omega - peak;
    const float r = exp(-(delta * delta) / (2.0f * sigma * sigma * peak * peak));

    const float omega2 = omega * omega;
    const float omega4 = omega2 * omega2;
    const float peakRatio = peak / omega;
    const float peakRatio4 = peakRatio * peakRatio * peakRatio * peakRatio;

    const float base = jonswapAlpha() * kGravity * kGravity / (omega4 * omega)
                     * exp(-1.25f * peakRatio4);

    return base * pow(peakEnhancement, r) * depthAttenuation(omega);
}

// Longuet-Higgins normalization for cos^2s(theta/2), as the standard two-piece polynomial fit.
float spreadNormalization(float s)
{
    const float s2 = s * s;
    const float s3 = s2 * s;
    const float s4 = s3 * s;
    if (s < 5.0f)
    {
        return -0.000564f * s4 + 0.00776f * s3 - 0.044f * s2 + 0.192f * s + 0.163f;
    }
    return -4.80e-08f * s4 + 1.07e-05f * s3 - 9.53e-04f * s2 + 5.90e-02f * s + 3.93e-01f;
}

float spreadPower(float omega, float peak)
{
    const float ratio = omega / peak;
    if (omega > peak) return 9.77f * pow(ratio, -2.5f);
    return 6.97f * pow(ratio, 5.0f);
}

float directionalSpread(float omega, float theta)
{
    const float peak = peakOmega();
    const float s = spreadPower(omega, peak)
                  + 16.0f * tanh(min(omega / peak, 20.0f)) * swell * swell;

    const float lobe = spreadNormalization(s) * pow(abs(cos(theta * 0.5f)), 2.0f * s);
    const float cosTheta = cos(theta);
    const float isotropic = 2.0f / kPi * cosTheta * cosTheta;

    return lerp(isotropic, lobe, spreadBlend);
}

// Variance one bin should average to, i.e. the target for |h0(k)|^2. Zero at k == 0 and outside
// this cascade's band, so callers write the result unconditionally.
float binVariance(float2 waveVector, float deltaK)
{
    const float k = length(waveVector);
    if (k < 1e-6f) return 0.0f;
    if (k < cutoffLow || k >= cutoffHigh) return 0.0f;

    const float omega = dispersion(k);
    const float theta = atan2(waveVector.y, waveVector.x) - windDirection;

    // S(omega) d(omega) d(theta) -> S(k) k dk d(theta) costs a dOmega/dk and a 1/k; the bin area
    // then turns the density into this bin's share.
    const float density = jonswap(omega) * directionalSpread(omega, theta)
                        * dispersionDerivative(k) / k;

    const float fade = exp(-(shortWavesFade * shortWavesFade) * k * k);

    // The quarter is not arbitrary, and getting it wrong drowns the whole sea in foam.
    //
    // The surface is synthesised as sum over k of h(k,t), where h(k,t) pairs h0(k) with
    // conj(h0(-k)). Those are independent draws, so E[|h(k,t)|^2] = 2 E[|h0|^2], and that has to
    // come out as S(k) dk^2 for the sea to have the variance the spectrum says it does. The caller
    // builds h0 as (gauss + i gauss) * sqrt(this), which is itself a factor of two because both
    // gaussians carry unit variance. Two twos, so a quarter.
    return 0.25f * density * deltaK * deltaK * fade * amplitude;
}

#endif // OCEAN_COMMON_HLSLI
