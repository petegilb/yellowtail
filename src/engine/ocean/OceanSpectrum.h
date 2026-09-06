//
// The wave spectrum, shared by the CPU sampler and the GPU simulation.
//
// JONSWAP (Joint North Sea Wave Project) rather than Tessendorf's original Phillips spectrum.
// Phillips has no fetch and no peak sharpness, so it cannot express the difference between choppy
// near-shore water and a long ocean swell, which is the control a ship game wants most.
//
// OceanInitialSpectrum.comp.hlsl is a direct transliteration of the functions here. The two have to
// agree numerically or the CPU surface drifts away from the drawn one, so change them together.
//

#ifndef YELLOWTAIL_OCEANSPECTRUM_H
#define YELLOWTAIL_OCEANSPECTRUM_H

#include <array>
#include <cstdint>

#include <glm/vec2.hpp>

namespace ytail::ocean {
    inline constexpr float Gravity = 9.81f;

    // Frequency bands the surface is split across. Fixed rather than runtime-configurable: the
    // GPU texture arrays and the shader's cascade loop are both sized by it.
    inline constexpr int CascadeCount = 3;

    // Spectrum shape plus the one art-direction knob that belongs at this level. Laid out in
    // 16-byte rows to match cbuffer Spectrum in the ocean compute shaders, so it uploads with no
    // repacking.
    struct SpectrumParams {
        // Wind speed at 10m, m/s. With fetch, this is what JONSWAP is really parameterized by.
        float windSpeed = 12.0f;
        // Radians, 0 = +X. Waves travel along this.
        float windDirection = 0.0f;
        // Metres of open water the wind has blown across. Longer fetch = longer, more ordered swell.
        float fetch = 100000.0f;
        // JONSWAP gamma. 1.0 collapses to Pierson-Moskowitz (fully developed sea), 3.3 is the
        // JONSWAP mean, higher gives a sharper, more single-wavelength swell.
        float peakEnhancement = 3.3f;

        // 0 = isotropic cos^2, 1 = full Longuet-Higgins spreading. Isotropic looks wrong on an open
        // ocean; this exists to dial the effect back, not to turn it off.
        float spreadBlend = 1.0f;
        // Narrows the spread and biases energy toward the peak. The knob that turns confused chop
        // into long rolling swell.
        float swell = 0.2f;
        // Water depth in metres. Large values reduce the dispersion to deep-water sqrt(g k) and the
        // TMA factor to 1. Kept in the spectrum from the start so shore waves are a parameter
        // change later rather than a rewrite.
        float depth = 500.0f;
        // Gaussian rolloff on the smallest waves, in metres: energy is cut where the wavelength
        // drops below roughly this. Larger = more aggressive.
        //
        // This matters far more than its share of the energy suggests. Surface slope variance goes
        // as the integral of k^2 S(k), which for a wind sea diverges logarithmically, so the
        // shortest waves present always dominate how steep and how busy the surface looks however
        // little energy they carry. Left too small, the sea reads as fast crawling chop no matter
        // how slow and large the swell underneath it is.
        float shortWavesFade = 0.03f;

        // Overall energy scale. Multiplies the variance, so wave height scales with its square root.
        float amplitude = 1.0f;
        // Seconds after which the whole surface repeats. Every wave's frequency is snapped to a
        // multiple of 2*pi/loopPeriod, which costs under a percent of the slowest wave's frequency
        // and buys two things: the simulation time can be wrapped, so a session running for hours
        // never loses float precision in the phase, and the CPU and GPU stay in step for as long
        // as the session lasts rather than drifting apart as the numbers grow.
        float loopPeriod = 200.0f;
        // Multiplies the clock the waves advance on. 1 is physically correct: a deep water wave
        // travels at sqrt(g * wavelength / 2pi), so the default sea's 69m swell has a 6.6 second
        // period whether or not that suits the shot.
        //
        // Reach for the spectrum first when the sea looks too busy, since this only changes how
        // fast waves cross and not how big or how far apart they are. Slowing a short sea down
        // makes it read as syrup rather than as a longer swell.
        float timeScale = 1.0f;
        float _pad0 = 0.0f;
    };
    static_assert(sizeof(SpectrumParams) == 48, "SpectrumParams must match the Spectrum cbuffer layout");

    // One frequency band. Each cascade is a separate FFT tile covering patchSize metres, carrying
    // only the wavenumbers in [cutoffLow, cutoffHigh).
    struct CascadeParams {
        // L: the world-space size one FFT tile covers, metres.
        float patchSize = 250.0f;
        // Band edges in rad/m. cutoffHigh of one cascade is cutoffLow of the next, so no wavelength
        // is generated twice and the cascades sum without doubling the sea's energy.
        float cutoffLow = 0.0f;
        float cutoffHigh = 1e9f;
        float _pad0 = 0.0f;
    };
    static_assert(sizeof(CascadeParams) == 16, "CascadeParams must match the shader struct layout");

    // Patch sizes are deliberately non-harmonic so one tile's repeat does not beat against
    // another's, and the band edges follow from them. 250m swell, 42m chop, 7m ripples.
    std::array<CascadeParams, CascadeCount> defaultCascades();

    // Angular frequency where JONSWAP peaks, from wind speed and fetch.
    float peakOmega(const SpectrumParams& params);
    // The fetch-dependent Phillips constant JONSWAP scales by.
    float jonswapAlpha(const SpectrumParams& params);

    // Finite-depth dispersion and its derivative. Both reduce to the deep-water forms
    // (sqrt(g k), g / 2omega) once k * depth is large.
    float dispersion(float waveNumber, float depth);
    float dispersionDerivative(float waveNumber, float depth);

    // Dispersion snapped to a multiple of 2*pi/loopPeriod, so every wave completes a whole number
    // of cycles per loop and the field repeats exactly. Time evolution uses this; the spectrum
    // itself uses the unsnapped value, since that is a change of variables rather than a phase.
    float loopedDispersion(float waveNumber, const SpectrumParams& params);

    // Energy density at one angular frequency, including the TMA shallow-water correction.
    float jonswap(float omega, const SpectrumParams& params);

    // Normalized directional density: how the energy at omega spreads around the wind direction.
    // theta is measured from the wind, and the result integrates to 1 over a full turn.
    float directionalSpread(float omega, float theta, const SpectrumParams& params);

    // Variance one spectrum bin should average to, i.e. the target for |h0(k)|^2. deltaK is the bin
    // spacing 2*pi/patchSize, which is what turns a density into a per-bin quantity.
    //
    // Returns 0 at k == 0 and outside the cascade's band, so a caller can write the result
    // unconditionally.
    float binVariance(glm::vec2 waveVector, float deltaK,
                      const SpectrumParams& params, const CascadeParams& cascade);

    // Integer avalanche hash. Exact in both C++ and HLSL, which is what lets the CPU sampler seed
    // itself identically to the GPU without uploading a noise texture.
    uint32_t hashUint(uint32_t x);

    // Two independent standard normals, seeded by the signed wavenumber index rather than by a
    // grid position. The CPU sampler runs a smaller grid over the same patch size, so a given wave
    // sits at a different grid index there but the same wavenumber; seeding on the wavenumber is
    // what makes both grids draw the same ocean.
    glm::vec2 gaussianPair(glm::ivec2 waveIndex, uint32_t cascade, uint32_t seed);
} // ytail::ocean

#endif //YELLOWTAIL_OCEANSPECTRUM_H
