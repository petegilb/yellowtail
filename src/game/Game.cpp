//
// Created by Peter Gilbert on 7/14/26.
//

#include "Game.h"

#include "engine/Engine.h"
#include "engine/components/RenderComponent.h"
#include "engine/components/TransformComponent.h"
#include "engine/components/CameraComponent.h"
#include "engine/components/LightComponent.h"
#include "engine/managers/ResourceManager.h"

namespace ytail
{
    Game::Game(Engine* inEngine) : Application(inEngine) {
    }

    void Game::start(){

    }

    void Game::tick(float deltaTime){
    }
} // ytail
