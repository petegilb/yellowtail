//
// Created by Peter Gilbert on 6/28/26.
//

#ifndef YELLOWTAIL_TEXTURE_H
#define YELLOWTAIL_TEXTURE_H

#include <string>
#include <SDL3/SDL.h>

namespace ytail {
    // SDL_GPUTexture wrapper class so that we can have a destructor
    class Texture {
    public:
        Texture(SDL_GPUDevice* d, SDL_GPUTexture* t, Uint32 w, Uint32 h)
            : device(d), tex(t), width(w), height(h) {}
        ~Texture() { if (tex) SDL_ReleaseGPUTexture(device, tex); }   // RAII

        Texture(const Texture&) = delete;             // non-copyable (owns a GPU handle)
        Texture& operator=(const Texture&) = delete;

        SDL_GPUTexture* handle() const { return tex; }
        // The file this texture came from (empty for solid-color textures), used when saving.
        std::string sourcePath;
        Uint32 width = 0, height = 0;
    private:
        SDL_GPUDevice*  device = nullptr;
        SDL_GPUTexture* tex    = nullptr;
    };
} // ytail


#endif //YELLOWTAIL_TEXTURE_H