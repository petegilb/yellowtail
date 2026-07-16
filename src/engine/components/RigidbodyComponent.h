//
// Created by Peter Gilbert on 7/16/26.
//

#ifndef YELLOWTAIL_RIGIDBODYCOMPONENT_H
#define YELLOWTAIL_RIGIDBODYCOMPONENT_H
#include "../Component.h"
#include "../managers/PhysicsManager.h"

namespace ytail {
    class TransformComponent;

    // Ties the sibling transform to a Jolt body. Dynamic bodies drive the transform each fixed
    // step; static bodies are placed once at creation.
    class RigidbodyComponent : public Component {
    public:
        ~RigidbodyComponent() override;

        void fixedTick(float deltaTime) override;

        physics::ColliderShape shape = physics::ColliderShape::Box;
        glm::vec3 halfExtents{0.5f};  // box
        float radius = 0.5f;          // sphere
        physics::BodyType type = physics::BodyType::Dynamic;

    private:
        // create the body from the sibling transform on the first fixed tick
        bool ensureBody();

        TransformComponent* transformComp = nullptr;
        physics::BodyHandle body = physics::InvalidBody;
    };
} // ytail

#endif //YELLOWTAIL_RIGIDBODYCOMPONENT_H
