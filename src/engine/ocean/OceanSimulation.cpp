//
// Created by Peter Gilbert on 8/14/26.
//

#include "OceanSimulation.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include "../Constants.h"
#include "../Profiling.h"

namespace ytail::ocean {
    namespace {
        constexpr float kPi = 3.14159265358979323846f;
        constexpr float kTwoPi = 2.0f * kPi;

        // In-place iterative radix-2 inverse DFT. Bit-reversal permutation up front, then the
        // usual Danielson-Lanczos passes with a positive exponent.
        //
        // No 1/N normalization, matching OceanIFFT.comp.hlsl: the surface is literally the sum
        // over k of h(k) e^(ik.x), which is the unnormalized inverse, and the physical amplitude
        // already lives in the spectrum's per-bin variance.
        void inverseTransform(std::complex<float>* data, const int count) {
            for (int i = 1, j = 0; i < count; ++i) {
                int bit = count >> 1;
                for (; (j & bit) != 0; bit >>= 1) j ^= bit;
                j ^= bit;
                if (i < j) std::swap(data[i], data[j]);
            }

            for (int span = 2; span <= count; span <<= 1) {
                const int halfSpan = span / 2;
                for (int start = 0; start < count; start += span) {
                    for (int offset = 0; offset < halfSpan; ++offset) {
                        // Computed rather than accumulated. Marching a twiddle by repeated
                        // multiplication drifts, and the whole point of this class is that it
                        // agrees with the GPU.
                        const float angle = kTwoPi * static_cast<float>(offset)
                                          / static_cast<float>(span);
                        const std::complex<float> twiddle(std::cos(angle), std::sin(angle));

                        const std::complex<float> low = data[start + offset];
                        const std::complex<float> high = data[start + offset + halfSpan] * twiddle;
                        data[start + offset] = low + high;
                        data[start + offset + halfSpan] = low - high;
                    }
                }
            }
        }

        // Rows then columns. The column pass gathers into scratch because the transform wants its
        // input contiguous.
        void inverseTransform2D(std::vector<std::complex<float>>& field, const int size,
                                std::vector<std::complex<float>>& scratch) {
            for (int row = 0; row < size; ++row) {
                inverseTransform(field.data() + static_cast<size_t>(row) * size, size);
            }

            scratch.resize(size);
            for (int column = 0; column < size; ++column) {
                for (int row = 0; row < size; ++row) {
                    scratch[row] = field[static_cast<size_t>(row) * size + column];
                }
                inverseTransform(scratch.data(), size);
                for (int row = 0; row < size; ++row) {
                    field[static_cast<size_t>(row) * size + column] = scratch[row];
                }
            }
        }

        int wrapIndex(const int index, const int size) {
            const int wrapped = index % size;
            return wrapped < 0 ? wrapped + size : wrapped;
        }
    } // namespace

    void OceanSimulation::setSettings(const OceanSettings& newSettings) {
        const bool spectrumChanged =
            std::memcmp(&settings.spectrum, &newSettings.spectrum, sizeof(SpectrumParams)) != 0
            || std::memcmp(settings.cascades.data(), newSettings.cascades.data(),
                           sizeof(CascadeParams) * ocean::CascadeCount) != 0
            || settings.randomSeed != newSettings.randomSeed;

        // Choppiness scales the displacement the transform already produced, so it changes the
        // cached fields without changing h0. Everything cached is stale either way.
        const bool fieldChanged = spectrumChanged || settings.choppiness != newSettings.choppiness;

        settings = newSettings;

        // The rebuild itself waits for the next evaluate, which knows which tick's weather to
        // build for. Only the cache has to go now.
        if (spectrumChanged) spectrumValid = false;
        if (fieldChanged) {
            for (Frame& frame : history) frame.valid = false;
        }
    }

    OceanSettings OceanSimulation::settingsForTick(const Uint64 tick) const {
        OceanSettings resolved = settings;
        if (!seaState.empty()) {
            applySeaState(resolved.spectrum, seaState.evaluate(tick), settings);
        }
        return resolved;
    }

