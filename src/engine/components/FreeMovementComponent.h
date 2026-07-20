//
// Created by PeterPC on 7/14/2026.
//

#ifndef YELLOWTAIL_FREEMOVEMENTCOMPONENT_H
#define YELLOWTAIL_FREEMOVEMENTCOMPONENT_H
#include "../Component.h"

namespace ytail
{
    class TransformComponent;

    // Flies the sibling transform around like an editor/spectator camera.
    // WASD moves on the view plane, E/Q go world up/down, hold Left Shift to sprint.
    class FreeMovementComponent : public Component{
    public:
        void tick(float deltaTime) override;
        void eventTick(const SDL_Event& event) override;

        // units per second
        float moveSpeed = 15.f;
        float minSpeed = 1.f;
        float maxSpeed = 100.f;
        float scrollPower = 1.5f;

        // degrees of rotation per pixel of mouse movement
        float lookSensitivity = 0.1f;

        // true = hold right click for movement
        bool requireRightClick = true;

        static constexpr const char* SerialId = "freeMovement";
        void serialize(Archive& ar) override;
        [[nodiscard]] const char* serialId() const override { return SerialId; }

        [[nodiscard]] const char* getTypeName() const override { return "Free Movement"; }
        void drawInspector() override;

    private:
        // make sure we can get the transform from the transform component
        bool ensureTransform();

        TransformComponent* transformComp = nullptr;

        // Look angles in degrees, owned here so we never decompose the quaternion.
        float yaw = 0.f;
        float pitch = 0.f;
        // if we've retrieved the starting transform from the transform component yet.
        bool seeded = false;
    };
} // ytail

#endif //YELLOWTAIL_FREEMOVEMENTCOMPONENT_H
