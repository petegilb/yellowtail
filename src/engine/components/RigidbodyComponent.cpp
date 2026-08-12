//
// Created by Peter Gilbert on 7/16/26.
//

#include "RigidbodyComponent.h"

#include <algorithm>
#include <cmath>

#include "imgui.h"

#include "TransformComponent.h"
#include "engine/World.h"
#include "engine/GameplayStatics.h"
#include "engine/serialize/Archive.h"
#include "engine/serialize/EnumJson.h"

namespace ytail {
    using namespace physics;

    // Below these a body counts as settled for replication, even if Jolt has not slept it yet.
    constexpr float RestLinearSpeed = 0.05f;
    constexpr float RestAngularSpeed = 0.1f;
    // How far a settled body may sit from an at-rest update before it is worth waking it to correct.
    constexpr float RestSnapEpsilon = 0.05f;
    // The rate the decay factors in VisualSmoothing were authored at, which has nothing to do with
    // the fixed tick: smoothing is cosmetic, so it follows the wall clock.
    constexpr float VisualErrorReferenceHz = 60.0f;
    // Live, for the same reason as the replication rates: this is judged by eye, under lag, and
    // rebuilding to try a number is the expensive part of this project. Sliders live in the
    // networking debug window.
    VisualSmoothing RigidbodyComponent::smoothing;

    void RigidbodyComponent::serialize(Archive& ar) {
        ar("colliders", colliders);
        ar("bodyType", type);
        ar("properties", properties);
    }

    RigidbodyComponent::~RigidbodyComponent() {
        if (body != InvalidBody) PhysicsManager::get().removeBody(body);
    }

    RigidbodyComponent::RigidbodyComponent(RigidbodyComponent&& other) noexcept
        : Component(std::move(other)),
          colliders(std::move(other.colliders)), type(other.type), properties(other.properties),
          body(other.body), bodyDirty(other.bodyDirty), propertiesDirty(other.propertiesDirty),
          visualPositionError(other.visualPositionError), visualRotationError(other.visualRotationError),
          previousPosition(other.previousPosition), previousRotation(other.previousRotation),
          currentPosition(other.currentPosition), currentRotation(other.currentRotation),
          poseCount(other.poseCount) {
        other.body = InvalidBody; // the old copy's destructor must not delete the live body
    }

    RigidbodyComponent& RigidbodyComponent::operator=(RigidbodyComponent&& other) noexcept {
        if (this == &other) return *this;
        if (body != InvalidBody) PhysicsManager::get().removeBody(body);
        Component::operator=(std::move(other));
        colliders = std::move(other.colliders);
        type = other.type;
        properties = other.properties;
        body = other.body;
        bodyDirty = other.bodyDirty;
        propertiesDirty = other.propertiesDirty;
        visualPositionError = other.visualPositionError;
        visualRotationError = other.visualRotationError;
        previousPosition = other.previousPosition;
        previousRotation = other.previousRotation;
        currentPosition = other.currentPosition;
        currentRotation = other.currentRotation;
        poseCount = other.poseCount;
        other.body = InvalidBody;
        return *this;
    }

    bool RigidbodyComponent::ensureBody(const TransformComponent* transform) {
        if (transform == nullptr) return false;

        if (bodyDirty && body != InvalidBody) {
            PhysicsManager::get().removeBody(body);
            body = InvalidBody;
            poseCount = 0;
        }
        bodyDirty = false;

        // Create the body from the transform's current pose.
        if (body == InvalidBody) {
            BodyDef def;
            def.colliders = colliders;
            def.position = transform->getPosition();
            def.rotation = transform->getRotation();
            def.type = type;
            def.properties = properties;
            body = PhysicsManager::get().createBody(def);
            propertiesDirty = false;
        } else if (propertiesDirty) {
            PhysicsManager::get().applyBodyProperties(body, properties);
            propertiesDirty = false;
        }
        return true;
    }

    // The body has to exist before the step, and before anything applies a force to it.
    void RigidbodyComponent::fixedPreTick(float) {
        ensureBody(getSibling<TransformComponent>());
    }

