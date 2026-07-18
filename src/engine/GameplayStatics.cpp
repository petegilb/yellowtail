//
// Created by Peter Gilbert on 7/18/26.
//

#include "GameplayStatics.h"

#include "Engine.h"

namespace ytail {
    Engine* GameplayStatics::engine = nullptr;

    PlayState GameplayStatics::getPlayState() {
        return engine ? engine->getPlayState() : PlayState::Paused;
    }

    bool GameplayStatics::isSimulating() {
        return getPlayState() == PlayState::Simulating;
    }
} // ytail
