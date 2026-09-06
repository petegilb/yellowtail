//
// GPU half of the ocean: the compute passes that build the wave field and the clipmap draw that
// puts it on screen. The CPU half lives in OceanSimulation and is what physics samples.
//
// Owned by Engine alongside PointShadowRenderer, and driven the same way: given the frame's
// command buffer, records into it, owns no frame state of its own beyond the textures.
//

#ifndef YELLOWTAIL_OCEANRENDERER_H
#define YELLOWTAIL_OCEANRENDERER_H

#include <array>
#include <memory>

#include <SDL3/SDL.h>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include "OceanSettings.h"

namespace ytail {
    class Mesh;
    class ResourceManager;

    // Mirrors cbuffer OceanCompute in OceanCommon.hlsli. 16-byte rows throughout; the two nested
    // structs are already row-shaped, which is why they can sit at the front unmodified.
    struct OceanComputeUniform {
        ocean::SpectrumParams spectrum; // rows 0-2
        ocean::CascadeParams cascade;   // row 3

        Uint32 cascadeIndex = 0; // row 4
        Uint32 randomSeed = 0;
        Uint32 fftAxis = 0;
        Uint32 _pad0 = 0;

        float time = 0.0f; // row 5
        float deltaTime = 0.0f;
        float choppiness = 1.0f;
        float foamThreshold = 0.0f;

        float foamDecayRate = 0.0f; // row 6
        float foamGrowRate = 0.0f;
        float _pad1 = 0.0f;
        float _pad2 = 0.0f;
    };
    static_assert(sizeof(OceanComputeUniform) == 112, "must match cbuffer OceanCompute");

    // Mirrors cbuffer OceanVertex in Ocean.vert.hlsl.
    struct OceanVertexUniform {
        glm::mat4 viewProjection;
        // baseCellSize and texelsPerPatch are what let the vertex stage pick a mip: together with
        // the vertex's ring level they say how much of a cascade this vertex can actually resolve.
        glm::vec2 gridOrigin;    float baseCellSize = 1.0f; float texelsPerPatch = 1.0f;
        glm::vec4 patchSizes{0.0f};
        Uint32 cascadeCount = 0; Uint32 mipCount = 1; float seaLevel = 0.0f; float _pad0 = 0.0f;
    };
    static_assert(sizeof(OceanVertexUniform) == 112, "must match cbuffer OceanVertex");

    // Mirrors cbuffer OceanFragment in Ocean.frag.hlsl. Every vec3 is followed by the scalar that
    // shares its 16-byte row, so the pads are structural rather than decorative.
    struct OceanFragmentUniform {
        glm::vec3 cameraPosition; float skyYaw = 0.0f;
        glm::vec3 sunDirection;   float sunIntensity = 1.0f;
        glm::vec3 sunColor;       float roughness = 0.08f;
        glm::vec3 shallowColor;   float foamStrength = 1.0f;
        glm::vec3 deepColor;      float subsurfaceStrength = 1.0f;
        glm::vec3 skyTint;        float fresnelPower = 5.0f;
        glm::vec4 patchSizes{0.0f};
        glm::vec4 cascadeFadeDistance{0.0f};
        Uint32 cascadeCount = 0;  float seaLevel = 0.0f; float _pad0[2] = {};
    };
    static_assert(sizeof(OceanFragmentUniform) == 144, "must match cbuffer OceanFragment");

    class OceanRenderer {
    public:
        // FFT size. Fixed because OceanIFFT.comp.hlsl sizes its groupshared buffer and thread count
        // from OCEAN_FFT_SIZE at compile time; the two must stay equal.
        static constexpr int Resolution = 256;

        // Full mip chain down to 1x1, for the sampled outputs only. A clipmap vertex whose cell
        // spans many wavelengths cannot show them, only alias them, and that aliasing slides every
        // time the grid snaps to follow the camera. Sampling a mip matched to the cell size low
        // passes the wave field to what the vertex can carry, which is what holds the surface
        // still under a moving camera.
        static constexpr int MipCount = 9; // log2(256) + 1

        OceanRenderer(SDL_GPUDevice* inDevice, ResourceManager* inResources);
        ~OceanRenderer();

