//
// Created by Peter Gilbert on 8/14/26.
//

#include "SeaState.h"

#include <algorithm>
#include <cmath>

#include "OceanSettings.h"

namespace ytail::ocean {
    namespace {
        // A keyframe already in the past matters only as the start of the ramp we are inside, so
        // the oldest is the one to give up when the list is full.
        void trimOldest(std::vector<SeaStateKeyframe>& frames) {
            while (frames.size() > static_cast<size_t>(SeaStateTrack::MaxKeyframes)) {
                frames.erase(frames.begin());
            }
        }
    } // namespace

    void SeaStateTrack::schedule(const Uint64 tick, const float intensity) {
        const float clamped = std::clamp(intensity, 0.0f, 1.0f);
        ++version;

        const auto existing = std::ranges::find_if(frames, [tick](const SeaStateKeyframe& frame) {
            return frame.tick == tick;
        });
        if (existing != frames.end()) {
            existing->intensity = clamped;
            return;
        }

        frames.push_back({ tick, clamped });
        std::ranges::sort(frames, {}, &SeaStateKeyframe::tick);
        trimOldest(frames);
    }

    void SeaStateTrack::setKeyframes(const std::vector<SeaStateKeyframe>& newFrames) {
        ++version;
        frames = newFrames;
        std::ranges::sort(frames, {}, &SeaStateKeyframe::tick);
        trimOldest(frames);
    }

    float SeaStateTrack::evaluate(const Uint64 tick) const {
        if (frames.empty()) return 0.0f;

        float raw;
        if (tick <= frames.front().tick) {
            raw = frames.front().intensity;
        } else if (tick >= frames.back().tick) {
            raw = frames.back().intensity;
        } else {
            // Short list, so a scan beats anything cleverer and stays obviously correct.
            raw = frames.back().intensity;
            for (size_t i = 1; i < frames.size(); ++i) {
                if (tick > frames[i].tick) continue;

                const SeaStateKeyframe& from = frames[i - 1];
                const SeaStateKeyframe& to = frames[i];
                const auto span = static_cast<double>(to.tick - from.tick);
                const double progress = span > 0.0
                    ? static_cast<double>(tick - from.tick) / span
                    : 1.0;
                raw = from.intensity
                    + (to.intensity - from.intensity) * static_cast<float>(progress);
                break;
            }
        }

        // Stepped, not continuous: see the note on Steps. Rounding rather than flooring keeps the
        // endpoints exact, so a keyframe at 1.0 really does reach 1.0.
        return std::round(std::clamp(raw, 0.0f, 1.0f) * static_cast<float>(Steps))
             / static_cast<float>(Steps);
    }

    void applySeaState(SpectrumParams& spectrum, const float intensity,
                       const OceanSettings& settings) {
        const float clamped = std::clamp(intensity, 0.0f, 1.0f);
        spectrum.windSpeed = settings.calmWindSpeed
                           + (settings.stormWindSpeed - settings.calmWindSpeed) * clamped;
        spectrum.fetch = settings.calmFetch
                       + (settings.stormFetch - settings.calmFetch) * clamped;
    }
} // ytail::ocean
