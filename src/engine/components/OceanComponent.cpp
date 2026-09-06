//
// Created by Peter Gilbert on 8/14/26.
//

#include "OceanComponent.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "imgui.h"

#include "../serialize/Archive.h"
#include "../serialize/GlmJson.h"

namespace ytail {
    void OceanComponent::serialize(Archive& ar) {
        ocean::SpectrumParams& spectrum = settings.spectrum;
        ar("windSpeed", spectrum.windSpeed);
        ar("windDirection", spectrum.windDirection);
        ar("fetch", spectrum.fetch);
        ar("peakEnhancement", spectrum.peakEnhancement);
        ar("spreadBlend", spectrum.spreadBlend);
        ar("swell", spectrum.swell);
        ar("depth", spectrum.depth);
        ar("shortWavesFade", spectrum.shortWavesFade);
        ar("amplitude", spectrum.amplitude);
        ar("loopPeriod", spectrum.loopPeriod);
        ar("timeScale", spectrum.timeScale);

        // Patch sizes are saved but the band edges are not: they follow from the patch sizes, and
        // storing a derived value is how the two end up disagreeing after an edit.
        for (int i = 0; i < ocean::CascadeCount; ++i) {
            const std::string key = "cascadePatchSize" + std::to_string(i);
            ar(key.c_str(), settings.cascades[i].patchSize);
            ar(("cascadeFadeDistance" + std::to_string(i)).c_str(), settings.cascadeFadeDistance[i]);
        }
        if (ar.reading()) rebuildCascadeBands();

        ar("randomSeed", settings.randomSeed);
        ar("choppiness", settings.choppiness);
        ar("foamThreshold", settings.foamThreshold);
        ar("foamDecayRate", settings.foamDecayRate);
        ar("foamGrowRate", settings.foamGrowRate);
        ar("foamStrength", settings.foamStrength);

        ar("shallowColor", settings.shallowColor);
        ar("deepColor", settings.deepColor);
        ar("subsurfaceStrength", settings.subsurfaceStrength);
        ar("fresnelPower", settings.fresnelPower);
        ar("roughness", settings.roughness);

        ar("baseCellSize", settings.baseCellSize);
        ar("gridSize", settings.gridSize);
        ar("ringCount", settings.ringCount);
        ar("morphStart", settings.morphStart);
        ar("seaLevel", settings.seaLevel);

        ar("calmWindSpeed", settings.calmWindSpeed);
        ar("stormWindSpeed", settings.stormWindSpeed);
        ar("calmFetch", settings.calmFetch);
        ar("stormFetch", settings.stormFetch);
    }

    void OceanComponent::rebuildCascadeBands() {
        // Each band starts where the previous one stopped, six bins inside the finer tile's
        // Nyquist limit, so no wavelength is generated twice and the cascades sum without
        // doubling the sea's energy.
        constexpr float twoPi = 6.28318530717959f;
        constexpr float boundaryBins = 6.0f;

        for (int i = 0; i < ocean::CascadeCount; ++i) {
            const float patchSize = std::max(settings.cascades[i].patchSize, 0.01f);
            settings.cascades[i].patchSize = patchSize;
            settings.cascades[i].cutoffLow = i == 0 ? 0.0f : boundaryBins * twoPi / patchSize;
            settings.cascades[i].cutoffHigh = i == ocean::CascadeCount - 1
                ? 1e9f
                : boundaryBins * twoPi / std::max(settings.cascades[i + 1].patchSize, 0.01f);
        }
    }

    void OceanComponent::drawInspector() {
        ocean::SpectrumParams& spectrum = settings.spectrum;

        if (ImGui::CollapsingHeader("Spectrum", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::DragFloat("Wind Speed", &spectrum.windSpeed, 0.1f, 0.1f, 40.0f, "%.1f m/s");
            ImGui::SliderAngle("Wind Direction", &spectrum.windDirection);
            ImGui::DragFloat("Fetch", &spectrum.fetch, 1000.0f, 1000.0f, 1000000.0f, "%.0f m");
            ImGui::DragFloat("Peak Enhancement", &spectrum.peakEnhancement, 0.05f, 1.0f, 10.0f);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("JONSWAP gamma. 1 is a fully developed sea, 3.3 the JONSWAP "
                                  "mean, higher a sharper swell.");
            }
            ImGui::SliderFloat("Spread Blend", &spectrum.spreadBlend, 0.0f, 1.0f);
            ImGui::SliderFloat("Swell", &spectrum.swell, 0.0f, 1.0f);
            ImGui::DragFloat("Depth", &spectrum.depth, 1.0f, 1.0f, 2000.0f, "%.0f m");
            ImGui::DragFloat("Short Wave Fade", &spectrum.shortWavesFade, 0.001f, 0.0f, 0.5f, "%.4f");
            ImGui::DragFloat("Amplitude", &spectrum.amplitude, 0.05f, 0.0f, 20.0f);
            ImGui::DragFloat("Loop Period", &spectrum.loopPeriod, 1.0f, 10.0f, 3600.0f, "%.0f s");
            ImGui::DragFloat("Time Scale", &spectrum.timeScale, 0.01f, 0.05f, 4.0f);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("1 is physically correct. A sea that looks too busy is usually "
                                  "carrying too much short wave energy, so try Short Wave Fade and "
                                  "the patch sizes before slowing the clock down.");
            }

            // The single most useful readout when tuning: it says how big and how slow the sea
            // actually is, which the wind and fetch sliders only imply.
            const float peak = ocean::peakOmega(spectrum);
            const float peakPeriod = 6.28318531f / std::max(peak, 1e-4f) / std::max(spectrum.timeScale, 1e-4f);
            const float peakWavelength = 9.81f * 6.28318531f / std::max(peak * peak, 1e-6f);
            ImGui::TextDisabled("peak wave: %.0f m, %.1f s", static_cast<double>(peakWavelength),
                                static_cast<double>(peakPeriod));
            if (peakWavelength > settings.cascades[0].patchSize * 0.25f) {
                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                                   "Peak is large for cascade 0's %.0fm patch; few bins carry the "
                                   "swell and it will look repetitive.",
                                   static_cast<double>(settings.cascades[0].patchSize));
            }

