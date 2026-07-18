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
        ~RigidbodyComponent() override;

        void fixedTick(float deltaTime) override;
        void tick(float deltaTime) override;

        std::vector<physics::ColliderDef> colliders{ {} };
        physics::BodyType type = physics::BodyType::Dynamic;

        // Editor gizmo writes a collider's body-local offset/rotation and flags a rebuild.
        void setColliderTransform(size_t index, const glm::vec3& offset, const glm::quat& rotation);

        [[nodiscard]] const char* getTypeName() const override { return "Rigidbody"; }
        void drawInspector() override;

    private:
        // create the body on the first tick, and rebuild it when an inspector edit marks it dirty
        bool ensureBody();

        TransformComponent* transformComp = nullptr;
        physics::BodyHandle body = physics::InvalidBody;
        bool bodyDirty = false;
    };
} // ytail

#endif //YELLOWTAIL_RIGIDBODYCOMPONENT_H
