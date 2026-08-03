//
// Created by Peter Gilbert on 6/28/26.
//

#ifndef YELLOWTAIL_COMPONENT_H
#define YELLOWTAIL_COMPONENT_H

#include <SDL3/SDL.h>

#include "Entity.h"

namespace ytail {
    class World;
    struct Archive;

    // Components are stored by value in the World's pools, so a Component* is not safe
    class Component {
public:
        Component();
        virtual ~Component() = default;

        // A user-declared destructor removes the compiler's move operations, so we bring them
        // back explicitly. It matters for subclasses that own a resource: see RigidbodyComponent.
        // A suppressed move would fall back to copying the body handle and double-free it.
        Component(const Component&) = default;
        Component& operator=(const Component&) = default;
        Component(Component&&) = default;
        Component& operator=(Component&&) = default;

        virtual void fixedTick(float deltaTime) {}

        virtual void tick(float deltaTime) {}

        virtual void eventTick(const SDL_Event& event){}

        // Save or load this component's fields through the archive (same code for both).
        virtual void serialize(Archive& ar) {}
        // The name used to save this component and build it back later. Never change it
        [[nodiscard]] virtual const char* serialId() const { return "component"; }

        // Editor inspector: label for the collapsing header, and the ImGui widgets
        [[nodiscard]] virtual const char* getTypeName() const { return "Component"; }
        virtual void drawInspector() {}

        [[nodiscard]] EntityId getOwnerId() const { return ownerId; }
    protected:
        friend class World;

        // The entity this component is attached to (pointer valid until the next add/remove).
        [[nodiscard]] Entity* getOwner() const;
        // Another component on the same entity, or nullptr if it has none. Defined in World.h.
        template<typename T>
        [[nodiscard]] T* getSibling() const;

        EntityId ownerId = NULL_ENTITY;
        World* world = nullptr;
    };
} // ytail

#endif //YELLOWTAIL_COMPONENT_H
