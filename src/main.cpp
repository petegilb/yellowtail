#include <iostream>

#include <SDL3/SDL.h>

class Yellowtail {
public:
    Yellowtail() {
        std::cout << "Starting yellowtail..." << std::endl;
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            std::cout << "Error starting SDL!" << std::endl;
        }
    }
    ~Yellowtail() {
        std::cout << "Ending yellowtail..." << std::endl;
        if (window) SDL_DestroyWindow(window);
        SDL_Quit();
    }

    void run() {
        window = SDL_CreateWindow("yellowtail.", 1280, 720, 0);
        if (!window) {
            std::cout << "Error creating SDL window!" << std::endl;
        }
        mainLoop();
    }

    void mainLoop() {
        Uint64 fps = 0;
        Uint64 lastTime = 0;
        while (bRunning) {
            const Uint64 currentTick = SDL_GetTicks();
            tick(0.0f);
            fps++;
            Uint64 deltaTime = SDL_GetTicks() - currentTick;

            // framerate lock
            // SDL_Delay(16);
            // 1 sec == 1000ms and 1000ms/60frames = 16ms
            // we delay the target amount - the delta time
            if (framerateLock > 0) {
                Sint64 targetMs = 1000/framerateLock;
                targetMs -= static_cast<Sint64>(deltaTime);
                if (targetMs > 0) {
                    SDL_Delay(targetMs);
                }
            }

            // this is once a second (so we can find our frames per second)
            if (currentTick > lastTime + 1000) {
                lastTime = currentTick;
                std::string debugString = "Current FPS: " + std::to_string(fps) + " Delta Time: " + std::to_string(deltaTime);
                SDL_SetWindowTitle(window, debugString.c_str());
                fps = 0;
            }
        }
    }

    void quit() {
        bRunning = false;
    }

    // Advance a frame (one iteration in the main loop)
    void tick(float deltaTime) {
        inputTick();
        updateTick();
        renderTick();
    }

    void inputTick() {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
                case SDL_EVENT_QUIT:
                    bRunning = false;
                    break;
                case SDL_EVENT_KEY_DOWN:
                    handleInput(event.key);
                    break;
                default: ;
            }
        }
    }

    void updateTick() {

    }

    void renderTick() {

    }

    void handleInput(const SDL_KeyboardEvent& keyboard_event) {
        if (keyboard_event.key == SDLK_ESCAPE) {
            quit();
        }
    }

protected:
    SDL_Window* window = nullptr;
    bool bRunning = true;

    // locks the framerate if greater than 0
    int framerateLock = 30;
};


// entry point
int main(int argc, char* argv[]) {
    std::cout << "Starting yellowtail process." << std::endl;
    Yellowtail Game;
    Game.run();
    std::cout << "Ending yellowtail process." << std::endl;
    return 0;
}
