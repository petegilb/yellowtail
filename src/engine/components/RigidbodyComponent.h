//
// Created by Peter Gilbert on 7/16/26.
//

#ifndef YELLOWTAIL_RIGIDBODYCOMPONENT_H
#define YELLOWTAIL_RIGIDBODYCOMPONENT_H
#include <vector>

#include "../Component.h"
#include "../managers/PhysicsManager.h"

namespace ytail {
    class TransformComponent;

    // Ties the sibling transform to a Jolt body
    class RigidbodyComponent : public Component {
    public:
        RigidbodyComponent() = default;
        ~RigidbodyComponent() override;

        // Components get moved around inside their pool. The physics body handle must transfer
        // on move so the old copy's destructor doesn't delete a live body; copying would leave
        // two owners.
        RigidbodyComponent(RigidbodyComponent&& other) noexcept;
        RigidbodyComponent& operator=(RigidbodyComponent&& other) noexcept;
        RigidbodyComponent(const RigidbodyComponent&) = delete;
        RigidbodyComponent& operator=(const RigidbodyComponent&) = delete;

        void fixedTick(float deltaTime) override;
        void tick(float deltaTime) override;

        std::vector<physics::ColliderDef> colliders{ {} };
        physics::BodyType type = physics::BodyType::Dynamic;
        physics::BodyProperties properties;
        // Edit properties directly, then call this so the change reaches the live body.
        void markPropertiesDirty() { propertiesDirty = true; }
        // Above this, driveTo teleports rather than deriving a velocity. Raise it for fast movers.
        float maxDrivenSpeed = 50.0f;

        // Editor gizmo writes a collider's body-local offset/rotation and flags a rebuild.
        void setColliderTransform(size_t index, const glm::vec3& offset, const glm::quat& rotation);

        // Hands the body to the network: it becomes kinematic so the local solver stops deciding
        // where it goes, and driveTo moves it so Jolt still derives a velocity to push things with.
        void setNetworkDriven(bool driven);
        [[nodiscard]] bool isNetworkDriven() const { return networkDriven; }
        void driveTo(const glm::vec3& position, const glm::quat& rotation, float deltaTime);

        // Forwarded to the body, and no-ops until it exists (first fixedTick). Forces and torques
        // are consumed by the next step, so apply them from fixedTick, not tick.
        void addForce(const glm::vec3& force);
        void addForceAtPosition(const glm::vec3& force, const glm::vec3& worldPosition);
        void addTorque(const glm::vec3& torque);
        void addImpulse(const glm::vec3& impulse);
        void addAngularImpulse(const glm::vec3& angularImpulse);

        [[nodiscard]] glm::vec3 getLinearVelocity() const;
        void setLinearVelocity(const glm::vec3& velocity);
        [[nodiscard]] glm::vec3 getAngularVelocity() const;
        void setAngularVelocity(const glm::vec3& velocity);
        [[nodiscard]] float getMass() const;

        // For the parts of PhysicsManager this doesn't wrap (damping, friction, ray filtering).
        // InvalidBody until the first fixedTick, and it changes when an inspector edit rebuilds.
        [[nodiscard]] physics::BodyHandle getBody() const { return body; }

        // The last two simulated poses. Rendering blends between them so motion stays smooth when
        // the frame rate and the fixed step disagree.
        [[nodiscard]] bool hasInterpolatedPose() const { return poseCount >= 2; }
        [[nodiscard]] glm::vec3 getInterpolatedPosition(float alpha) const;
        [[nodiscard]] glm::quat getInterpolatedRotation(float alpha) const;

        static constexpr const char* SerialId = "rigidbody";
        void serialize(Archive& ar) override;
        [[nodiscard]] const char* serialId() const override { return SerialId; }

        [[nodiscard]] const char* getTypeName() const override { return "Rigidbody"; }
        void drawInspector() override;

    private:
        // create the body on the first tick, and rebuild it when an inspector edit marks it dirty
        bool ensureBody(const TransformComponent* transform);

        physics::BodyHandle body = physics::InvalidBody;
        bool bodyDirty = false;
        bool propertiesDirty = false;
        bool networkDriven = false;
        physics::BodyType authoredType = physics::BodyType::Dynamic;
        glm::vec3 previousPosition{0.0f};
        glm::quat previousRotation{1.0f, 0.0f, 0.0f, 0.0f};
        glm::vec3 currentPosition{0.0f};
        glm::quat currentRotation{1.0f, 0.0f, 0.0f, 0.0f};
        // 0 until the body has been stepped twice, so the first frames don't blend against a default pose
        int poseCount = 0;
    };
} // ytail

#endif //YELLOWTAIL_RIGIDBODYCOMPONENT_H
