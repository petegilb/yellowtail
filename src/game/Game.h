//
// Created by Peter Gilbert on 7/14/26.
//

#ifndef YELLOWTAIL_GAME_H
#define YELLOWTAIL_GAME_H

#include "engine/Application.h"

namespace ytail
{
    class Engine;

    class Game : public Application {
    public:
        explicit Game(Engine* inEngine);

        void start() override;
        void tick(float deltaTime) override;
    };
} // ytail

#endif //YELLOWTAIL_GAME_H
