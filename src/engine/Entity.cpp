//
// Created by Peter Gilbert on 6/28/26.
//

#include "Entity.h"

#include "World.h"

namespace ytail {
    Entity::Entity(const EntityId newId, World* inWorld) : world(inWorld), entityId(newId) {
        name = "Entity " + std::to_string(entityIndex(newId));
    }

    Entity* Entity::getParent() const {
        return world != nullptr ? world->getEntity(parentId) : nullptr;
    }
} // ytail
