//
// Created by Peter Gilbert on 8/14/26.
//

#include "BuoyancyComponent.h"

#include <algorithm>
#include <cmath>

#include "imgui.h"
#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>

#include "RigidbodyComponent.h"
#include "TransformComponent.h"
#include "../GameplayStatics.h"
#include "../Profiling.h"
#include "../serialize/Archive.h"

namespace ytail {
    namespace {
        // Fresh water at 1000 kg/m^3. Sea water is nearer 1025, but the difference is inside what
        // the buoyancy multiplier is for.
        constexpr float kWaterDensity = 1000.0f;
        constexpr float kGravity = 9.81f;

        // Half-extents of the box that contains every collider on the body, in body space. Used
        // only to spread the probes over something the right shape; the physics still solves
        // against the real colliders.
        glm::vec3 colliderHalfExtents(const RigidbodyComponent& body) {
            glm::vec3 halfExtents{0.0f};
            for (const physics::ColliderDef& collider : body.colliders) {
                glm::vec3 reach{0.0f};
                switch (collider.shape) {
                    case physics::ColliderShape::Box:
                        reach = collider.halfExtents;
                        break;
                    case physics::ColliderShape::Sphere:
                        reach = glm::vec3(collider.radius);
                        break;
                    case physics::ColliderShape::Capsule:
                        reach = glm::vec3(collider.radius,
                                          collider.halfHeight + collider.radius,
                                          collider.radius);
                        break;
                }
                halfExtents = glm::max(halfExtents, glm::abs(collider.offset) + reach);
            }
            return glm::max(halfExtents, glm::vec3(0.01f));
        }

        // Volume of the body's colliders, cubic metres. Overlapping shapes in a compound are
        // counted twice, which errs toward more buoyant and is inside what the multiplier covers.
        //
        // This has to be the real geometric volume. Deriving it from the mass instead gives the
        // body exactly the density of water by construction, and a body with water's density does
        // not float, it hangs wherever it is put.
        float colliderVolume(const RigidbodyComponent& body) {
            constexpr float pi = 3.14159265358979323846f;
            float volume = 0.0f;
            for (const physics::ColliderDef& collider : body.colliders) {
                switch (collider.shape) {
                    case physics::ColliderShape::Box:
                        volume += 8.0f * collider.halfExtents.x * collider.halfExtents.y
                                * collider.halfExtents.z;
                        break;
                    case physics::ColliderShape::Sphere:
                        volume += 4.0f / 3.0f * pi * collider.radius * collider.radius * collider.radius;
                        break;
                    case physics::ColliderShape::Capsule:
                        // Cylindrical middle plus the two hemispherical caps.
                        volume += pi * collider.radius * collider.radius * 2.0f * collider.halfHeight
                                + 4.0f / 3.0f * pi * collider.radius * collider.radius * collider.radius;
                        break;
                }
            }
            return volume;
        }
    } // namespace

    void BuoyancyComponent::rebuildProbes(const RigidbodyComponent& body) {
        const glm::vec3 halfExtents = colliderHalfExtents(body);
        const int perAxis = std::clamp(probesPerAxis, 1, 8);

        localProbes.clear();
        localProbes.reserve(static_cast<size_t>(perAxis) * perAxis);

        // A grid over the waterplane footprint, at the body's vertical centre. Spreading in x and
        // z is what produces pitch and roll; a second row in y would only duplicate the same
        // column of water.
        for (int zStep = 0; zStep < perAxis; ++zStep) {
            for (int xStep = 0; xStep < perAxis; ++xStep) {
                // A single probe belongs in the middle, not in a corner.
                const float u = perAxis == 1 ? 0.0f
                    : (static_cast<float>(xStep) / static_cast<float>(perAxis - 1)) * 2.0f - 1.0f;
                const float v = perAxis == 1 ? 0.0f
                    : (static_cast<float>(zStep) / static_cast<float>(perAxis - 1)) * 2.0f - 1.0f;

                localProbes.emplace_back(u * halfExtents.x * probeSpread, 0.0f,
                                         v * halfExtents.z * probeSpread);
            }
        }

        worldProbes.resize(localProbes.size());
        probeColumns.resize(localProbes.size());
        probeSamples.resize(localProbes.size());

        displacedVolume = colliderVolume(body);
        // Zero means "as deep as the body is tall", which is the draught of something that floats
        // about half out of the water and is a far better guess than any fixed number.
        derivedSubmersionDepth = submersionDepth > 0.0f ? submersionDepth : halfExtents.y * 2.0f;

        builtProbesPerAxis = perAxis;
        builtProbeSpread = probeSpread;
        builtSubmersionDepth = submersionDepth;
        builtHalfExtents = halfExtents;
    }

