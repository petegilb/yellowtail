//
// Omnidirectional (cube-map) shadows for point lights.
//

#ifndef YELLOWTAIL_POINTSHADOWRENDERER_H
#define YELLOWTAIL_POINTSHADOWRENDERER_H

#include <memory>
#include <unordered_map>
#include <vector>
#include <SDL3/SDL.h>

namespace ytail {
    class ResourceManager;
    class Entity;

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

        // Render the 6 cube faces for each shadow-casting point light (up to the cap) and record
        // its slot. Call once per frame, before the scene pass. Begins its own render passes.
        void generate(SDL_GPUCommandBuffer* commandBuffer,
                      const std::vector<Entity>& entities);

        // Clear this frame's slot assignments and stats. Call when generation is skipped.
        void reset();

        // Cube-array slice for a light entity this frame, or -1 if it isn't shadowed.
        [[nodiscard]] int slotForLightId(Uint32 id) const {
            const auto it = slotForLight.find(id);
            return it == slotForLight.end() ? -1 : it->second;
        }

        [[nodiscard]] SDL_GPUTexture* getTexture() const { return cubeArray; }
        [[nodiscard]] int getMaxShadowedPoints() const { return maxShadowedPoints; }
        [[nodiscard]] int getFaceResolution() const { return faceResolution; }

        // Stats from the last generate(), for verifying radius culling (no visual effect on its own).
        [[nodiscard]] int getActiveLights() const { return lastActiveLights; }   // lights providing a shadow
        [[nodiscard]] int getCasterDraws() const { return lastCasterDraws; }     // submesh draws across all faces
        [[nodiscard]] int getCulledCasters() const { return lastCulled; }        // (light, caster) pairs radius-rejected
        [[nodiscard]] int getSlotsRegenerated() const { return lastRegenerated; }// slots that re-rendered (0 = all cached)

        // Tunables (editor-exposed later).
        int maxShadowedPoints    = 4;   // cube-map slots = hard cap on shadowed point lights
        int faceResolution       = 512; // per-face square resolution
        int refreshBudgetPerFrame = 4;  // max slots re-rendered per frame; caching skips the rest

    private:
        SDL_GPUDevice*   device    = nullptr;
        ResourceManager* resources = nullptr;

        SDL_GPUTexture* cubeArray    = nullptr;
        int             currentRes   = 0;
        int             currentSlots = 0;
        bool            cubeRecreated = false; // texture was (re)created; cached layers are gone

        // Per-frame stats (see getters).
        int lastActiveLights = 0;
        int lastCasterDraws  = 0;
        int lastCulled       = 0;
        int lastRegenerated  = 0;

        // Persistent per-slot cache state. A light keeps its slot across frames so its cube layers
        // stay valid; a slot re-renders only when its input signature changes.
        struct SlotState {
            Uint32 owner     = 0;      // light entity id, 0 = free
            size_t signature = 0;      // hash of the inputs that produced the cached cube
            bool   valid     = false;  // rendered at least once (safe to sample)
        };
        std::vector<SlotState> slots;
        int refreshCursor = 0;         // round-robin start for the per-frame refresh budget

        // Light entity id -> cube-array slot for valid (sampleable) slots, rebuilt every generate().
        std::unordered_map<Uint32, int> slotForLight;
    };
} // ytail

#endif //YELLOWTAIL_POINTSHADOWRENDERER_H
