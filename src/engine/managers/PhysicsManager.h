//
// Created by Peter Gilbert on 7/14/26.
//

#ifndef YELLOWTAIL_PHYSICSMANAGER_H
#define YELLOWTAIL_PHYSICSMANAGER_H

#include <cstdint>
#include <memory>
#include <vector>

#include <glm/vec3.hpp>
#include <glm/gtc/quaternion.hpp>

#include "../render/JoltDebugVertex.h"

namespace ytail::physics {
    enum class BodyType : uint8_t { Static, Dynamic, Kinematic };
    enum class ColliderShape : uint8_t { Box, Sphere, Capsule };

    // Opaque handle to a Jolt body (its BodyID as a raw int, so callers never see Jolt).
    using BodyHandle = uint32_t;
    static constexpr BodyHandle InvalidBody = 0xFFFFFFFFu; // matches JPH::BodyID::cInvalidBodyID

    // One collision shape plus its body-local transform. A body's shape is a list of these:
    // one = a plain (or offset) shape, several = a compound.
    struct ColliderDef {
        ColliderShape shape = ColliderShape::Box;
        glm::vec3 halfExtents{0.5f}; // box
        float radius = 0.5f; // sphere, capsule
        float halfHeight = 0.5f; // capsule (half-height of the cylindrical middle)
        glm::vec3 offset{0.0f}; // local to the body
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    };

    struct BodyDef {
        std::vector<ColliderDef> colliders;
        glm::vec3 position{0.0f}; // body world pose
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
        BodyType type = BodyType::Dynamic;
    };

    struct RayHit {
        BodyHandle body = InvalidBody;
        glm::vec3 position{0.0f};
        glm::vec3 normal{0.0f};
        float distance = 0.0f;
    };

    // Owns the Jolt physics world. Singleton so the header stays Jolt-free; all Jolt lives in the .cpp.
    class PhysicsManager {
    public:
        static PhysicsManager& get();

        // advance the simulation by a fixed dt (collisionSteps = Jolt sub-steps, 1 is fine at 60Hz)
        void step(float deltaTime, int collisionSteps = 1);

        BodyHandle createBody(const BodyDef& def);
        void removeBody(BodyHandle handle);

        void getBodyTransform(BodyHandle handle, glm::vec3& outPosition, glm::quat& outRotation) const;
        void setBodyTransform(BodyHandle handle, const glm::vec3& position, const glm::quat& rotation);

        // changes motion type in place, keeping the body's velocity and BodyID
        void setBodyMotionType(BodyHandle handle, BodyType type);
        // moves a kinematic body toward a target so Jolt derives a velocity from it (setBodyTransform teleports)
        void moveKinematic(BodyHandle handle, const glm::vec3& position, const glm::quat& rotation, float deltaTime);
        [[nodiscard]] glm::vec3 getLinearVelocity(BodyHandle handle) const;
        void setLinearVelocity(BodyHandle handle, const glm::vec3& velocity);
        [[nodiscard]] glm::vec3 getAngularVelocity(BodyHandle handle) const;
        void setAngularVelocity(BodyHandle handle, const glm::vec3& velocity);

        // Forces and torques accumulate and are consumed by the next step(), so apply them on
        // every fixed tick they should act on. Impulses apply once. All world space, all wake
        // a sleeping body.
        void addForce(BodyHandle handle, const glm::vec3& force);
        void addForceAtPosition(BodyHandle handle, const glm::vec3& force, const glm::vec3& worldPosition);
        void addTorque(BodyHandle handle, const glm::vec3& torque);
        void addImpulse(BodyHandle handle, const glm::vec3& impulse);
        void addAngularImpulse(BodyHandle handle, const glm::vec3& angularImpulse);

        // 0 for static and kinematic bodies, which Jolt treats as infinitely heavy.
        [[nodiscard]] float getMass(BodyHandle handle) const;
        // Damping is the fraction of velocity bled off per second. Friction and restitution are 0..1.
        void setLinearDamping(BodyHandle handle, float damping);
        void setAngularDamping(BodyHandle handle, float damping);
        void setFriction(BodyHandle handle, float friction);
        void setRestitution(BodyHandle handle, float restitution);
        void setGravityFactor(BodyHandle handle, float factor);

        // Closest hit along the ray, where direction carries the ray's length. ignoreBody skips
        // one body, usually the caster's own. Returns false and leaves outHit alone on a miss.
        bool castRay(const glm::vec3& origin, const glm::vec3& direction, RayHit& outHit,
                     BodyHandle ignoreBody = InvalidBody) const;

        // generate debug wireframe so we can draw it in the renderer
        void debugDraw();
        [[nodiscard]] const std::vector<JoltDebugVertex>& getDebugLines() const;

        PhysicsManager(const PhysicsManager&) = delete;
        PhysicsManager& operator=(const PhysicsManager&) = delete;

    private:
        PhysicsManager();
        ~PhysicsManager();

        struct Impl;
        std::unique_ptr<Impl> impl;
    };
} // ytail::physics

#endif //YELLOWTAIL_PHYSICSMANAGER_H