    void BuoyancyComponent::fixedPreTick(float deltaTime) {
        ZoneScoped;
        submergedProbes = 0;

        auto* body = getSibling<RigidbodyComponent>();
        const auto* transform = getSibling<TransformComponent>();
        if (body == nullptr || transform == nullptr) return;
        if (body->type != physics::BodyType::Dynamic) return;

        // Null when the scene has no ocean, which means there is no water here to float on.
        // Not named `ocean`: that would shadow the namespace the sample type comes from.
        ocean::OceanSimulation* oceanSimulation = GameplayStatics::getOcean();
        if (oceanSimulation == nullptr) return;

        const float mass = body->getMass();
        if (mass <= 0.0f) return;

        if (localProbes.empty() || builtProbesPerAxis != std::clamp(probesPerAxis, 1, 8)
            || builtProbeSpread != probeSpread || builtSubmersionDepth != submersionDepth
            || builtHalfExtents != colliderHalfExtents(*body)) {
            rebuildProbes(*body);
        }
        if (localProbes.empty() || displacedVolume <= 0.0f) return;

        // The tick being simulated, which a rollback replay sets to the tick it is replaying. Every
        // sample below is taken for that tick, so a replayed hull feels the water it originally
        // felt rather than the water of the present.
        const Uint64 tick = GameplayStatics::getTickNumber();

        const glm::mat4& world = transform->worldMatrix();
        const glm::vec3 bodyPosition{ world[3] };

        for (size_t i = 0; i < localProbes.size(); ++i) {
            worldProbes[i] = glm::vec3(world * glm::vec4(localProbes[i], 1.0f));
            probeColumns[i] = glm::vec2(worldProbes[i].x, worldProbes[i].z);
        }

        // One batched query for the whole hull: the cache lookup and the two frames it resolves
        // are per call, not per probe.
        oceanSimulation->sampleBatch(probeColumns.data(), probeColumns.size(), tick, probeSamples.data());

        // Each probe carries an equal share of the body, so the forces sum back to the whole
        // regardless of how many probes there are. Changing the probe count then changes how
        // finely the hull follows the wave, not how hard it floats.
        const float shareOfBody = 1.0f / static_cast<float>(localProbes.size());
        const glm::vec3 bodyVelocity = body->getLinearVelocity();
        const glm::vec3 bodyAngularVelocity = body->getAngularVelocity();

        for (size_t i = 0; i < localProbes.size(); ++i) {
            const ocean::OceanSample& sample = probeSamples[i];
            const float depth = sample.height - worldProbes[i].y;

            // The ramp straddles the probe rather than starting at it, because the probe sits at
            // the middle of the column of body it stands for, not at its underside. Half a span
            // above the water the column is dry, level with the water it is half displaced, half
            // a span below it is fully under.
            //
            // Measuring downward from the probe instead makes the body half submerged before it
            // feels anything at all, and then no amount of buoyancy can lift it further than that:
            // equilibrium needs submerged = density / (water * buoyancy), which only reaches the
            // waterline as buoyancy goes to infinity. It floats, fully underwater, forever.
            const float span = std::max(derivedSubmersionDepth, 0.01f);
            const float submerged = std::clamp(depth / span + 0.5f, 0.0f, 1.0f);
            if (submerged <= 0.0f) continue;
            ++submergedProbes;

            const float buoyantForce = kWaterDensity * kGravity * displacedVolume
                                     * shareOfBody * submerged * buoyancy;
            body->addForceAtPosition(glm::vec3(0.0f, buoyantForce, 0.0f), worldProbes[i]);

            // Velocity of this point of the hull, including the spin, so a rolling ship resists
            // rolling rather than only translating.
            const glm::vec3 leverArm = worldProbes[i] - bodyPosition;
            const glm::vec3 probeVelocity = bodyVelocity + glm::cross(bodyAngularVelocity, leverArm);
            const glm::vec3 relative = probeVelocity - sample.velocity;

            // Drag against the water, not against the world: a hull carried along by a wave feels
            // none, which is what lets it be carried at all.
            const glm::vec3 drag{ -relative.x * horizontalDrag * mass * shareOfBody * submerged,
                                  -relative.y * verticalDrag * mass * shareOfBody * submerged,
                                  -relative.z * horizontalDrag * mass * shareOfBody * submerged };
            body->addForceAtPosition(drag, worldProbes[i]);

            // The push down the wave face. The water's own horizontal motion, applied as a force
            // rather than folded into the drag, so it can be tuned without changing how sharply
            // the hull settles.
            const glm::vec3 push{ sample.velocity.x * wavePush * mass * shareOfBody * submerged,
                                  0.0f,
                                  sample.velocity.z * wavePush * mass * shareOfBody * submerged };
            body->addForceAtPosition(push, worldProbes[i]);
        }

        if (submergedProbes > 0) {
            // Whole-body angular drag, scaled by how much of the hull is actually in the water.
            const float wetFraction = static_cast<float>(submergedProbes)
                                    / static_cast<float>(localProbes.size());
            body->addTorque(-bodyAngularVelocity * angularDrag * mass * wetFraction);
        }
    }

