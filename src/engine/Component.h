//
// Created by Peter Gilbert on 6/28/26.
//

#ifndef YELLOWTAIL_COMPONENT_H
#define YELLOWTAIL_COMPONENT_H

namespace ytail {
    class Component {
public:
        Component();
        virtual ~Component() = default;

        virtual void fixedTick(float deltaTime) {}

        virtual void tick(float deltaTime) {}
    };
} // ytail

#endif //YELLOWTAIL_COMPONENT_H