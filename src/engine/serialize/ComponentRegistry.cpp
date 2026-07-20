//
// Created for scene serialization.
//

#include "ComponentRegistry.h"

#include <SDL3/SDL.h>

#include "../components/TransformComponent.h"
#include "../components/CameraComponent.h"
#include "../components/LightComponent.h"
#include "../components/FreeMovementComponent.h"
#include "../components/RigidbodyComponent.h"
#include "../components/RenderComponent.h"

namespace ytail {
    std::unique_ptr<Component> ComponentRegistry::create(const std::string& id) const {
        const auto it = factories.find(id);
        if (it == factories.end()) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Unknown component serialId: %s", id.c_str());
            return nullptr;
        }
        return it->second();
    }

    void ComponentRegistry::registerBuiltins() {
        registerType<TransformComponent>(TransformComponent::SerialId);
        registerType<CameraComponent>(CameraComponent::SerialId);
        registerType<LightComponent>(LightComponent::SerialId);
        registerType<FreeMovementComponent>(FreeMovementComponent::SerialId);
        registerType<RigidbodyComponent>(RigidbodyComponent::SerialId);
        registerType<RenderComponent>(RenderComponent::SerialId);
    }
} // ytail
