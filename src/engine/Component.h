//
// Created by Peter Gilbert on 6/28/26.
//

#ifndef YELLOWTAIL_COMPONENT_H
#define YELLOWTAIL_COMPONENT_H

#include <SDL3/SDL.h>

namespace ytail {
    class Entity;
    struct Archive;
    class Component {
public:
        Component();
        virtual ~Component() = default;

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
    protected:
        friend class Entity;
        Entity* owner = nullptr;
    };
} // ytail

#endif //YELLOWTAIL_COMPONENT_H