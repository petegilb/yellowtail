//
// Created by Peter Gilbert on 8/14/26.
//

#include "OceanSpectrum.h"

#include <algorithm>
#include <cmath>

namespace ytail::ocean {
    namespace {
        constexpr float kPi = 3.14159265358979323846f;
        constexpr float kTwoPi = 2.0f * kPi;

        // Longuet-Higgins normalization for the cos^2s(theta/2) spreading, as a polynomial fit.
        // The closed form is a ratio of gamma functions, which is neither cheap nor expressible in
        // HLSL; this is the standard two-piece fit that every reference implementation uses.
        float spreadNormalization(const float s) {
            const float s2 = s * s;
            const float s3 = s2 * s;
            const float s4 = s3 * s;
            if (s < 5.0f) {
                return -0.000564f * s4 + 0.00776f * s3 - 0.044f * s2 + 0.192f * s + 0.163f;
            }
            return -4.80e-08f * s4 + 1.07e-05f * s3 - 9.53e-04f * s2 + 5.90e-02f * s + 3.93e-01f;
        }

        // How tightly the energy at omega clusters around the wind direction. Sharpest at the peak
        // and broadening away from it, which is what stops the sea looking like a corduroy sheet.
        float spreadPower(const float omega, const float peak) {
            const float ratio = omega / peak;
            if (omega > peak) return 9.77f * std::pow(ratio, -2.5f);
            return 6.97f * std::pow(ratio, 5.0f);
        }

        // Kitaigorodskii depth attenuation: the TMA half of the TMA spectrum. Exactly 1 in deep
        // water, so it costs nothing when depth is large.
        float depthAttenuation(const float omega, const float depth) {
            const float omegaH = omega * std::sqrt(depth / Gravity);
            if (omegaH <= 1.0f) return 0.5f * omegaH * omegaH;
            if (omegaH < 2.0f) {
                const float t = 2.0f - omegaH;
                return 1.0f - 0.5f * t * t;
            }
            return 1.0f;
        }
    } // namespace

    std::array<CascadeParams, CascadeCount> defaultCascades() {
        // The band edge between a cascade and the next sits at 6 bins of the finer tile, which is
        // comfortably inside that tile's Nyquist limit while still overlapping nothing.
        // The first patch has to be several times the peak wavelength, not just larger than it.
        // Bin spacing is 2*pi/patchSize, so at 250m a 12 m/s sea puts its spectral peak less than
        // four bins from the origin and the whole swell is built from a handful of components.
        // That reads as a few sine waves marching past rather than as a sea. At 500m the peak
        // lands near bin seven, which is enough to look like weather.
        constexpr float patchSizes[CascadeCount] = { 500.0f, 42.0f, 7.0f };
        constexpr float boundaryBins = 6.0f;

        std::array<CascadeParams, CascadeCount> cascades{};
        for (int i = 0; i < CascadeCount; ++i) {
            cascades[i].patchSize = patchSizes[i];
            cascades[i].cutoffLow = i == 0 ? 0.0f : boundaryBins * kTwoPi / patchSizes[i];
            cascades[i].cutoffHigh = i == CascadeCount - 1
                                         ? 1e9f
                                         : boundaryBins * kTwoPi / patchSizes[i + 1];
        }
        return cascades;
    }

    float peakOmega(const SpectrumParams& params) {
        const float windFetch = std::max(params.windSpeed * params.fetch, 1e-3f);
        return 22.0f * std::pow(Gravity * Gravity / windFetch, 1.0f / 3.0f);
    }

    float jonswapAlpha(const SpectrumParams& params) {
        const float fetchG = std::max(params.fetch * Gravity, 1e-3f);
        return 0.076f * std::pow(params.windSpeed * params.windSpeed / fetchG, 0.22f);
    }

    float dispersion(const float waveNumber, const float depth) {
        return std::sqrt(Gravity * waveNumber * std::tanh(std::min(waveNumber * depth, 20.0f)));
    }

    float dispersionDerivative(const float waveNumber, const float depth) {
        const float kh = std::min(waveNumber * depth, 20.0f);
        const float tanhKh = std::tanh(kh);
        const float omega = std::sqrt(Gravity * waveNumber * tanhKh);
        if (omega < 1e-6f) return 0.0f;
        // d/dk of g k tanh(kh), over 2 omega. sech^2 = 1 - tanh^2.
        const float sech2 = 1.0f - tanhKh * tanhKh;
        return Gravity * (tanhKh + kh * sech2) / (2.0f * omega);
    }

    float loopedDispersion(const float waveNumber, const SpectrumParams& params) {
        const float omega = dispersion(waveNumber, params.depth);
        const float baseOmega = kTwoPi / std::max(params.loopPeriod, 1e-3f);
        // Never round down to zero: that would freeze the longest waves solid.
        return std::max(std::round(omega / baseOmega), 1.0f) * baseOmega;
    }