    void OceanSimulation::rebuildSpectrum(const SpectrumParams& spectrum) {
        ZoneScoped;
        for (int cascadeIndex = 0; cascadeIndex < SampledCascades; ++cascadeIndex) {
            Cascade& cascade = cascades[cascadeIndex];
            const CascadeParams& params = settings.cascades[cascadeIndex];

            const int size = Resolutions[cascadeIndex];
            const auto texelCount = static_cast<size_t>(size) * size;

            cascade.resolution = size;
            cascade.patchSize = params.patchSize;
            cascade.initial.assign(texelCount, {});
            cascade.initialConjugate.assign(texelCount, {});
            cascade.omega.assign(texelCount, 0.0f);

            // Bin spacing follows the patch size alone, so these bins are exactly the
            // low-wavenumber subset of the ones the GPU transforms at 256.
            const float deltaK = kTwoPi / params.patchSize;
            const int centre = size / 2;

            for (int row = 0; row < size; ++row) {
                for (int column = 0; column < size; ++column) {
                    const size_t index = static_cast<size_t>(row) * size + column;

                    const glm::ivec2 waveIndex{ column - centre, row - centre };
                    const glm::vec2 waveVector = glm::vec2(waveIndex) * deltaK;

                    const glm::vec2 gauss = gaussianPair(waveIndex, static_cast<uint32_t>(cascadeIndex),
                                                         settings.randomSeed);
                    const float amplitude = std::sqrt(
                        binVariance(waveVector, deltaK, spectrum, params));
                    cascade.initial[index] = { gauss.x * amplitude, gauss.y * amplitude };

                    // The -k half cannot be derived from the +k half: directional spreading is not
                    // symmetric about the origin, so it has to be evaluated on its own.
                    const glm::ivec2 mirroredIndex{ wrapIndex(size - column, size) - centre,
                                                    wrapIndex(size - row, size) - centre };
                    const glm::vec2 mirroredVector = glm::vec2(mirroredIndex) * deltaK;
                    const glm::vec2 mirroredGauss = gaussianPair(
                        mirroredIndex, static_cast<uint32_t>(cascadeIndex), settings.randomSeed);
                    const float mirroredAmplitude = std::sqrt(
                        binVariance(mirroredVector, deltaK, spectrum, params));
                    cascade.initialConjugate[index] = std::conj(std::complex<float>{
                        mirroredGauss.x * mirroredAmplitude, mirroredGauss.y * mirroredAmplitude });

                    cascade.omega[index] = loopedDispersion(glm::length(waveVector), spectrum);
                }
            }
        }

        builtSpectrum = spectrum;
        spectrumValid = true;
    }

