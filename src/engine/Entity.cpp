//
// Created by Peter Gilbert on 6/28/26.
//

#include "Entity.h"

#include "World.h"

namespace ytail {
    Entity::Entity(const EntityId newId, World* inWorld) : world(inWorld), entityId(newId) {
        name = "Entity " + std::to_string(entityIndex(newId));
    }

    Entity::Entity(Entity&& other) noexcept
        : world(other.world), parentId(other.parentId), childIds(std::move(other.childIds)),
          entityId(other.entityId), name(std::move(other.name)), serializable(other.serializable),
          components(std::move(other.components)) {
        for (const auto& comp : components) comp->owner = this;
    }

    Entity& Entity::operator=(Entity&& other) noexcept {
        if (this == &other) return *this;
        world = other.world;
        parentId = other.parentId;
        childIds = std::move(other.childIds);
        entityId = other.entityId;
        name = std::move(other.name);
        serializable = other.serializable;
        components = std::move(other.components);
        for (const auto& comp : components) comp->owner = this;
        return *this;
    }

    Entity* Entity::getParent() const {
        return world != nullptr ? world->getEntity(parentId) : nullptr;
    }
} // ytail
