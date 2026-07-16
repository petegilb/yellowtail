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
    enum class BodyType : uint8_t { Static, Dynamic };
    enum class ColliderShape : uint8_t { Box, Sphere };

    // Opaque handle to a Jolt body (its BodyID as a raw int, so callers never see Jolt).
    using BodyHandle = uint32_t;
    static constexpr BodyHandle InvalidBody = 0xFFFFFFFFu;  // matches JPH::BodyID::cInvalidBodyID

    struct BodyDef {
        ColliderShape shape = ColliderShape::Box;
        glm::vec3 halfExtents{0.5f};  // box
        float radius = 0.5f;          // sphere
        glm::vec3 position{0.0f};
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
        BodyType type = BodyType::Dynamic;
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