    void RigidbodyComponent::fixedTick(float deltaTime) {
        // Looked up each tick: cached component pointers go stale when pools change.
        TransformComponent* transform = getSibling<TransformComponent>();
        if (!ensureBody(transform)) return;

        // Moving bodies are authoritative: write the simulated pose back onto the entity. The sim
        // works in world space and this writes into the local position/rotation, so a moving body
        // is expected to be a root entity. Parenting one is unsupported (sim wins).
        if (type != BodyType::Static) {
            glm::vec3 position;
            glm::quat rotation;
            PhysicsManager::get().getBodyTransform(body, position, rotation);
            transform->setPosition(position);
            transform->setRotation(rotation);
            previousPosition = currentPosition;
            previousRotation = currentRotation;
            currentPosition = position;
            currentRotation = rotation;
            if (poseCount < 2) ++poseCount;
        }
    }

    // Ramped between the two rates across the range where an error goes from unnoticeable to
    // obvious, rather than stepped at a single threshold: a step shows up as the fade rate changing
    // abruptly as a body crosses it. reference: https://www.gafferongames.com/post/state_synchronization/
    static float decayFactor(const float error, const float small, const float large) {
        const VisualSmoothing& smoothing = RigidbodyComponent::smoothing;
        const float span = std::max(large - small, 1e-4f);
        const float t = std::clamp((error - small) / span, 0.0f, 1.0f);
        return glm::mix(smoothing.smallDecay, smoothing.largeDecay, t);
    }

    // tick is variable rate, so raise the per-frame factor by the number of reference frames this
    // one covers, or the smoothing runs faster on a faster display.
    void RigidbodyComponent::decayVisualError(const float deltaTime) {
        if (deltaTime <= 0.0f) return;

        const float positionError = glm::length(visualPositionError);
        // Its own magnitude, not the position's. A body a centimetre out of place but visibly out of
        // spin would otherwise take the gentle rate chosen for the position and hold that spin for
        // hundreds of milliseconds, which on anything with a surface is the more obvious error.
        const float angleError = 2.0f * std::acos(std::clamp(std::abs(visualRotationError.w), 0.0f, 1.0f));
        if (positionError < 1e-4f && angleError < 1e-4f) {
            visualPositionError = glm::vec3(0.0f);
            visualRotationError = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
            return;
        }

        const float frames = deltaTime * VisualErrorReferenceHz;
        visualPositionError *= std::pow(decayFactor(positionError, smoothing.smallError, smoothing.largeError), frames);
        const float rotationDecay =
            std::pow(decayFactor(angleError, smoothing.smallRotation, smoothing.largeRotation), frames);
        visualRotationError = glm::slerp(glm::quat(1.0f, 0.0f, 0.0f, 0.0f), visualRotationError, rotationDecay);
    }

    glm::vec3 RigidbodyComponent::getInterpolatedPosition(float alpha) const {
        return glm::mix(previousPosition, currentPosition, alpha);
    }

    glm::quat RigidbodyComponent::getInterpolatedRotation(float alpha) const {
        return glm::slerp(previousRotation, currentRotation, alpha);
    }

    void RigidbodyComponent::tick(float deltaTime) {
        decayVisualError(deltaTime);

        // we want to be able to edit the physics bodies in the editor so still run this on tick.
        const TransformComponent* transform = getSibling<TransformComponent>();
        if (!ensureBody(transform)) return;

        // When we're paused we take the transform from the gizmo but if we're simulating we trust the simulation
        if (!GameplayStatics::isSimulating()) {
            PhysicsManager::get().setBodyTransform(body, transform->getPosition(), transform->getRotation());
        }
    }

    bool RigidbodyComponent::isAtRest() const {
        if (body == InvalidBody) return true;
        if (!PhysicsManager::get().isBodyActive(body)) return true;
        const glm::vec3 linear = getLinearVelocity();
        const glm::vec3 angular = getAngularVelocity();
        return glm::dot(linear, linear) < RestLinearSpeed * RestLinearSpeed
            && glm::dot(angular, angular) < RestAngularSpeed * RestAngularSpeed;
    }

    // Points both render-interpolation endpoints at one pose. The offset below is solved against
    // the body, but rendering applies it to the interpolated pose, and those endpoints are only
    // refreshed after the physics step. Left alone they lag the body by up to a tick, which on a
    // ball spinning at 40 rad/s is around 38 degrees of rotation injected on every correction.
    void RigidbodyComponent::collapseInterpolation(const glm::vec3& position, const glm::quat& rotation) {
        previousPosition = position;
        previousRotation = rotation;
        currentPosition = position;
        currentRotation = rotation;
        poseCount = 2;
    }

