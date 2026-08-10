//
// Created by Peter Gilbert on 7/16/26.
//

#include "RigidbodyComponent.h"

#include <cmath>

#include "imgui.h"

#include "TransformComponent.h"
#include "engine/World.h"
#include "engine/GameplayStatics.h"
#include "engine/serialize/Archive.h"
#include "engine/serialize/EnumJson.h"

namespace ytail {
    using namespace physics;

    // cos of half the largest rotation step driveTo will derive an angular velocity from (~23 degrees).
    constexpr float MaxDrivenRotationCos = 0.98f;

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

    glm::vec3 RigidbodyComponent::getInterpolatedPosition(float alpha) const {
        return glm::mix(previousPosition, currentPosition, alpha);
    }

    glm::quat RigidbodyComponent::getInterpolatedRotation(float alpha) const {
        return glm::slerp(previousRotation, currentRotation, alpha);
    }

    void RigidbodyComponent::tick(float deltaTime) {
        // we want to be able to edit the physics bodies in the editor so still run this on tick.
        const TransformComponent* transform = getSibling<TransformComponent>();
        if (!ensureBody(transform)) return;

        // When we're paused we take the transform from the gizmo but if we're simulating we trust the simulation
        if (!GameplayStatics::isSimulating()) {
            PhysicsManager::get().setBodyTransform(body, transform->getPosition(), transform->getRotation());
        }
    }

    void RigidbodyComponent::setNetworkDriven(const bool driven) {
        if (networkDriven == driven) return;
        if (driven && type == BodyType::Static) return;
        networkDriven = driven;
        if (driven) authoredType = type;

        // Switched in place rather than through bodyDirty: a rebuild would drop velocity, contacts and the BodyID.
        type = driven ? BodyType::Kinematic : authoredType;
        if (body != InvalidBody) PhysicsManager::get().setBodyMotionType(body, type);
        poseCount = 0;
    }

    void RigidbodyComponent::driveTo(const glm::vec3& position, const glm::quat& rotation, const float deltaTime) {
        if (body == InvalidBody || !networkDriven || deltaTime <= 0.0f) return;

        glm::vec3 bodyPosition;
        glm::quat bodyRotation;
        PhysicsManager::get().getBodyTransform(body, bodyPosition, bodyRotation);

        // Past these limits the derived velocity would launch whatever this body is touching, so
        // teleport and give up the push for a frame.
        const glm::vec3 step = position - bodyPosition;
        const float maxStep = maxDrivenSpeed * deltaTime;
        const float rotationDot = std::abs(glm::dot(bodyRotation, rotation));
        if (glm::dot(step, step) > maxStep * maxStep || rotationDot < MaxDrivenRotationCos) {
            PhysicsManager::get().setBodyTransform(body, position, rotation);
            return;
        }

        PhysicsManager::get().moveKinematic(body, position, rotation, deltaTime);
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
