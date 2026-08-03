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
    Component* ComponentRegistry::emplaceById(const std::string& id, World& world, const EntityId entity) const {
        const auto it = indexById.find(id);
        if (it == indexById.end()) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Unknown component serialId: %s", id.c_str());
            return nullptr;
        }
        const ComponentTypeInfo& info = types[it->second];
        // One component per type per entity. Old scene files could contain duplicates; keep
        // the first instead of letting the second silently overwrite it.
        if (info.get(world, entity) != nullptr) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Entity %u already has a %s; skipping duplicate",
                         entity, id.c_str());
            return nullptr;
        }
        return info.emplace(world, entity);
    }

    void ComponentRegistry::reserveById(const std::string& id, World& world, const size_t count) const {
        const auto it = indexById.find(id);
        if (it == indexById.end()) return;
        types[it->second].reserve(world, count);
    }

    void ComponentRegistry::registerBuiltins() {
        // Registration order sets the tick order (World::tickAll walks pools by type id).
        // Reorder here if tick order ever matters.
        registerType<TransformComponent>(TransformComponent::SerialId);
        registerType<CameraComponent>(CameraComponent::SerialId);
        registerType<LightComponent>(LightComponent::SerialId);
        registerType<FreeMovementComponent>(FreeMovementComponent::SerialId);
        registerType<RigidbodyComponent>(RigidbodyComponent::SerialId);
        registerType<RenderComponent>(RenderComponent::SerialId);
    }
} // ytail
