//
// Scheduled weather: how rough the sea is, as a function of the shared tick.
//
// The whole point is that nothing about the wave field goes over the wire. Peers hold the same
// short list of "at tick T the sea becomes this rough", evaluate it against the tick timeline they
// already share, and derive identical spectra from it. A storm is a handful of bytes.
//

#ifndef YELLOWTAIL_SEASTATE_H
#define YELLOWTAIL_SEASTATE_H

#include <vector>

#include <SDL3/SDL_stdinc.h>

#include "OceanSpectrum.h"

namespace ytail::ocean {
    struct OceanSettings;

    // 0 is glass, 1 is a storm. Between keyframes the value ramps linearly.
    struct SeaStateKeyframe {
        Uint64 tick = 0;
        float intensity = 0.5f;
    };

    class SeaStateTrack {
    public:
        // Enough to describe a storm rolling in and back out again with room to spare. Bounded
        // because the whole list is resent on every change and has to stay inside one packet.
        static constexpr int MaxKeyframes = 8;

        // Quantization steps across the 0..1 range. A change of intensity means a new h0, and
        // rebuilding h0 costs a pass over every spectrum bin, so a value that moved every single
        // tick would rebuild every single tick. Stepping it means the spectrum holds still between
        // steps, which is also what keeps the ramp cheap during a network rollback.
        static constexpr int Steps = 64;

        void clear() { frames.clear(); ++version; }
        [[nodiscard]] bool empty() const { return frames.empty(); }
        [[nodiscard]] const std::vector<SeaStateKeyframe>& getKeyframes() const { return frames; }

        // Bumped by every mutation. A schedule arriving from the host rewrites the weather of ticks
        // already simulated, so anything caching a result per tick has to notice and recompute
        // rather than trusting a tick number alone.
        [[nodiscard]] Uint64 getVersion() const { return version; }

        // Schedule an intensity for a tick, keeping the list sorted by tick. A second keyframe on
        // the same tick replaces the first. Past MaxKeyframes the oldest goes, since a keyframe
        // already in the past only matters as the start of the ramp we are inside.
        void schedule(Uint64 tick, float intensity);
        void setKeyframes(const std::vector<SeaStateKeyframe>& newFrames);

        // Quantized intensity at a tick: held flat before the first keyframe and after the last,
        // linear between. Pure in the tick, which is what lets a rollback replay reproduce the
        // weather it originally ran under.
        [[nodiscard]] float evaluate(Uint64 tick) const;

    private:
        std::vector<SeaStateKeyframe> frames;
        Uint64 version = 0;
    };

    // Fold an intensity into a spectrum, by moving the two parameters JONSWAP is really
    // parameterized by. Wind speed and fetch rise together, because a storm is not just harder
    // wind but wind that has been blowing across more water.
    void applySeaState(SpectrumParams& spectrum, float intensity, const OceanSettings& settings);
} // ytail::ocean

#endif //YELLOWTAIL_SEASTATE_H