    void OceanSimulation::evaluate(const Uint64 tick, Frame& outFrame) {
        ZoneScoped;
        // Weather is a function of the tick too, so a tick under different weather from the last
        // one we built for needs its own h0. SeaStateTrack quantizes the intensity precisely so
        // this stays rare while a storm is rolling in rather than firing every tick.
        const SpectrumParams spectrum = settingsForTick(tick).spectrum;
        if (!spectrumValid || std::memcmp(&builtSpectrum, &spectrum, sizeof(SpectrumParams)) != 0) {
            rebuildSpectrum(spectrum);
        }

        // Wrapped into the loop period, so a session running for hours evaluates the same small
        // times a fresh one does and never loses phase precision to a growing float.
        const double loopPeriod = std::max(static_cast<double>(spectrum.loopPeriod), 1e-3);
        const auto seconds = static_cast<double>(tick) * constant::FixedDeltaTime * spectrum.timeScale;
        const auto time = static_cast<float>(std::fmod(seconds, loopPeriod));

        for (int cascadeIndex = 0; cascadeIndex < SampledCascades; ++cascadeIndex) {
            const Cascade& cascade = cascades[cascadeIndex];
            const int size = cascade.resolution;
            const auto texelCount = static_cast<size_t>(size) * size;
            const float deltaK = kTwoPi / cascade.patchSize;
            const int centre = size / 2;

            // Same packing as the GPU: one complex field carries the height and the x
            // displacement, the other carries z. Both spectra are hermitian, so the inverse
            // transform of (F + iG) comes out as two real fields.
            slotAField.assign(texelCount, {});
            slotBField.assign(texelCount, {});

            for (int row = 0; row < size; ++row) {
                for (int column = 0; column < size; ++column) {
                    const size_t index = static_cast<size_t>(row) * size + column;

                    const glm::vec2 waveVector = glm::vec2(static_cast<float>(column - centre),
                                                           static_cast<float>(row - centre)) * deltaK;
                    const float waveNumber = glm::length(waveVector);
                    if (waveNumber < 1e-6f) continue; // mean sea level carries no wave

                    const float phase = cascade.omega[index] * time;
                    const std::complex<float> rotation(std::cos(phase), std::sin(phase));

                    const std::complex<float> height =
                        cascade.initial[index] * rotation
                        + cascade.initialConjugate[index] * std::conj(rotation);

                    const glm::vec2 direction = waveVector / waveNumber;
                    // Dx's spectrum is -i * (kx/|k|) * height, so i * Dx collapses to a real
                    // scale on height and the height and x displacement ride one transform.
                    slotAField[index] = height * (1.0f + direction.x);
                    // -i * (kz/|k|) * height
                    slotBField[index] = std::complex<float>(height.imag(), -height.real()) * direction.y;
                }
            }

            inverseTransform2D(slotAField, size, rowScratch);
            inverseTransform2D(slotBField, size, rowScratch);

            std::vector<glm::vec3>& output = outFrame.displacement[cascadeIndex];
            output.resize(texelCount);

            for (int row = 0; row < size; ++row) {
                for (int column = 0; column < size; ++column) {
                    const size_t index = static_cast<size_t>(row) * size + column;
                    // The spectrum was laid out centred on zero, so the transform's output carries
                    // an alternating sign the shift has to be undone with.
                    const float unshift = ((column + row) & 1) != 0 ? -1.0f : 1.0f;
                    output[index] = glm::vec3(slotAField[index].imag() * settings.choppiness,
                                              slotAField[index].real(),
                                              slotBField[index].real() * settings.choppiness) * unshift;
                }
            }
        }

        outFrame.tick = tick;
        outFrame.seaStateVersion = seaState.getVersion();
        outFrame.valid = true;
    }

    const OceanSimulation::Frame& OceanSimulation::frameFor(const Uint64 tick) {
        Frame& slot = history[tick % static_cast<Uint64>(HistoryTicks)];
        if (!slot.valid || slot.tick != tick || slot.seaStateVersion != seaState.getVersion()) {
            evaluate(tick, slot);
        }
        return slot;
    }

    glm::vec3 OceanSimulation::displacementAt(const Frame& frame, const glm::vec2 gridXZ) const {
        glm::vec3 total{0.0f};

        for (int cascadeIndex = 0; cascadeIndex < SampledCascades; ++cascadeIndex) {
            const int size = cascades[cascadeIndex].resolution;
            if (size == 0) continue;
            const std::vector<glm::vec3>& field = frame.displacement[cascadeIndex];
            if (field.empty()) continue;

            // Texel coordinates, then a wrapping bilinear filter: the patch tiles, so the last
            // column blends into the first rather than clamping.
            const glm::vec2 texel = gridXZ / cascades[cascadeIndex].patchSize * static_cast<float>(size);
            const auto floorX = static_cast<int>(std::floor(texel.x));
            const auto floorY = static_cast<int>(std::floor(texel.y));
            const float fractionX = texel.x - static_cast<float>(floorX);
            const float fractionY = texel.y - static_cast<float>(floorY);

            const int x0 = wrapIndex(floorX, size);
            const int y0 = wrapIndex(floorY, size);
            const int x1 = wrapIndex(floorX + 1, size);
            const int y1 = wrapIndex(floorY + 1, size);

            const glm::vec3& topLeft = field[static_cast<size_t>(y0) * size + x0];
            const glm::vec3& topRight = field[static_cast<size_t>(y0) * size + x1];
            const glm::vec3& bottomLeft = field[static_cast<size_t>(y1) * size + x0];
            const glm::vec3& bottomRight = field[static_cast<size_t>(y1) * size + x1];

            const glm::vec3 top = glm::mix(topLeft, topRight, fractionX);
            const glm::vec3 bottom = glm::mix(bottomLeft, bottomRight, fractionX);
            total += glm::mix(top, bottom, fractionY);
        }

        return total;
    }

