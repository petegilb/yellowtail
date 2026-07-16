//
// Created by Peter Gilbert on 6/28/26.
//

#ifndef YELLOWTAIL_COMPONENT_H
#define YELLOWTAIL_COMPONENT_H

#include <SDL3/SDL.h>

namespace ytail {
    class Component {
public:
        Component();
        virtual ~Component() = default;

        virtual void fixedTick(float deltaTime) {}

        virtual void tick(float deltaTime) {}

        virtual void eventTick(const SDL_Event& event){}
    protected:
        friend class Entity;
        Entity* owner = nullptr;
    };
} // ytail

#endif //YELLOWTAIL_COMPONENT_H