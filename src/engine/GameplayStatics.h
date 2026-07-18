//
// Created by Peter Gilbert on 7/18/26.
//

#ifndef YELLOWTAIL_GAMEPLAYSTATICS_H
#define YELLOWTAIL_GAMEPLAYSTATICS_H

#include <cstdint>

namespace ytail {
    class Engine;

    enum class PlayState : uint8_t { Paused, Simulating };

    // Static accessors for global game state
    class GameplayStatics {
    public:
        [[nodiscard]] static PlayState getPlayState();
        [[nodiscard]] static bool isSimulating();

    private:
        // the engine binds itself here at construction
        friend class Engine;  
        static Engine* engine;
    };
} // ytail

#endif //YELLOWTAIL_GAMEPLAYSTATICS_H