    glm::vec2 OceanSimulation::inverseDisplace(const Frame& frame, const glm::vec2 worldXZ) const {
        // The transform gives the surface point for a grid point, and a query asks the other way
        // round. Fixed point: guess the grid point is the query point, then keep subtracting off
        // whatever displacement that guess implies.
        glm::vec2 gridXZ = worldXZ;
        for (int iteration = 0; iteration < InversionIterations; ++iteration) {
            const glm::vec3 displacement = displacementAt(frame, gridXZ);
            gridXZ = worldXZ - glm::vec2(displacement.x, displacement.z);
        }
        return gridXZ;
    }

    float OceanSimulation::heightAt(const glm::vec2 worldXZ, const Uint64 tick) {
        const Frame& frame = frameFor(tick);
        const glm::vec2 gridXZ = inverseDisplace(frame, worldXZ);
        return displacementAt(frame, gridXZ).y + settings.seaLevel;
    }

    OceanSample OceanSimulation::sample(const glm::vec2 worldXZ, const Uint64 tick) {
        OceanSample result{};
        sampleBatch(&worldXZ, 1, tick, &result);
        return result;
    }

    void OceanSimulation::sampleBatch(const glm::vec2* points, const size_t count,
                                      const Uint64 tick, OceanSample* outSamples) {
        ZoneScoped;
        if (points == nullptr || outSamples == nullptr || count == 0) return;

        // Both frames are resolved once for the whole batch. The two ticks land in different ring
        // slots (HistoryTicks is far greater than one), so neither reference invalidates the other.
        const Frame& current = frameFor(tick);
        const Frame& previous = tick > 0 ? frameFor(tick - 1) : current;
        const bool hasPrevious = tick > 0;

        // Finite differences for the normal, spaced one texel of the finest cascade apart. Closer
        // than that just resamples the same bilinear cell and returns its constant gradient.
        const int finest = SampledCascades - 1;
        const float step = cascades[finest].resolution > 0
            ? cascades[finest].patchSize / static_cast<float>(cascades[finest].resolution)
            : 1.0f;

        for (size_t i = 0; i < count; ++i) {
            const glm::vec2 worldXZ = points[i];
            const glm::vec2 gridXZ = inverseDisplace(current, worldXZ);
            const glm::vec3 displacement = displacementAt(current, gridXZ);

            const float heightRight = displacementAt(current, gridXZ + glm::vec2(step, 0.0f)).y;
            const float heightLeft = displacementAt(current, gridXZ - glm::vec2(step, 0.0f)).y;
            const float heightUp = displacementAt(current, gridXZ + glm::vec2(0.0f, step)).y;
            const float heightDown = displacementAt(current, gridXZ - glm::vec2(0.0f, step)).y;

            const float slopeX = (heightRight - heightLeft) / (2.0f * step);
            const float slopeZ = (heightUp - heightDown) / (2.0f * step);

            // Differencing two cached ticks rather than transforming the analytic time derivative.
            // It is the same derivative the simulation actually takes, and the previous tick is
            // already in the cache.
            const glm::vec3 velocity = hasPrevious
                ? (displacement - displacementAt(previous, gridXZ)) / constant::FixedDeltaTime
                : glm::vec3(0.0f);

            OceanSample& out = outSamples[i];
            out.height = displacement.y + settings.seaLevel;
            out.normal = glm::normalize(glm::vec3(-slopeX, 1.0f, -slopeZ));
            out.velocity = velocity;
        }
    }

    bool OceanSimulation::isUnderwater(const glm::vec3& worldPosition, const Uint64 tick) {
        return worldPosition.y < heightAt(glm::vec2(worldPosition.x, worldPosition.z), tick);
    }
} // ytail::ocean
