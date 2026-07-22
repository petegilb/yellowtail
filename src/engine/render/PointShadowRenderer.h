//
// Omnidirectional (cube-map) shadows for point lights.
//

#ifndef YELLOWTAIL_POINTSHADOWRENDERER_H
#define YELLOWTAIL_POINTSHADOWRENDERER_H

#include <SDL3/SDL.h>

namespace ytail {
    class ResourceManager;

    // Owns a cube-array depth texture: one "slot" (6 cube-array layers) per shadow-casting
    // point light, rendered from the light and sampled by the lit shader. The storage seam
    // for point shadows  a shadow atlas could later replace the cube array in here alone.
    // TODO change this to a shadow atlas like Doom (2016) -- they mentioned this in Realtime Rendering page 246
    class PointShadowRenderer {
    public:
        PointShadowRenderer(SDL_GPUDevice* inDevice, ResourceManager* inResources);
        ~PointShadowRenderer();

        // Create/recreate the texture on a cap or resolution change; no-op otherwise.
        void ensureTexture();

        // One-time clear of every face after (re)creation, so the map starts fully lit.
        void clearIfPending(SDL_GPUCommandBuffer* commandBuffer);

        [[nodiscard]] SDL_GPUTexture* getTexture() const { return cubeArray; }
        [[nodiscard]] int getMaxShadowedPoints() const { return maxShadowedPoints; }
        [[nodiscard]] int getFaceResolution() const { return faceResolution; }

        // Tunables (editor-exposed later).
        int maxShadowedPoints = 4;    // cube-map slots = hard cap on shadowed point lights
        int faceResolution    = 512;  // per-face square resolution

    private:
        SDL_GPUDevice*   device    = nullptr;
        ResourceManager* resources = nullptr;

        SDL_GPUTexture* cubeArray    = nullptr;
        int             currentRes   = 0;
        int             currentSlots = 0;
        bool            pendingClear = false;
    };
} // ytail

#endif //YELLOWTAIL_POINTSHADOWRENDERER_H
