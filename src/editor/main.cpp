#include "engine/Engine.h"
#include "Editor.h"

// Editor entry point. Same engine, driven by the Editor application instead of the game.
int main(int argc, char* argv[]) {
    SDL_Log("Starting yellowtail editor process.");
    ytail::Engine engine;
    ytail::Editor editor(&engine);
    engine.setApplication(&editor);
    return engine.run();
}
