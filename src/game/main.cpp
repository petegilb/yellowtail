#include "engine/Engine.h"
#include "Game.h"

// entry point
int main(int argc, char* argv[]) {
    SDL_Log("Starting yellowtail process.");
    ytail::Engine engine;
    ytail::Game game(&engine);
    engine.setApplication(&game);
    return engine.run();
}
