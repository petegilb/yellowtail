//
// Created by Peter Gilbert on 8/14/26.
//

#include "OceanRenderer.h"

#include <algorithm>
#include <cmath>

#include "../Profiling.h"
#include "../managers/ResourceManager.h"
#include "../render/Mesh.h"

namespace ytail {
    namespace {
        // Matches the [numthreads(8, 8, 1)] on the per-texel compute shaders.
        constexpr Uint32 kTexelGroupSize = 8;
        constexpr Uint32 kCascadeLayers = static_cast<Uint32>(ocean::CascadeCount);

        SDL_GPUTexture* createArrayTexture(SDL_GPUDevice* device, const SDL_GPUTextureFormat format,
                                           const SDL_GPUTextureUsageFlags usage, const char* name,
                                           const int mipLevels = 1) {
            SDL_GPUTextureCreateInfo info = {};
            info.type = SDL_GPU_TEXTURETYPE_2D_ARRAY;
            info.format = format;
            info.usage = usage;
            info.width = OceanRenderer::Resolution;
            info.height = OceanRenderer::Resolution;
            info.layer_count_or_depth = kCascadeLayers;
            info.num_levels = static_cast<Uint32>(mipLevels);
            info.sample_count = SDL_GPU_SAMPLECOUNT_1;

            SDL_GPUTexture* texture = SDL_CreateGPUTexture(device, &info);
            if (texture == nullptr) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Ocean: failed to create %s: %s",
                             name, SDL_GetError());
            }
            return texture;
        }

        // Pack the cascade patch sizes / fade distances into a float4 for the draw uniforms.
        // CascadeCount is 3, so the fourth lane is spare.
        glm::vec4 packCascadeFloats(const std::array<float, ocean::CascadeCount>& values) {
            glm::vec4 packed{0.0f};
            for (int i = 0; i < ocean::CascadeCount; ++i) packed[i] = values[i];
            return packed;
        }

        // Quads across a clipmap level. Forced even so each ring's hole is exactly half of it,
        // which is what makes a ring's inner edge land on the previous level's outer edge.
        int normalizedGridSize(const ocean::OceanSettings& settings) {
            return std::max(4, settings.gridSize & ~1);
        }
    } // namespace

    OceanRenderer::OceanRenderer(SDL_GPUDevice* inDevice, ResourceManager* inResources)
        : device(inDevice), resources(inResources) {
        // A single-channel float storage texture is the natural shape for foam, but the format is
        // not universally supported for compute writes. Fall back rather than losing the ocean.
        constexpr SDL_GPUTextureUsageFlags foamUsage = SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_READ
                                                     | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE
                                                     | SDL_GPU_TEXTUREUSAGE_SAMPLER
                                                     | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
        if (!SDL_GPUTextureSupportsFormat(device, SDL_GPU_TEXTUREFORMAT_R16_FLOAT,
                                          SDL_GPU_TEXTURETYPE_2D_ARRAY, foamUsage)) {
            foamFormat = SDL_GPU_TEXTUREFORMAT_R32_FLOAT;
        }

        createTextures();
    }

    OceanRenderer::~OceanRenderer() {
        releaseTextures();
    }

    void OceanRenderer::createTextures() {
        // The spectrum stages need full float precision: h0 spans many orders of magnitude across
        // the band, and half floats lose the smallest waves entirely.
        constexpr SDL_GPUTextureUsageFlags spectrumUsage = SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_READ
                                                         | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
        initialSpectrum = createArrayTexture(device, SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT,
                                             spectrumUsage, "initialSpectrum");
        // Both take a turn as source and as destination across the two IFFT axis passes.
        spectrumPing = createArrayTexture(device, SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT,
                                          spectrumUsage, "spectrumPing");
        spectrumPong = createArrayTexture(device, SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT,
                                          spectrumUsage, "spectrumPong");

        // The outputs are only ever written by compute and read by the draw, so half floats are
        // plenty and halve the bandwidth the vertex stage pays.
        //
        // COLOR_TARGET is there because SDL generates mips by blitting, and mips are what keep a
        // coarse clipmap ring from aliasing wave detail it cannot represent.
        constexpr SDL_GPUTextureUsageFlags outputUsage = SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE
                                                       | SDL_GPU_TEXTUREUSAGE_SAMPLER
                                                       | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
        displacement = createArrayTexture(device, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT,
                                          outputUsage, "displacement", MipCount);
        derivatives = createArrayTexture(device, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT,
                                         outputUsage, "derivatives", MipCount);

        constexpr SDL_GPUTextureUsageFlags foamUsage = SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_READ
                                                     | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE
                                                     | SDL_GPU_TEXTUREUSAGE_SAMPLER
                                                     | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
        foam[0] = createArrayTexture(device, foamFormat, foamUsage, "foam0", MipCount);
        foam[1] = createArrayTexture(device, foamFormat, foamUsage, "foam1", MipCount);
        clearFoam();
    }

    void OceanRenderer::clearFoam() {
        if (foam[0] == nullptr || foam[1] == nullptr) return;

        // Foam decays from whatever it held last frame, so the very first frame would decay
        // undefined memory. A NaN there would stick permanently, since NaN survives every
        // multiply and max that follows.
        const Uint32 texelBytes = foamFormat == SDL_GPU_TEXTUREFORMAT_R16_FLOAT ? 2 : 4;
        const Uint32 layerBytes = Resolution * Resolution * texelBytes;

        SDL_GPUTransferBufferCreateInfo transferInfo = {};
        transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        transferInfo.size = layerBytes;
        SDL_GPUTransferBuffer* transferBuffer = SDL_CreateGPUTransferBuffer(device, &transferInfo);
        if (transferBuffer == nullptr) return;

        void* mapped = SDL_MapGPUTransferBuffer(device, transferBuffer, false);
        SDL_memset(mapped, 0, layerBytes);
        SDL_UnmapGPUTransferBuffer(device, transferBuffer);

        SDL_GPUCommandBuffer* uploadCommands = SDL_AcquireGPUCommandBuffer(device);
        SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(uploadCommands);
        for (SDL_GPUTexture* texture : foam) {
            for (Uint32 layer = 0; layer < kCascadeLayers; ++layer) {
                SDL_GPUTextureTransferInfo source = {};
                source.transfer_buffer = transferBuffer;
                source.offset = 0;

                SDL_GPUTextureRegion destination = {};
                destination.texture = texture;
                destination.layer = layer;
                destination.w = Resolution;
                destination.h = Resolution;
                destination.d = 1;

                SDL_UploadToGPUTexture(copyPass, &source, &destination, false);
            }
        }
        SDL_EndGPUCopyPass(copyPass);
        SDL_SubmitGPUCommandBuffer(uploadCommands);
        SDL_ReleaseGPUTransferBuffer(device, transferBuffer);
    }

    void OceanRenderer::releaseTextures() {
        for (SDL_GPUTexture* texture : { initialSpectrum, spectrumPing, spectrumPong,
                                         displacement, derivatives, foam[0], foam[1] }) {
            if (texture) SDL_ReleaseGPUTexture(device, texture);
        }
        initialSpectrum = spectrumPing = spectrumPong = nullptr;
        displacement = derivatives = nullptr;
        foam[0] = foam[1] = nullptr;
    }

    bool OceanRenderer::isReady() const {
        return initialSpectrum && spectrumPing && spectrumPong
            && displacement && derivatives && foam[0] && foam[1];
    }

    SDL_GPUStorageTextureReadWriteBinding OceanRenderer::layerBinding(SDL_GPUTexture* texture,
                                                                     const Uint32 layer) {
        SDL_GPUStorageTextureReadWriteBinding binding = {};
        binding.texture = texture;
        binding.mip_level = 0;
        binding.layer = layer;
        return binding;
    }

    // ---- the clipmap ----

    void OceanRenderer::buildClipmap(const ocean::OceanSettings& settings) {
        ZoneScoped;
        const int gridSize = normalizedGridSize(settings);
        const int ringCount = std::max(1, settings.ringCount);
        const int half = gridSize / 2;
        const int verticesPerSide = gridSize + 1;

        std::vector<Vertex> vertices;
        std::vector<Uint32> indices;
        vertices.reserve(static_cast<size_t>(verticesPerSide) * verticesPerSide * ringCount);

        for (int level = 0; level < ringCount; ++level) {
            const float cellSize = settings.baseCellSize * static_cast<float>(1 << level);
            const auto levelBase = static_cast<Uint32>(vertices.size());
            // The outermost level has nothing coarser to blend into, so it never morphs.
            const bool morphs = level < ringCount - 1;

            // A full square grid every level, with the hole cut out of the indices rather than the
            // vertices. The unreferenced centre vertices cost memory and nothing else, since the
            // GPU only shades what the index buffer names.
            for (int row = 0; row <= gridSize; ++row) {
                for (int column = 0; column <= gridSize; ++column) {
                    const int indexX = column - half;
                    const int indexZ = row - half;

                    const glm::vec2 position{ static_cast<float>(indexX) * cellSize,
                                              static_cast<float>(indexZ) * cellSize };

                    // Collapsing an odd-indexed vertex onto its even neighbour puts it on the
                    // coarser level's edge, which is exactly where that level would have drawn it.
                    // The triangle degenerates, and that is the seam closing rather than a bug.
                    const glm::vec2 parent{ static_cast<float>(indexX - (indexX & 1)) * cellSize,
                                            static_cast<float>(indexZ - (indexZ & 1)) * cellSize };

                    // Chebyshev distance, because a clipmap level is a square and its boundary is
                    // where max(|x|, |z|) reaches the half-extent, not where the radius does.
                    const float extent = static_cast<float>(half) * cellSize;
                    const float distance = std::max(std::abs(position.x), std::abs(position.y)) / extent;
                    const float weight = morphs
                        ? std::clamp((distance - settings.morphStart)
                                     / std::max(1.0f - settings.morphStart, 1e-4f), 0.0f, 1.0f)
                        : 0.0f;

                    vertices.push_back(Vertex{
                        glm::vec3(position.x, 0.0f, position.y),
                        glm::vec3(parent.x, 0.0f, parent.y),
                        glm::vec2(weight, static_cast<float>(level))
                    });
                }
            }

            // Level 0 is solid; every level above it is a ring with its middle left to the level
            // below. The hole is the central half of the grid in each axis, which at double the
            // cell size lands exactly on the previous level's outer extent.
            const bool hasHole = level > 0;
            const int holeMin = half / 2;
            const int holeMax = half + half / 2;

            for (int row = 0; row < gridSize; ++row) {
                for (int column = 0; column < gridSize; ++column) {
                    const bool insideHole = hasHole
                                         && column >= holeMin && column < holeMax
                                         && row >= holeMin && row < holeMax;
                    if (insideHole) continue;

                    const Uint32 topLeft = levelBase
                        + static_cast<Uint32>(row * verticesPerSide + column);
                    const Uint32 topRight = topLeft + 1;
                    const Uint32 bottomLeft = topLeft + static_cast<Uint32>(verticesPerSide);
                    const Uint32 bottomRight = bottomLeft + 1;

                    // Counter-clockwise seen from above, matching the engine's front-face winding
                    // even though the ocean pipeline does not cull.
                    indices.push_back(topLeft);
                    indices.push_back(bottomLeft);
                    indices.push_back(topRight);

                    indices.push_back(topRight);
                    indices.push_back(bottomLeft);
                    indices.push_back(bottomRight);
                }
            }
        }

        indexCount = static_cast<int>(indices.size());
        std::vector<Submesh> submeshes{ Submesh{ 0, static_cast<Uint32>(indexCount), 0 } };
        clipmap = resources->uploadMesh("ocean:clipmap", vertices, indices, std::move(submeshes));

        builtCellSize = settings.baseCellSize;
        builtGridSize = gridSize;
        builtRingCount = ringCount;
        builtMorphStart = settings.morphStart;

        SDL_Log("Ocean: built clipmap, %zu vertices, %d triangles, radius %.0fm",
                vertices.size(), indexCount / 3,
                static_cast<double>(settings.baseCellSize * static_cast<float>(half)
                                    * static_cast<float>(1 << (ringCount - 1))));
    }

    // ---- the simulation ----

    void OceanRenderer::generateInitialSpectrum(SDL_GPUCommandBuffer* commandBuffer,
                                                const ocean::OceanSettings& settings) {
        ZoneScoped;
        SDL_GPUComputePipeline* pipeline =
            resources->getComputePipeline(ComputePipelineType::OceanInitialSpectrum);
        if (pipeline == nullptr) return;

        constexpr Uint32 groups = OceanRenderer::Resolution / kTexelGroupSize;

        for (Uint32 cascade = 0; cascade < kCascadeLayers; ++cascade) {
            OceanComputeUniform uniform{};
            uniform.spectrum = settings.spectrum;
            uniform.cascade = settings.cascades[cascade];
            uniform.cascadeIndex = cascade;
            uniform.randomSeed = settings.randomSeed;
            SDL_PushGPUComputeUniformData(commandBuffer, 0, &uniform, sizeof(uniform));

            const SDL_GPUStorageTextureReadWriteBinding target = layerBinding(initialSpectrum, cascade);
            SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(commandBuffer, &target, 1, nullptr, 0);
            SDL_BindGPUComputePipeline(pass, pipeline);
            SDL_DispatchGPUCompute(pass, groups, groups, 1);
            SDL_EndGPUComputePass(pass);
        }

        builtSpectrum = settings.spectrum;
        builtCascades = settings.cascades;
        builtSeed = settings.randomSeed;
        spectrumValid = true;
    }

    void OceanRenderer::simulate(SDL_GPUCommandBuffer* commandBuffer,
                                 const ocean::OceanSettings& settings,
                                 const float simulationTime, const float deltaTime) {
        ZoneScoped;
        if (!isReady()) return;

        if (clipmap == nullptr || builtCellSize != settings.baseCellSize
            || builtGridSize != normalizedGridSize(settings)
            || builtRingCount != std::max(1, settings.ringCount)
            || builtMorphStart != settings.morphStart) {
            buildClipmap(settings);
        }

        // The gaussians are seeded per bin and only their magnitudes rescale, so a spectrum change
        // crossfades continuously instead of jumping to a different sea.
        const bool spectrumChanged = !spectrumValid
            || SDL_memcmp(&builtSpectrum, &settings.spectrum, sizeof(ocean::SpectrumParams)) != 0
            || SDL_memcmp(builtCascades.data(), settings.cascades.data(),
                          sizeof(ocean::CascadeParams) * ocean::CascadeCount) != 0
            || builtSeed != settings.randomSeed;
        if (spectrumChanged) {
            generateInitialSpectrum(commandBuffer, settings);
        }

        SDL_GPUComputePipeline* evolvePipeline =
            resources->getComputePipeline(ComputePipelineType::OceanTimeEvolve);
        SDL_GPUComputePipeline* fftPipeline =
            resources->getComputePipeline(ComputePipelineType::OceanIFFT);
        SDL_GPUComputePipeline* derivativePipeline =
            resources->getComputePipeline(ComputePipelineType::OceanDerivatives);
        if (!evolvePipeline || !fftPipeline || !derivativePipeline) return;

        constexpr Uint32 texelGroups = OceanRenderer::Resolution / kTexelGroupSize;
        const int foamRead = foamWriteIndex;
        const int foamWrite = 1 - foamWriteIndex;

        OceanComputeUniform uniform{};
        uniform.spectrum = settings.spectrum;
        uniform.randomSeed = settings.randomSeed;
        uniform.time = simulationTime;
        uniform.deltaTime = deltaTime;
        uniform.choppiness = settings.choppiness;
        uniform.foamThreshold = settings.foamThreshold;
        uniform.foamDecayRate = settings.foamDecayRate;
        uniform.foamGrowRate = settings.foamGrowRate;

        for (Uint32 cascade = 0; cascade < kCascadeLayers; ++cascade) {
            uniform.cascade = settings.cascades[cascade];
            uniform.cascadeIndex = cascade;

            // Every stage reads what the previous one wrote, and a compute pass gives no ordering
            // between its own dispatches. So each stage is its own pass: ending one is the only
            // barrier SDL_GPU offers.

            { // h(k, t) into the packed complex pair the IFFT consumes
                uniform.fftAxis = 0;
                SDL_PushGPUComputeUniformData(commandBuffer, 0, &uniform, sizeof(uniform));

                const SDL_GPUStorageTextureReadWriteBinding target = layerBinding(spectrumPing, cascade);
                SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(commandBuffer, &target, 1, nullptr, 0);
                SDL_BindGPUComputePipeline(pass, evolvePipeline);
                SDL_BindGPUComputeStorageTextures(pass, 0, &initialSpectrum, 1);
                SDL_DispatchGPUCompute(pass, texelGroups, texelGroups, 1);
                SDL_EndGPUComputePass(pass);
            }

            { // rows
                uniform.fftAxis = 0;
                SDL_PushGPUComputeUniformData(commandBuffer, 0, &uniform, sizeof(uniform));

                const SDL_GPUStorageTextureReadWriteBinding target = layerBinding(spectrumPong, cascade);
                SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(commandBuffer, &target, 1, nullptr, 0);
                SDL_BindGPUComputePipeline(pass, fftPipeline);
                SDL_BindGPUComputeStorageTextures(pass, 0, &spectrumPing, 1);
                // One group per row; each runs all eight stages in groupshared memory.
                SDL_DispatchGPUCompute(pass, 1, OceanRenderer::Resolution, 1);
                SDL_EndGPUComputePass(pass);
            }

            { // columns, back into ping
                uniform.fftAxis = 1;
                SDL_PushGPUComputeUniformData(commandBuffer, 0, &uniform, sizeof(uniform));

                const SDL_GPUStorageTextureReadWriteBinding target = layerBinding(spectrumPing, cascade);
                SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(commandBuffer, &target, 1, nullptr, 0);
                SDL_BindGPUComputePipeline(pass, fftPipeline);
                SDL_BindGPUComputeStorageTextures(pass, 0, &spectrumPong, 1);
                SDL_DispatchGPUCompute(pass, 1, OceanRenderer::Resolution, 1);
                SDL_EndGPUComputePass(pass);
            }

            { // unpack into displacement, difference for slopes and the jacobian, accumulate foam
                SDL_PushGPUComputeUniformData(commandBuffer, 0, &uniform, sizeof(uniform));

                const SDL_GPUStorageTextureReadWriteBinding targets[3] = {
                    layerBinding(displacement, cascade),
                    layerBinding(derivatives, cascade),
                    layerBinding(foam[foamWrite], cascade),
                };
                SDL_GPUTexture* sources[2] = { spectrumPing, foam[foamRead] };

                SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(commandBuffer, targets, 3, nullptr, 0);
                SDL_BindGPUComputePipeline(pass, derivativePipeline);
                SDL_BindGPUComputeStorageTextures(pass, 0, sources, 2);
                SDL_DispatchGPUCompute(pass, texelGroups, texelGroups, 1);
                SDL_EndGPUComputePass(pass);
            }
        }

        foamWriteIndex = foamWrite;

        // Outside every pass, which is why this sits after the cascade loop rather than inside it.
        //
        // The mips are what stop the surface crawling under a moving camera. An outer clipmap ring
        // has cells tens of metres across and cannot represent a two metre wave; without a mip it
        // point-samples one, and every time the grid snaps forward it point-samples a different
        // one. Sampling a mip matched to the cell size means the ring sees only the part of the
        // wave field it can actually carry, which does not change as the grid slides.
        SDL_GenerateMipmapsForGPUTexture(commandBuffer, displacement);
        SDL_GenerateMipmapsForGPUTexture(commandBuffer, derivatives);
        SDL_GenerateMipmapsForGPUTexture(commandBuffer, foam[foamWrite]);
    }

    // ---- the draw ----

    void OceanRenderer::draw(SDL_GPURenderPass* renderPass, SDL_GPUCommandBuffer* commandBuffer,
                             const ocean::OceanSettings& settings, const glm::mat4& view,
                             const glm::mat4& projection, const glm::vec3& cameraPosition,
                             const glm::vec3& sunDirection, const glm::vec3& sunColor,
                             const float sunIntensity, SDL_GPUTexture* skyTexture,
                             const glm::vec3& skyTint, const float skyYaw) {
        ZoneScoped;
        if (!isReady() || clipmap == nullptr || indexCount == 0) return;

        SDL_GPUGraphicsPipeline* pipeline = resources->getPipeline(PipelineType::Ocean);
        if (pipeline == nullptr) return;

        // The sky panorama is the ocean's only reflection source, so without one there is nothing
        // to bind at slot 2 and binding null is invalid. Skip the whole draw rather than reading
        // garbage; a scene with an ocean and no sky is a setup mistake worth noticing.
        if (skyTexture == nullptr) return;

        std::array<float, ocean::CascadeCount> patchSizes{};
        for (int i = 0; i < ocean::CascadeCount; ++i) patchSizes[i] = settings.cascades[i].patchSize;

        // Snap the grid to the finest cell, so the core's vertices always land on the same
        // world-space lattice and only ever get added and removed at the edges rather than sliding
        // along the surface. The coarser rings still shift by up to one fine cell, which is what
        // the mip selection in the vertex shader exists to absorb.
        const float snap = std::max(settings.baseCellSize, 1e-3f);
        const glm::vec2 gridOrigin{ std::floor(cameraPosition.x / snap) * snap,
                                    std::floor(cameraPosition.z / snap) * snap };

        OceanVertexUniform vertexUniform{};
        vertexUniform.viewProjection = projection * view;
        // Applied in the shader rather than baked into the mesh, so moving it costs nothing.
        vertexUniform.seaLevel = settings.seaLevel;
        vertexUniform.gridOrigin = gridOrigin;
        vertexUniform.baseCellSize = settings.baseCellSize;
        vertexUniform.texelsPerPatch = static_cast<float>(Resolution);
        vertexUniform.patchSizes = packCascadeFloats(patchSizes);
        vertexUniform.cascadeCount = kCascadeLayers;
        vertexUniform.mipCount = MipCount;

        OceanFragmentUniform fragmentUniform{};
        fragmentUniform.cameraPosition = cameraPosition;
        fragmentUniform.skyYaw = skyYaw;
        fragmentUniform.sunDirection = sunDirection;
        fragmentUniform.sunIntensity = sunIntensity;
        fragmentUniform.sunColor = sunColor;
        fragmentUniform.roughness = settings.roughness;
        fragmentUniform.shallowColor = settings.shallowColor;
        fragmentUniform.foamStrength = settings.foamStrength;
        fragmentUniform.deepColor = settings.deepColor;
        fragmentUniform.subsurfaceStrength = settings.subsurfaceStrength;
        fragmentUniform.skyTint = skyTint;
        fragmentUniform.fresnelPower = settings.fresnelPower;
        fragmentUniform.patchSizes = vertexUniform.patchSizes;
        fragmentUniform.cascadeFadeDistance = packCascadeFloats(settings.cascadeFadeDistance);
        fragmentUniform.cascadeCount = kCascadeLayers;
        fragmentUniform.seaLevel = settings.seaLevel;

        SDL_BindGPUGraphicsPipeline(renderPass, pipeline);

        SDL_GPUBufferBinding vertexBinding{ .buffer = clipmap->vertexBuffer, .offset = 0 };
        SDL_GPUBufferBinding indexBinding{ .buffer = clipmap->indexBuffer, .offset = 0 };
        SDL_BindGPUVertexBuffers(renderPass, 0, &vertexBinding, 1);
        SDL_BindGPUIndexBuffer(renderPass, &indexBinding, clipmap->indexSize);

        // The patch tiles, so every ocean sampler wraps. Anything clamping would smear the tile's
        // edge row across the whole horizon.
        SDL_GPUSampler* wrapSampler = resources->getSampler(SamplerType::LinearWrap);
        const SDL_GPUTextureSamplerBinding vertexTextures[1] = {
            { .texture = displacement, .sampler = wrapSampler },
        };
        SDL_BindGPUVertexSamplers(renderPass, 0, vertexTextures, 1);

        const SDL_GPUTextureSamplerBinding fragmentTextures[3] = {
            { .texture = derivatives, .sampler = wrapSampler },
            { .texture = foam[foamWriteIndex], .sampler = wrapSampler },
            // The panorama wraps at its seam behind the camera, so it needs the wrapping sampler
            // for the same reason the sky pass does.
            { .texture = skyTexture, .sampler = wrapSampler },
        };
        SDL_BindGPUFragmentSamplers(renderPass, 0, fragmentTextures, 3);

        SDL_PushGPUVertexUniformData(commandBuffer, 0, &vertexUniform, sizeof(vertexUniform));
        SDL_PushGPUFragmentUniformData(commandBuffer, 0, &fragmentUniform, sizeof(fragmentUniform));

        SDL_DrawGPUIndexedPrimitives(renderPass, static_cast<Uint32>(indexCount), 1, 0, 0, 0);
    }
} // ytail