            int seed = static_cast<int>(settings.randomSeed);
            if (ImGui::InputInt("Random Seed", &seed)) {
                settings.randomSeed = static_cast<Uint32>(std::max(seed, 0));
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("A different ocean in the same weather. Every peer must agree "
                                  "on it, so it is saved with the scene.");
            }
        }

        if (ImGui::CollapsingHeader("Cascades")) {
            bool patchSizeChanged = false;
            for (int i = 0; i < ocean::CascadeCount; ++i) {
                ImGui::PushID(i);
                ImGui::Text("Cascade %d", i);
                patchSizeChanged |= ImGui::DragFloat("Patch Size", &settings.cascades[i].patchSize,
                                                     0.5f, 1.0f, 2000.0f, "%.1f m");
                ImGui::DragFloat("Fade Distance", &settings.cascadeFadeDistance[i],
                                 5.0f, 10.0f, 1.0e9f, "%.0f m");
                ImGui::TextDisabled("band %.3f .. %.3f rad/m",
                                    static_cast<double>(settings.cascades[i].cutoffLow),
                                    static_cast<double>(settings.cascades[i].cutoffHigh));
                ImGui::PopID();
            }
            // The bands are derived, so an edit to a patch size has to reach them straight away or
            // the next frame simulates a spectrum with a hole or an overlap in it.
            if (patchSizeChanged) rebuildCascadeBands();
        }

        if (ImGui::CollapsingHeader("Displacement and Foam")) {
            ImGui::DragFloat("Choppiness", &settings.choppiness, 0.01f, 0.0f, 2.0f);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Past about 1.5 the surface folds through itself everywhere and "
                                  "the whole sea turns to foam.");
            }
            ImGui::DragFloat("Foam Threshold", &settings.foamThreshold, 0.01f, -1.0f, 2.0f);
            ImGui::DragFloat("Foam Decay Rate", &settings.foamDecayRate, 0.01f, 0.0f, 10.0f);
            ImGui::DragFloat("Foam Grow Rate", &settings.foamGrowRate, 0.05f, 0.0f, 50.0f);
            ImGui::DragFloat("Foam Strength", &settings.foamStrength, 0.01f, 0.0f, 5.0f);
        }

        if (ImGui::CollapsingHeader("Shading")) {
            ImGui::ColorEdit3("Shallow Color", &settings.shallowColor.x);
            ImGui::ColorEdit3("Deep Color", &settings.deepColor.x);
            ImGui::DragFloat("Subsurface", &settings.subsurfaceStrength, 0.05f, 0.0f, 20.0f);
            ImGui::DragFloat("Fresnel Power", &settings.fresnelPower, 0.05f, 1.0f, 10.0f);
            ImGui::DragFloat("Roughness", &settings.roughness, 0.005f, 0.005f, 1.0f);
        }

        if (ImGui::CollapsingHeader("Sea State")) {
            ImGui::TextDisabled("What a scheduled intensity of 0 and 1 mean. Only used once "
                                "weather has been scheduled against the tick timeline.");
            ImGui::DragFloat("Calm Wind", &settings.calmWindSpeed, 0.1f, 0.1f, 40.0f, "%.1f m/s");
            ImGui::DragFloat("Storm Wind", &settings.stormWindSpeed, 0.1f, 0.1f, 40.0f, "%.1f m/s");
            ImGui::DragFloat("Calm Fetch", &settings.calmFetch, 1000.0f, 1000.0f, 1000000.0f, "%.0f m");
            ImGui::DragFloat("Storm Fetch", &settings.stormFetch, 1000.0f, 1000.0f, 1000000.0f, "%.0f m");
        }

        if (ImGui::CollapsingHeader("Clipmap")) {
            // Every one of these rebuilds the mesh, which is why they sit apart from the rest.
            ImGui::DragFloat("Base Cell Size", &settings.baseCellSize, 0.05f, 0.05f, 10.0f, "%.2f m");
            ImGui::DragInt("Grid Size", &settings.gridSize, 2.0f, 4, 512);
            ImGui::DragInt("Ring Count", &settings.ringCount, 0.1f, 1, 12);
            ImGui::SliderFloat("Morph Start", &settings.morphStart, 0.0f, 0.99f);
            ImGui::DragFloat("Sea Level", &settings.seaLevel, 0.05f, -100.0f, 100.0f, "%.2f m");

            const float radius = settings.baseCellSize * static_cast<float>(settings.gridSize / 2)
                               * static_cast<float>(1 << std::max(settings.ringCount - 1, 0));
            ImGui::TextDisabled("reaches %.0f m", static_cast<double>(radius));
        }
    }
} // ytail
