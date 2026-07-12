#include "engine/Engine.h"

// entry point
int main(int argc, char* argv[]) {
    SDL_Log("Starting yellowtail process.");
    ytail::Engine Engine;
    return Engine.run();
}
