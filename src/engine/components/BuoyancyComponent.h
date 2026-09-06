//
// Created by Peter Gilbert on 8/14/26.
//

#ifndef YELLOWTAIL_BUOYANCYCOMPONENT_H
#define YELLOWTAIL_BUOYANCYCOMPONENT_H

#include <vector>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include "../Component.h"
#include "../ocean/OceanSimulation.h"

namespace ytail {
    class RigidbodyComponent;

    // Floats a sibling rigidbody on the ocean by sampling the surface at a few points spread over
    // the body and pushing up at each one. Sampling several points rather than one is what makes a
    // hull pitch and roll with the wave instead of sliding along it flat.
    //
    // Runs in fixedPreTick, so the forces it applies are consumed by the physics step that
    // follows. Every sample is taken for the tick being simulated rather than the tick we have
    // really reached, so a rollback replay feels the water as it stood at the tick it is replaying.
    class BuoyancyComponent : public Component {
    public:
        void fixedPreTick(float deltaTime) override;

        // Probes per axis over the body's footprint. 2 gives four corners, which is the minimum
        // that produces righting torque; 3 or 4 suits a long hull. Cost is the square of this.
        int probesPerAxis = 2;
        // How much of the body's own extents the probe grid spans. Below 1 pulls the probes in
        // from the hull's edges, which softens the roll.
        float probeSpread = 0.8f;

        // Vertical span over which a probe goes from dry to fully displaced, centred on the probe:
        // half of it reaches above the probe and half below. Zero derives it from the collider's
        // height, which is nearly always what you want, since that is exactly the column of body
        // the probe stands for.
        float submersionDepth = 0.0f;
        // Effectively the ratio of the water's density to the body's, so this is the knob that
        // decides whether something floats at all.
        //
        // Buoyancy is the weight of water the colliders displace, so a body only floats when it
        // is less dense than water. Jolt's default density is 1000 kg/m^3, exactly water's, so a
        // body left on the defaults is neutrally buoyant and will hang at whatever depth it
        // reaches rather than rise. Either give the rigidbody an overrideMass below its volume in
        // tonnes, or raise this.
        float buoyancy = 1.6f;

        // Drag against the water, per probe, split because a hull resists sideways motion far more
        // than it resists moving along itself.
        float verticalDrag = 2.0f;
        float horizontalDrag = 0.6f;
        // Angular drag, applied to the whole body rather than per probe.
        float angularDrag = 1.5f;

        // How hard the water's own horizontal motion pushes the body. This is what makes a hull
        // surf down a wave face rather than bobbing in place, and it is the single biggest
        // contributor to the ocean feeling alive.
        float wavePush = 1.0f;

        // Skip everything while the body is clear of the water, so a ship on a slipway or a crate
        // in the air costs nothing.
        [[nodiscard]] bool isSubmerged() const { return submergedProbes > 0; }
        // Probes that found themselves under the surface last tick, for the inspector readout.
        [[nodiscard]] int getSubmergedProbes() const { return submergedProbes; }

        static constexpr const char* SerialId = "buoyancy";
        void serialize(Archive& ar) override;
        [[nodiscard]] const char* serialId() const override { return SerialId; }

        [[nodiscard]] const char* getTypeName() const override { return "Buoyancy"; }
        void drawInspector() override;

    private:
        // Body-local probe positions, rebuilt when the probe layout or the collider changes.
        void rebuildProbes(const RigidbodyComponent& body);

        std::vector<glm::vec3> localProbes;
        // Geometric volume of the colliders, cubic metres. What the buoyant force is the weight
        // of, so it is measured rather than derived from the mass.
        float displacedVolume = 0.0f;
        // submersionDepth, or the collider's height when that is zero.
        float derivedSubmersionDepth = 1.0f;

        // What rebuildProbes last built for, so it does not run every tick.
        int builtProbesPerAxis = 0;
        float builtProbeSpread = -1.0f;
        float builtSubmersionDepth = -1.0f;
        glm::vec3 builtHalfExtents{0.0f};

        // Scratch reused across ticks so a hull's probes do not allocate 60 times a second.
        std::vector<glm::vec3> worldProbes;
        std::vector<glm::vec2> probeColumns;
        std::vector<ocean::OceanSample> probeSamples;

        int submergedProbes = 0;
    };
} // ytail

#endif //YELLOWTAIL_BUOYANCYCOMPONENT_H
