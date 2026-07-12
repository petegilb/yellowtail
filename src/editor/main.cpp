#include "engine/Engine.h"

// Editor entry point. For now this mirrors the game entry - it just runs the engine so the
// target builds and links. It becomes the editor host once the engine exposes a Game seam.
int main(int argc, char* argv[]) {
    SDL_Log("Starting yellowtail editor process.");
    ytail::Engine Engine;
    return Engine.run();
}