    // Moves the body and nothing else. Hiding the move is the caller's job, by wrapping this in
    // captureVisualReference/applyVisualReference: a correction is always followed by a replay that
    // moves the body again, and only the total displacement is worth smoothing.
    void RigidbodyComponent::setState(const glm::vec3& position, const glm::quat& rotation,
                                      const glm::vec3& linear, const glm::vec3& angular,
                                      const bool atRest) {
        if (body == InvalidBody) return;

        glm::vec3 bodyPosition;
        glm::quat bodyRotation;
        PhysicsManager::get().getBodyTransform(body, bodyPosition, bodyRotation);
        const glm::vec3 error = position - bodyPosition;

        // setBodyTransform always wakes the body, so a settled one already in about the right place
        // must be left alone or every packet holds it awake.
        if (atRest && isAtRest() && glm::dot(error, error) < RestSnapEpsilon * RestSnapEpsilon) return;

        PhysicsManager::get().setBodyTransform(body, position, rotation);
        PhysicsManager::get().setLinearVelocity(body, linear);
        PhysicsManager::get().setAngularVelocity(body, angular);
        collapseInterpolation(position, rotation);
    }

    // Where this body is on screen right now: the interpolated pose plus whatever offset is still
    // decaying from an earlier correction. alpha must be the one the last frame was drawn with.
    void RigidbodyComponent::captureVisualReference(const float alpha) {
        if (body == InvalidBody) return;
        renderedPosition = getInterpolatedPosition(alpha) + visualPositionError;
        renderedRotation = glm::normalize(visualRotationError * getInterpolatedRotation(alpha));
    }

    // Re-solves the offset against whatever trajectory the rollback left behind, so the render lands
    // back where the eye had it however far the body moved in between. Continuity is unconditional:
    // the offset is taken in full, and how fast it is given back is decayVisualError's business.
    // Capping it here would make the part given up an instant jump, which is the one thing this
    // whole mechanism exists to avoid.
    //
    // Deliberately does not touch the interpolation endpoints. Collapsing them onto the corrected
    // pose also works, but it costs the body a tick of smooth motion, and a body corrected on every
    // packet would pay that ten times a second. Reading the endpoints instead means a body the
    // rollback left alone comes out with exactly the offset it already had.
    void RigidbodyComponent::applyVisualReference(const float alpha) {
        if (body == InvalidBody) return;
        visualPositionError = renderedPosition - getInterpolatedPosition(alpha);
        visualRotationError = glm::normalize(renderedRotation * glm::inverse(getInterpolatedRotation(alpha)));
    }

    void RigidbodyComponent::addForce(const glm::vec3& force) {
        PhysicsManager::get().addForce(body, force);
    }

    void RigidbodyComponent::addForceAtPosition(const glm::vec3& force, const glm::vec3& worldPosition) {
        PhysicsManager::get().addForceAtPosition(body, force, worldPosition);
    }

    void RigidbodyComponent::addTorque(const glm::vec3& torque) {
        PhysicsManager::get().addTorque(body, torque);
    }

    void RigidbodyComponent::addImpulse(const glm::vec3& impulse) {
        PhysicsManager::get().addImpulse(body, impulse);
    }

    void RigidbodyComponent::addAngularImpulse(const glm::vec3& angularImpulse) {
        PhysicsManager::get().addAngularImpulse(body, angularImpulse);
    }

    glm::vec3 RigidbodyComponent::getLinearVelocity() const {
        return PhysicsManager::get().getLinearVelocity(body);
    }

    void RigidbodyComponent::setLinearVelocity(const glm::vec3& velocity) {
        PhysicsManager::get().setLinearVelocity(body, velocity);
    }

    glm::vec3 RigidbodyComponent::getAngularVelocity() const {
        return PhysicsManager::get().getAngularVelocity(body);
    }

    void RigidbodyComponent::setAngularVelocity(const glm::vec3& velocity) {
        PhysicsManager::get().setAngularVelocity(body, velocity);
    }

    float RigidbodyComponent::getMass() const {
        return PhysicsManager::get().getMass(body);
    }

