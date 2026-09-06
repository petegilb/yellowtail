//
// CPU half of the ocean: what physics and gameplay ask where the water is.
//
// The one property everything else rests on is that this is a pure function of the tick number.
// The netcode replays past ticks through Engine::simulateStep after a correction, and a hull being
// replayed at tick T has to feel the water as it stood at tick T. So nothing here advances; a tick
// goes in and a wave field comes out, with a ring cache in front so a replay costs nothing.
//
// It mirrors the drawn ocean at lower resolution rather than approximating it. Bin spacing is
// 2*pi/patchSize whatever the grid size, so the coarse grid holds exactly the low-wavenumber subset
// of the bins the GPU transforms, seeded identically. The result is the drawn surface low-pass
// filtered, which is what a hull responds to anyway.
//

#ifndef YELLOWTAIL_OCEANSIMULATION_H
#define YELLOWTAIL_OCEANSIMULATION_H

#include <array>
#include <complex>
#include <vector>

#include <SDL3/SDL_stdinc.h>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include "OceanSettings.h"
#include "SeaState.h"

namespace ytail::ocean {
    struct OceanSample {
        // World Y of the surface at the queried point.
        float height = 0.0f;
        glm::vec3 normal{0.0f, 1.0f, 0.0f};
        // How the water itself is moving. The horizontal part is what pushes a hull down a wave
        // face; the vertical part is what drag measures against.
        glm::vec3 velocity{0.0f};
    };

    class OceanSimulation {
    public:
        // How many of the drawn cascades a floating body can actually feel. Deliberately fewer
        // than ocean::CascadeCount: ripples under a metre are visual detail, a hull averages
        // straight over them, and transforming them here would be paid for nothing. Named apart
        // from ocean::CascadeCount so the two are never confused for each other.
        static constexpr int SampledCascades = 2;
        static constexpr int Resolutions[SampledCascades] = { 64, 32 };

        // Ticks kept. Must exceed ReplicationManager::MaxRollbackTicks (48) so a correction's
        // replay always hits the cache; a power of two so the ring index is a mask.
        static constexpr int HistoryTicks = 64;

        // Iterations of the fixed point that inverts the horizontal displacement. Three is
        // converged at any choppiness the surface stays sane at.
        static constexpr int InversionIterations = 3;

        // Regenerates h0 if the spectrum moved. Cheap enough to call every frame.
        void setSettings(const OceanSettings& newSettings);
        [[nodiscard]] const OceanSettings& getSettings() const { return settings; }

        // Scheduled weather. Both peers hold the same keyframes and evaluate them against the
        // shared tick, so the wave field itself never goes over the wire.
        [[nodiscard]] SeaStateTrack& getSeaState() { return seaState; }
        [[nodiscard]] const SeaStateTrack& getSeaState() const { return seaState; }

        // Settings with the sea state for a given tick folded into the spectrum. What the renderer
        // should draw with, so the drawn weather and the sampled weather are the same weather.
        [[nodiscard]] OceanSettings settingsForTick(Uint64 tick) const;

        // Surface at a world xz, for the given tick. Not const: a miss fills the cache, which is
        // the whole point of asking through here rather than recomputing per call.
        OceanSample sample(glm::vec2 worldXZ, Uint64 tick);
        // Height alone, skipping the normal and the second tick the velocity needs.
        float heightAt(glm::vec2 worldXZ, Uint64 tick);
        // One cache lookup for a whole hull's worth of probes instead of one per probe.
        void sampleBatch(const glm::vec2* points, size_t count, Uint64 tick, OceanSample* outSamples);

        bool isUnderwater(const glm::vec3& worldPosition, Uint64 tick);

    private:
        // h0(k) for one cascade, rebuilt only when the spectrum changes.
        struct Cascade {
            int resolution = 0;
            float patchSize = 0.0f;
            // Both halves of the counter-rotating pair, stored so evaluation is one pass.
            std::vector<std::complex<float>> initial;          // h0(k)
            std::vector<std::complex<float>> initialConjugate; // conj(h0(-k))
            std::vector<float> omega;
        };

        // The wave field at one tick. Displacement only: velocity comes from differencing two
        // ticks, which is free once both are cached and is exactly the derivative the simulation
        // actually takes.
        struct Frame {
            Uint64 tick = 0;
            // Which weather schedule this was built under. A schedule arriving from the host
            // rewrites the weather of ticks already simulated, so the tick number alone is not
            // enough to say a cached field is still the right answer.
            Uint64 seaStateVersion = 0;
            bool valid = false;
            std::array<std::vector<glm::vec3>, SampledCascades> displacement;
        };

        // Rebuild h0 for a spectrum. Cached frames stay valid across this: each was built with the
        // h0 that belonged to its own tick, which is still the right answer for that tick.
        void rebuildSpectrum(const SpectrumParams& spectrum);
        // Cached field for a tick, computing it if the slot holds something else.
        const Frame& frameFor(Uint64 tick);
        void evaluate(Uint64 tick, Frame& outFrame);

        // Bilinear sample of one frame's summed cascades at an undisplaced grid position.
        [[nodiscard]] glm::vec3 displacementAt(const Frame& frame, glm::vec2 gridXZ) const;
        // Invert x + D_xz(x) = worldXZ, so a query in world space finds the grid point whose
        // displaced position lands there.
        [[nodiscard]] glm::vec2 inverseDisplace(const Frame& frame, glm::vec2 worldXZ) const;

        OceanSettings settings;
        SeaStateTrack seaState;

        // The spectrum h0 currently holds, after the sea state for whichever tick built it. A tick
        // whose weather differs from this one rebuilds; the stepping in SeaStateTrack is what keeps
        // that from happening every tick while weather is changing.
        SpectrumParams builtSpectrum{};
        bool spectrumValid = false;

        std::array<Cascade, SampledCascades> cascades;
        std::array<Frame, HistoryTicks> history;

        // Kept as members so a per-tick evaluation reuses the allocations instead of churning
        // them. The two slots are the packed complex fields the transform runs on; rowScratch is
        // the column gather inside it.
        std::vector<std::complex<float>> slotAField;
        std::vector<std::complex<float>> slotBField;
        std::vector<std::complex<float>> rowScratch;
    };
} // ytail::ocean

#endif //YELLOWTAIL_OCEANSIMULATION_H
