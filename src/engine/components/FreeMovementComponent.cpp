//
// Created by PeterPC on 7/14/2026.
//

#include "FreeMovementComponent.h"

#include <algorithm>
#include <cmath>

#include <glm/gtc/quaternion.hpp>

#include "imgui.h"

#include "TransformComponent.h"
#include "engine/Constants.h"
#include "engine/Entity.h"
#include "engine/serialize/Archive.h"
#include "engine/Input.h"

namespace ytail
{
    void FreeMovementComponent::serialize(Archive& ar) {
        ar("moveSpeed", moveSpeed);
        ar("minSpeed", minSpeed);
        ar("maxSpeed", maxSpeed);
        ar("scrollPower", scrollPower);
        ar("lookSensitivity", lookSensitivity);
        ar("requireRightClick", requireRightClick);
    }

    bool FreeMovementComponent::ensureTransform(){
        if (transformComp == nullptr && owner != nullptr){
            transformComp = owner->getComponent<TransformComponent>();
        }
        if (transformComp == nullptr) return false;

        // Seed our yaw/pitch from the transform's starting rotation. Derive them from the forward
        // vector, matching the qY(yaw)*qX(pitch) build in eventTick — NOT glm::eulerAngles, whose
        // yaw is asin-limited to [-90,90] and folds larger yaws into pitch/roll (which snaps the
        // view on scene reload once you've turned past 90 degrees).
        if (!seeded){
            const glm::vec3 fwd = glm::normalize(transformComp->rotation * constant::WorldForward);
            pitch = glm::degrees(std::asin(std::clamp(fwd.y, -1.0f, 1.0f)));
            yaw   = glm::degrees(std::atan2(-fwd.x, -fwd.z));
            seeded = true;
        }
        return true;
    }

    void FreeMovementComponent::tick(float deltaTime){
        if (!ensureTransform()) return;

        // Always-on mode holds the mouse whenever we have focus. setMouseCaptured refuses while
        // the UI is active, so this yields to menus and re-grabs once they close.
        if (!requireRightClick && Input::get().isWindowFocused())
            Input::get().setMouseCaptured(true);

        // Movement follows capture: RMB-held in editor mode, always-on otherwise.
        if (!Input::get().isMouseCaptured()) return;

        // Held-key state, polled so movement is smooth and frame-rate independent
        const bool* keys = SDL_GetKeyboardState(nullptr);

        // Basis from the current orientation: forward is -Z, right is +X, up is world +Y
        const glm::quat& rot = transformComp->rotation;
        const glm::vec3 forward = rot * constant::WorldForward;
        const glm::vec3 right = rot * constant::WorldRight;

        glm::vec3 direction(0.f);
        if (keys[SDL_SCANCODE_W]) direction += forward;
        if (keys[SDL_SCANCODE_S]) direction -= forward;
        if (keys[SDL_SCANCODE_D]) direction += right;
        if (keys[SDL_SCANCODE_A]) direction -= right;
        if (keys[SDL_SCANCODE_E]) direction += constant::WorldUp;
        if (keys[SDL_SCANCODE_Q]) direction -= constant::WorldUp;

        // Normalize so diagonals aren't faster and skip if no keys are down
        if (glm::dot(direction, direction) > 0.f){
            float speed = moveSpeed;
            transformComp->position += glm::normalize(direction) * speed * deltaTime;
        }
    }

    void FreeMovementComponent::eventTick(const SDL_Event& event){
        if (!ensureTransform()) return;

        switch (event.type) {
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                // Don't grab the camera if the click was meant for an ImGui window.
                if (requireRightClick && event.button.button == SDL_BUTTON_RIGHT
                    && !ImGui::GetIO().WantCaptureMouse)
                    Input::get().setMouseCaptured(true);
                break;
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (requireRightClick && event.button.button == SDL_BUTTON_RIGHT)
                    Input::get().setMouseCaptured(false);
                break;
            case SDL_EVENT_MOUSE_WHEEL:
                moveSpeed += event.wheel.y * scrollPower;
                moveSpeed = SDL_clamp(moveSpeed, minSpeed, maxSpeed);
                break;
            case SDL_EVENT_MOUSE_MOTION:
                if (Input::get().isMouseCaptured()) {
                    yaw   -= event.motion.xrel * lookSensitivity;
                    pitch -= event.motion.yrel * lookSensitivity;
                    pitch = std::clamp(pitch, -89.f, 89.f);

                    // Yaw about world up, then pitch about local right. Order avoids roll.
                    transformComp->rotation =
                          glm::angleAxis(glm::radians(yaw),   constant::WorldUp)
                        * glm::angleAxis(glm::radians(pitch), constant::WorldRight);
                }
                break;
            default: ;
        }
    }

    void FreeMovementComponent::drawInspector() {
        ImGui::DragFloat("Move Speed", &moveSpeed, 0.1f, minSpeed, maxSpeed);
        ImGui::DragFloat("Min Speed", &minSpeed, 0.1f, 0.0f, maxSpeed);
        ImGui::DragFloat("Max Speed", &maxSpeed, 0.1f, minSpeed, 1000.0f);
        ImGui::DragFloat("Scroll Power", &scrollPower, 0.05f);
        ImGui::DragFloat("Look Sensitivity", &lookSensitivity, 0.01f, 0.0f, 5.0f);
        ImGui::Checkbox("Require Right Click", &requireRightClick);
    }
} // ytail
