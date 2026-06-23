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
        Uint64 fpsTimer = SDL_GetTicksNS();
        Uint64 lastFrame = SDL_GetTicksNS();
        // TODO change to accumulator and fixed time step
        while (bRunning) {
            const Uint64 frameStart = SDL_GetTicksNS();
            const float deltaTime = static_cast<float>(frameStart - lastFrame) / SDL_NS_PER_SECOND;
            lastFrame = frameStart;

            tick(deltaTime);
            fps++;

            // framerate lock: sleep for whatever is left of this frame's budget.
            // 1 sec / N frames = budget per frame; subtract the work we just did.
            if (framerateLock > 0) {
                const Uint64 targetNs = SDL_NS_PER_SECOND / framerateLock;
                const Uint64 workedNs = SDL_GetTicksNS() - frameStart;
                if (workedNs < targetNs) {
                    SDL_DelayNS(targetNs - workedNs);
                }
            }

            // once a second, report FPS and the average frame time in ms
            if (frameStart - fpsTimer >= SDL_NS_PER_SECOND) {
                fpsTimer = frameStart;
                const double frameMs = fps > 0 ? 1000.0 / static_cast<double>(fps) : 0.0;
                char title[128];
                SDL_snprintf(title, sizeof(title), "yellowtail | %d FPS | %.2f ms/frame",
                             static_cast<int>(fps), frameMs);
                SDL_SetWindowTitle(window, title);
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
    int framerateLock = 60;
};


// entry point
int main(int argc, char* argv[]) {
    std::cout << "Starting yellowtail process." << std::endl;
    Yellowtail Game;
    Game.run();
    std::cout << "Ending yellowtail process." << std::endl;
    return 0;
}
