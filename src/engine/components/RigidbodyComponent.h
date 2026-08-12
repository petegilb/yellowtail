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

    // How a network correction is hidden. The decay factors are per rendered frame at 60Hz, which
    // is the rate they were authored at and has nothing to do with the fixed tick: this is
    // cosmetic, so it follows the wall clock.
    struct VisualSmoothing {
        // A small offset fades gently, since holding a small lie costs nothing; a large one is
        // pulled in harder, since holding a large lie looks worse than the correction it hides. The
        // rate ramps between the two across the small..large range rather than stepping at a
        // threshold. Under sustained lag large errors are the common case, so this is the first pair
        // to try inverting if remote players slide rather than jitter.
        float smallDecay = 0.96f;
        float largeDecay = 0.94f;
        float smallError = 0.25f;
        float largeError = 1.0f;
        // Rotation ramps over its own range, because it diverges independently: a ball can sit in
        // the right place while its spin is visibly wrong. 10 to 90 degrees.
        float smallRotation = glm::radians(45.f);
        float largeRotation = 1.571f;
    };

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

        void fixedPreTick(float deltaTime) override;
        void fixedTick(float deltaTime) override;
        void tick(float deltaTime) override;

        std::vector<physics::ColliderDef> colliders{ {} };
        physics::BodyType type = physics::BodyType::Dynamic;
        physics::BodyProperties properties;
        // Edit properties directly, then call this so the change reaches the live body.
        void markPropertiesDirty() { propertiesDirty = true; }

        // Editor gizmo writes a collider's body-local offset/rotation and flags a rebuild.
        void setColliderTransform(size_t index, const glm::vec3& offset, const glm::quat& rotation);

        // Snaps the body onto an authoritative state, hard, because a correction spread across the
        // physics transform fights the solver and never resolves. The jump is hidden by the visual
        // error below rather than by slowing the correction down.
        // reference: https://www.gafferongames.com/post/state_synchronization/
        //
        // atRest skips the write when the body has already settled in about the right place, since
        // setBodyTransform always reactivates.
        void setState(const glm::vec3& position, const glm::quat& rotation,
                      const glm::vec3& linear, const glm::vec3& angular, bool atRest);
        // Bracket a rollback with these: the first records where the body is drawn, the second turns
        // the distance it travelled over the whole restore-and-replay into the offset that hides it.
        // alpha must be the one the last frame was drawn with.
        void captureVisualReference(float alpha);
        void applyVisualReference(float alpha);
        // True once Jolt has put the body to sleep, or it is moving slowly enough to count as settled.
        [[nodiscard]] bool isAtRest() const;

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

        // Where the body was before the last snap, carried as an offset that decays to nothing.
        // Rendering adds it back so a correction is smoothed out of the picture without any of it
        // reaching the simulation, which is what lets the snap itself stay hard.
        [[nodiscard]] const glm::vec3& getVisualPositionError() const { return visualPositionError; }
        [[nodiscard]] const glm::quat& getVisualRotationError() const { return visualRotationError; }

        // Shared by every body, and tuned at runtime: how a correction should be hidden is judged
        // by watching balls under lag, which is not a thing to rebuild for.
        static VisualSmoothing smoothing;

        static constexpr const char* SerialId = "rigidbody";
        void serialize(Archive& ar) override;
        [[nodiscard]] const char* serialId() const override { return SerialId; }

        [[nodiscard]] const char* getTypeName() const override { return "Rigidbody"; }
        void drawInspector() override;

    private:
        // create the body on the first tick, and rebuild it when an inspector edit marks it dirty
        bool ensureBody(const TransformComponent* transform);
        void decayVisualError(float deltaTime);
        void collapseInterpolation(const glm::vec3& position, const glm::quat& rotation);

        physics::BodyHandle body = physics::InvalidBody;
        bool bodyDirty = false;
        bool propertiesDirty = false;
        glm::vec3 visualPositionError{0.0f};
        glm::quat visualRotationError{1.0f, 0.0f, 0.0f, 0.0f};
        // The pose that was on screen when the rollback started.
        glm::vec3 renderedPosition{0.0f};
        glm::quat renderedRotation{1.0f, 0.0f, 0.0f, 0.0f};
        glm::vec3 previousPosition{0.0f};
        glm::quat previousRotation{1.0f, 0.0f, 0.0f, 0.0f};
        glm::vec3 currentPosition{0.0f};
        glm::quat currentRotation{1.0f, 0.0f, 0.0f, 0.0f};
        // 0 until the body has been stepped twice, so the first frames don't blend against a default pose
        int poseCount = 0;
    };
} // ytail

#endif //YELLOWTAIL_RIGIDBODYCOMPONENT_H