    void RigidbodyComponent::setColliderTransform(size_t index, const glm::vec3 &offset, const glm::quat &rotation) {
        if (index >= colliders.size()) return;
        colliders[index].offset = offset;
        colliders[index].rotation = rotation;
        bodyDirty = true;
    }

    void RigidbodyComponent::drawInspector() {
        const char* typeNames[] = { "Static", "Dynamic", "Kinematic" };
        const char* shapeNames[] = { "Box", "Sphere", "Capsule" };

        int typeIdx = static_cast<int>(type);
        if (ImGui::Combo("Type", &typeIdx, typeNames, IM_ARRAYSIZE(typeNames))) {
            type = static_cast<BodyType>(typeIdx);
            bodyDirty = true;
        }

        ImGui::SeparatorText("Body");

        bool propertiesChanged = false;
        propertiesChanged |= ImGui::DragFloat("Friction", &properties.friction, 0.01f, 0.f, 1.f);
        propertiesChanged |= ImGui::DragFloat("Restitution", &properties.restitution, 0.01f, 0.f, 1.f);
        propertiesChanged |= ImGui::DragFloat("Linear Damping", &properties.linearDamping, 0.01f, 0.f, 10.f);
        propertiesChanged |= ImGui::DragFloat("Angular Damping", &properties.angularDamping, 0.01f, 0.f, 10.f);
        propertiesChanged |= ImGui::DragFloat("Gravity Factor", &properties.gravityFactor, 0.05f, -10.f, 10.f);
        propertiesChanged |= ImGui::DragFloat("Max Linear Velocity", &properties.maxLinearVelocity, 1.f, 0.f, 10000.f);
        propertiesChanged |= ImGui::DragFloat("Max Angular Velocity", &properties.maxAngularVelocity, 0.5f, 0.f, 1000.f);
        propertiesChanged |= ImGui::Checkbox("Allow Sleeping", &properties.allowSleeping);
        if (propertiesChanged) propertiesDirty = true;

        // Mass is baked into the body's inertia tensor, so it only takes effect on a rebuild.
        if (ImGui::DragFloat("Mass Override", &properties.overrideMass, 0.1f, 0.f, 10000.f, "%.2f (0 = from shape)"))
            bodyDirty = true;
        if (body != InvalidBody && type == BodyType::Dynamic) ImGui::Text("Mass: %.2f", getMass());

        ImGui::SeparatorText("Networking");

        ImGui::Text("At Rest: %s", isAtRest() ? "yes" : "no");
        ImGui::Text("Visual Error: %.3f m", glm::length(visualPositionError));

        ImGui::SeparatorText("Colliders");

        int removeIdx = -1;
        for (int i = 0; i < static_cast<int>(colliders.size()); ++i) {
            ColliderDef& c = colliders[i];
            ImGui::PushID(i);

            int shapeIdx = static_cast<int>(c.shape);
            if (ImGui::Combo("Shape", &shapeIdx, shapeNames, IM_ARRAYSIZE(shapeNames))) {
                c.shape = static_cast<ColliderShape>(shapeIdx);
                bodyDirty = true;
            }

            if (c.shape == ColliderShape::Box) {
                if (ImGui::DragFloat3("Half Extents", &c.halfExtents.x, 0.1f, 0.01f, 1000.f)) bodyDirty = true;
            } else {
                if (ImGui::DragFloat("Radius", &c.radius, 0.1f, 0.01f, 1000.f)) bodyDirty = true;
                if (c.shape == ColliderShape::Capsule) {
                    if (ImGui::DragFloat("Half Height", &c.halfHeight, 0.1f, 0.01f, 1000.f)) bodyDirty = true;
                }
            }

            if (ImGui::DragFloat3("Offset", &c.offset.x, 0.1f)) bodyDirty = true;

            // keep at least one collider so the body always has a shape
            if (colliders.size() > 1 && ImGui::SmallButton("Remove")) removeIdx = i;

            ImGui::Separator();
            ImGui::PopID();
        }

        if (ImGui::Button("Add Collider")) {
            colliders.emplace_back();
            bodyDirty = true;
        }

        if (removeIdx >= 0) {
            colliders.erase(colliders.begin() + removeIdx);
            bodyDirty = true;
        }
    }
} // ytail