        OceanRenderer(const OceanRenderer&) = delete;
        OceanRenderer& operator=(const OceanRenderer&) = delete;

        // Records the compute passes that advance the wave field. Must be called outside a render
        // pass, so before the frame's SDL_BeginGPURenderPass.
        //
        // simulationTime is (tickNumber + renderAlpha) * FIXED_DT wrapped into the spectrum's loop
        // period: the same continuous function OceanSimulation samples, so the drawn surface and
        // the one physics feels coincide exactly at every tick boundary and interpolate between
        // them in step.
        //
        // deltaTime is real frame time, and only foam accumulation uses it.
        void simulate(SDL_GPUCommandBuffer* commandBuffer, const ocean::OceanSettings& settings,
                      float simulationTime, float deltaTime);

        // Draws the clipmap. Call inside the scene render pass. skyTexture is the panorama the
        // water reflects; the draw is skipped without one, since there would be nothing to bind
        // and nothing to reflect.
        void draw(SDL_GPURenderPass* renderPass, SDL_GPUCommandBuffer* commandBuffer,
                  const ocean::OceanSettings& settings, const glm::mat4& view,
                  const glm::mat4& projection, const glm::vec3& cameraPosition,
                  const glm::vec3& sunDirection, const glm::vec3& sunColor, float sunIntensity,
                  SDL_GPUTexture* skyTexture, const glm::vec3& skyTint, float skyYaw);

    private:
        // True once every texture and the mesh exist. A failed allocation disables the ocean
        // rather than crashing, the same way a failed pipeline does.
        [[nodiscard]] bool isReady() const;

        void createTextures();
        void releaseTextures();
        // Zero both foam textures. Foam decays from last frame's value, so the first frame would
        // otherwise decay whatever the allocation happened to contain.
        void clearFoam();
        // Rebuilds the clipmap vertex/index buffers. Only on a settings change, never per frame.
        void buildClipmap(const ocean::OceanSettings& settings);
        // h0(k). Re-run only when the spectrum moves, which is startup and sea-state changes.
        void generateInitialSpectrum(SDL_GPUCommandBuffer* commandBuffer,
                                     const ocean::OceanSettings& settings);

        // One read-write binding on a single array layer, which is all SDL_GPU allows: a compute
        // pass writes one slice at a time even though reads see the whole array.
        [[nodiscard]] static SDL_GPUStorageTextureReadWriteBinding layerBinding(SDL_GPUTexture* texture,
                                                                               Uint32 layer);

        SDL_GPUDevice* device = nullptr;
        ResourceManager* resources = nullptr;

        // h0(k) packed with conj(h0(-k)). Rebuilt on a spectrum change, read every frame.
        SDL_GPUTexture* initialSpectrum = nullptr;
        // IFFT ping-pong. Both carry two packed complex fields per texel, and both act as source
        // and destination across the two axis passes, so both need read and write usage.
        SDL_GPUTexture* spectrumPing = nullptr;
        SDL_GPUTexture* spectrumPong = nullptr;

        SDL_GPUTexture* displacement = nullptr; // Dx, Dy, Dz. Sampled by the vertex stage.
        SDL_GPUTexture* derivatives = nullptr;  // height slopes + jacobian. Sampled by the fragment stage.
        // Foam has to read last frame's value to decay it, so it ping-pongs rather than being
        // written in place.
        SDL_GPUTexture* foam[2] = { nullptr, nullptr };
        int foamWriteIndex = 0;

        SDL_GPUTextureFormat foamFormat = SDL_GPU_TEXTUREFORMAT_R16_FLOAT;

        // The clipmap, rebuilt only when its settings change.
        std::shared_ptr<Mesh> clipmap;
        int indexCount = 0;
        // What buildClipmap last built for, so a frame that changed nothing rebuilds nothing.
        float builtCellSize = 0.0f;
        int builtGridSize = 0;
        int builtRingCount = 0;
        float builtMorphStart = -1.0f;

        // What generateInitialSpectrum last ran for, same idea.
        ocean::SpectrumParams builtSpectrum{};
        std::array<ocean::CascadeParams, ocean::CascadeCount> builtCascades{};
        Uint32 builtSeed = 0;
        bool spectrumValid = false;
    };
} // ytail

#endif //YELLOWTAIL_OCEANRENDERER_H
