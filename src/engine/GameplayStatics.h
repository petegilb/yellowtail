//
// Created by Peter Gilbert on 7/18/26.
//

#ifndef YELLOWTAIL_GAMEPLAYSTATICS_H
#define YELLOWTAIL_GAMEPLAYSTATICS_H

#include <cstdint>

#include <glm/mat4x4.hpp>

#include "Entity.h"

namespace ytail {
    class Engine;
    class World;

    enum class PlayState : uint8_t { Paused, Simulating };

    // Static accessors for global game state
    class GameplayStatics {
    public:
        [[nodiscard]] static PlayState getPlayState();
        [[nodiscard]] static bool isSimulating();

        // The engine's entity/component storage, or nullptr before the engine exists.
        [[nodiscard]] static World* getWorld();

        // World matrix to draw with, blending the last two simulated poses by alpha.
        [[nodiscard]] static glm::mat4 renderWorldMatrix(const World& world, EntityId id, float alpha);

    private:
        // the engine binds itself here at construction
        friend class Engine;  
        static Engine* engine;
    };
} // ytail

#endif //YELLOWTAIL_GAMEPLAYSTATICS_H
