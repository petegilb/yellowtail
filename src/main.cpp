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

    bool bRunning = true;

    void run() {
        window = SDL_CreateWindow("yellowtail.", 1280, 720, 0);
        if (!window) {
            std::cout << "Error creating SDL window!" << std::endl;
        }
        while (bRunning) {
            tick(0.0f);
        }
    }

    void quit() {
        bRunning = false;
    }

    void tick(float deltaTime) {
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

    void handleInput(SDL_KeyboardEvent& keyboard_event) {
        if (keyboard_event.key == SDLK_ESCAPE) {
            quit();
        }
    }

protected:
    SDL_Window* window = nullptr;
};



int main(int /*argc*/, char** /*argv*/) {
    Yellowtail Game;
    Game.run();
    std::cout << "Ending yellowtail process." << std::endl;
    return 0;
}