    float jonswap(const float omega, const SpectrumParams& params) {
        if (omega < 1e-4f) return 0.0f;

        const float peak = peakOmega(params);
        const float alpha = jonswapAlpha(params);

        // Peak enhancement: a bump of width sigma centred on the peak, raised to gamma. This is
        // the whole difference between JONSWAP and Pierson-Moskowitz.
        const float sigma = omega <= peak ? 0.07f : 0.09f;
        const float delta = omega - peak;
        const float r = std::exp(-(delta * delta) / (2.0f * sigma * sigma * peak * peak));

        const float omega2 = omega * omega;
        const float omega4 = omega2 * omega2;
        const float peakRatio = peak / omega;
        const float peakRatio4 = peakRatio * peakRatio * peakRatio * peakRatio;

        const float base = alpha * Gravity * Gravity / (omega4 * omega)
                         * std::exp(-1.25f * peakRatio4);

        return base * std::pow(params.peakEnhancement, r) * depthAttenuation(omega, params.depth);
    }

    float directionalSpread(const float omega, const float theta, const SpectrumParams& params) {
        const float peak = peakOmega(params);

        // The swell term widens the exponent well past what wind alone gives, which is what pulls
        // energy into a narrow band of directions and reads as a long ordered swell.
        const float s = spreadPower(omega, peak)
                      + 16.0f * std::tanh(std::min(omega / peak, 20.0f)) * params.swell * params.swell;

        const float cosHalf = std::abs(std::cos(theta * 0.5f));
        const float lobe = spreadNormalization(s) * std::pow(cosHalf, 2.0f * s);

        const float cosTheta = std::cos(theta);
        const float isotropic = 2.0f / kPi * cosTheta * cosTheta;

        return isotropic + (lobe - isotropic) * params.spreadBlend;
    }

    float binVariance(const glm::vec2 waveVector, const float deltaK,
                      const SpectrumParams& params, const CascadeParams& cascade) {
        const float k = std::sqrt(waveVector.x * waveVector.x + waveVector.y * waveVector.y);

        // The k == 0 bin is the mean sea level and carries no wave; the band test keeps each
        // wavelength in exactly one cascade.
        if (k < 1e-6f) return 0.0f;
        if (k < cascade.cutoffLow || k >= cascade.cutoffHigh) return 0.0f;

        const float omega = dispersion(k, params.depth);
        const float theta = std::atan2(waveVector.y, waveVector.x) - params.windDirection;

        // S(omega) d(omega) d(theta) becomes S(k) k dk d(theta), so the change of variables costs a
        // dOmega/dk and a 1/k. Multiplying by the bin area then turns the density into the variance
        // this one bin should hold.
        const float density = jonswap(omega, params)
                            * directionalSpread(omega, theta, params)
                            * dispersionDerivative(k, params.depth) / k;

        // Waves far below a texel only alias, so roll them off rather than paying to render noise.
        const float fade = std::exp(-(params.shortWavesFade * params.shortWavesFade) * k * k);

        // The quarter is not arbitrary, and getting it wrong drowns the whole sea in foam.
        //
        // The surface is synthesised as sum over k of h(k,t), where h(k,t) pairs h0(k) with
        // conj(h0(-k)). Those are independent draws, so E[|h(k,t)|^2] = 2 E[|h0|^2], and that has
        // to come out as S(k) dk^2 for the sea to have the variance the spectrum says it does.
        // The caller builds h0 as (gauss + i gauss) * sqrt(this), which is itself a factor of two
        // because both gaussians carry unit variance. Two twos, so a quarter.
        return 0.25f * density * deltaK * deltaK * fade * params.amplitude;
    }

    uint32_t hashUint(uint32_t x) {
        x ^= x >> 17; x *= 0xed5ad4bbu;
        x ^= x >> 11; x *= 0xac4c1b51u;
        x ^= x >> 15; x *= 0x31848babu;
        x ^= x >> 14;
        return x;
    }

    glm::vec2 gaussianPair(const glm::ivec2 waveIndex, const uint32_t cascade, const uint32_t seed) {
        const uint32_t base = hashUint(static_cast<uint32_t>(waveIndex.x) * 73856093u
                                     ^ static_cast<uint32_t>(waveIndex.y) * 19349663u
                                     ^ cascade * 83492791u ^ seed);

        // Top 24 bits into [0,1). Dropping the low bits keeps the result identical whatever the
        // rounding mode, since 24 bits is exactly a float's mantissa.
        const auto toUnitFloat = [](const uint32_t hash) {
            return static_cast<float>(hash >> 8) * (1.0f / 16777216.0f);
        };

        const float u1 = std::max(toUnitFloat(base), 1e-7f);
        const float u2 = toUnitFloat(hashUint(base));

        const float radius = std::sqrt(-2.0f * std::log(u1));
        const float theta = kTwoPi * u2;
        return { radius * std::cos(theta), radius * std::sin(theta) };
    }
} // ytail::ocean
