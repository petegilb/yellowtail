//
// Created by Peter Gilbert on 7/18/26.
//

#ifndef YELLOWTAIL_GAMEPLAYSTATICS_H
#define YELLOWTAIL_GAMEPLAYSTATICS_H

#include <cstdint>

#include <SDL3/SDL_stdinc.h>

#include <glm/mat4x4.hpp>

#include "Entity.h"

namespace ytail {
    class Engine;
    class World;
    namespace net { class ReplicationManager; }
    namespace ocean { class OceanSimulation; }

    enum class PlayState : uint8_t { Paused, Simulating };

    // Static accessors for global game state
    class GameplayStatics {
    public:
        [[nodiscard]] static PlayState getPlayState();
        [[nodiscard]] static bool isSimulating();

        // The engine's entity/component storage, or nullptr before the engine exists.
        [[nodiscard]] static World* getWorld();

        // Fixed steps run so far. Components need it to tag input and state by tick.
        //
        // During a network rollback this reports the tick being replayed, not the one we have
        // really reached, which is what lets anything keyed off it replay correctly.
        [[nodiscard]] static Uint64 getTickNumber();

        // The wave field, for buoyancy and anything else asking where the water is. Nullptr when
        // the scene has no ocean, which is the signal that there is no water here at all rather
        // than water at height zero.
        [[nodiscard]] static ocean::OceanSimulation* getOcean();

#if YELLOWTAIL_WITH_NETWORKING
        // Nullptr before the engine exists. Bound to a peer only while a session is running, so
        // callers still have to check isBound().
        [[nodiscard]] static net::ReplicationManager* getReplication();
#endif

        // World matrix to draw with, blending the last two simulated poses by alpha.
        [[nodiscard]] static glm::mat4 renderWorldMatrix(const World& world, EntityId id, float alpha);

    private:
        // the engine binds itself here at construction
        friend class Engine;  
        static Engine* engine;
    };
} // ytail

#endif //YELLOWTAIL_GAMEPLAYSTATICS_H
