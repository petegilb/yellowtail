//
// Everything an ocean is, in one struct: the physical spectrum, the art direction laid over it,
// and the clipmap the surface is drawn on.
//
// OceanComponent serializes this, the editor edits it, OceanRenderer draws from it and
// OceanSimulation samples from it, so all four agree by construction rather than by convention.
//

#ifndef YELLOWTAIL_OCEANSETTINGS_H
#define YELLOWTAIL_OCEANSETTINGS_H

#include <array>

#include <SDL3/SDL_stdinc.h>
#include <glm/vec3.hpp>

#include "OceanSpectrum.h"

namespace ytail::ocean {
    struct OceanSettings {
        SpectrumParams spectrum;
        std::array<CascadeParams, CascadeCount> cascades = defaultCascades();

        // Shifts every gaussian in the initial spectrum, so a new seed is a different ocean with
        // the same weather. Part of the saved scene: peers must agree on it.
        Uint32 randomSeed = 1337;

        // --- displacement and foam ---

        // Horizontal displacement scale. Above about 1.5 the surface starts folding through itself
        // everywhere and the whole thing turns to foam.
        float choppiness = 1.0f;
        // Jacobian determinant below which the surface counts as folding. Flat water sits at 1 and
        // a genuinely breaking crest passes through 0, so this belongs near zero: raising it much
        // puts foam on every wave face rather than on the ones that are actually breaking.
        float foamThreshold = 0.1f;
        // Per second. Decay is exponential, growth is linear while the fold lasts.
        float foamDecayRate = 0.4f;
        float foamGrowRate = 1.0f;
        // How opaque the accumulated foam draws.
        float foamStrength = 1.0f;

        // --- shading ---

        glm::vec3 shallowColor{0.06f, 0.36f, 0.42f};
        glm::vec3 deepColor{0.004f, 0.03f, 0.07f};
        // Light scattering up through a wave, per metre of crest height above the mean surface.
        // 0.5 means a two metre crest glows fully. The thing that makes a wave read as water with
        // depth behind it rather than as a painted surface.
        float subsurfaceStrength = 0.5f;
        // Schlick exponent. 5 is physical; lower spreads the sky reflection further down the wave.
        float fresnelPower = 5.0f;
        // Sun glint tightness. Small values give a hard sparkle, large ones a broad sheen.
        float roughness = 0.08f;

        // --- clipmap ---

        // Quad size of the finest ring, metres. Halving it doubles the near detail and halves the
        // radius the same ring count reaches.
        float baseCellSize = 0.5f;
        // Quads across one ring level. Must be even: each ring's hole is exactly half of it.
        int gridSize = 128;
        // Ring levels, each double the cell size of the last. The outermost reaches
        // baseCellSize * gridSize/2 * 2^(ringCount-1) metres.
        int ringCount = 8;
        // Where in a ring the morph toward the coarser level starts, as a fraction of the ring's
        // half-extent. It always finishes at 1.0, which is what keeps the seam watertight.
        float morphStart = 0.85f;
        // Beyond this a cascade stops contributing. The fine cascades are below a pixel long
        // before the horizon, and skipping them is most of what makes the outer rings cheap.
        std::array<float, CascadeCount> cascadeFadeDistance{ 1.0e9f, 900.0f, 180.0f };

        // Mean sea level, world Y.
        float seaLevel = 0.0f;

        // --- sea state endpoints ---
        // What a scheduled intensity of 0 and 1 mean. The spectrum's own windSpeed and fetch are
        // what a scene without any scheduled weather uses; these only come into play once a
        // SeaStateTrack has keyframes on it.
        float calmWindSpeed = 5.0f;
        float stormWindSpeed = 22.0f;
        float calmFetch = 20000.0f;
        float stormFetch = 400000.0f;
    };
} // ytail::ocean

#endif //YELLOWTAIL_OCEANSETTINGS_H
