//
// Created by Peter Gilbert on 6/28/26.
//

#include "Component.h"

#include "World.h"

namespace ytail {
    Component::Component() {
    }

    Entity* Component::getOwner() const {
        return world != nullptr ? world->getEntity(ownerId) : nullptr;
    }
} // ytail