    void BuoyancyComponent::serialize(Archive& ar) {
        ar("probesPerAxis", probesPerAxis);
        ar("probeSpread", probeSpread);
        ar("submersionDepth", submersionDepth);
        ar("buoyancy", buoyancy);
        ar("verticalDrag", verticalDrag);
        ar("horizontalDrag", horizontalDrag);
        ar("angularDrag", angularDrag);
        ar("wavePush", wavePush);
    }

    void BuoyancyComponent::drawInspector() {
        if (getSibling<RigidbodyComponent>() == nullptr) {
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "Needs a Rigidbody on this entity");
        }

        ImGui::DragInt("Probes Per Axis", &probesPerAxis, 0.1f, 1, 8);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Cost is the square of this. 2 is the minimum that gives righting "
                              "torque; 3 or 4 suits a long hull.");
        }
        ImGui::SliderFloat("Probe Spread", &probeSpread, 0.1f, 1.0f);
        ImGui::DragFloat("Submersion Span", &submersionDepth, 0.05f, 0.0f, 20.0f, "%.2f m");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Dry to fully displaced, centred on the probe. 0 derives it from the "
                              "collider's height.");
        }
        // Not just "Buoyancy": the component's own collapsing header is already called that, and a
        // header does not push an ID scope, so the two would collide.
        ImGui::DragFloat("Buoyancy Ratio", &buoyancy, 0.01f, 0.0f, 5.0f);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Water's density over the body's. Above 1 floats, below 1 sinks.");
        }
        ImGui::DragFloat("Vertical Drag", &verticalDrag, 0.05f, 0.0f, 20.0f);
        ImGui::DragFloat("Horizontal Drag", &horizontalDrag, 0.05f, 0.0f, 20.0f);
        ImGui::DragFloat("Angular Drag", &angularDrag, 0.05f, 0.0f, 20.0f);
        ImGui::DragFloat("Wave Push", &wavePush, 0.05f, 0.0f, 10.0f);

        ImGui::TextDisabled("%d / %d probes submerged", submergedProbes,
                            static_cast<int>(localProbes.size()));

        // Whether this body floats is one division, and it is the question everyone actually has
        // when it sinks. Answer it here rather than making them work it out from the sliders.
        if (const auto* body = getSibling<RigidbodyComponent>()) {
            const float mass = body->getMass();
            if (mass > 0.0f && displacedVolume > 0.0f) {
                const float density = mass / displacedVolume;
                const float floats = 1000.0f * buoyancy / density;
                ImGui::TextDisabled("%.0f kg over %.2f m3 = %.0f kg/m3", static_cast<double>(mass),
                                    static_cast<double>(displacedVolume),
                                    static_cast<double>(density));
                if (floats < 1.02f) {
                    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                                       "Too dense to float: raise Buoyancy Ratio above %.2f, or "
                                       "lower the rigidbody's mass.",
                                       static_cast<double>(density / 1000.0f));
                } else {
                    ImGui::TextDisabled("floats, riding %.0f%% submerged",
                                        static_cast<double>(100.0f / floats));
                }
            }
        }
    }
} // ytail
