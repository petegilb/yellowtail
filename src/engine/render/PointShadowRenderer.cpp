//
// Omnidirectional (cube-map) shadows for point lights.
//

#include "PointShadowRenderer.h"

#include <algorithm>
#include <vector>
#include <glm/gtc/matrix_transform.hpp>

#include "Mesh.h"
#include "Material.h"
#include "../Entity.h"
#include "../components/LightComponent.h"
#include "../components/RenderComponent.h"
#include "../components/TransformComponent.h"
#include "../managers/ResourceManager.h"

namespace ytail {
    static constexpr int kFacesPerCube = 6;                // one slot = 6 cube-array layers
    static constexpr int kMaxSlots = 256 / kFacesPerCube;  // layer index (slot*6+face) must fit a Uint8
    static constexpr float kFaceFovDegrees = 90.0f;
    static constexpr float kFaceNearPlane  = 0.05f;
    // FNV-1a offset basis + golden-ratio mix, for the change-detection hash.
    static constexpr size_t kFnvOffsetBasis = 1469598103934665603ULL;
    static constexpr size_t kHashMix        = 0x9e3779b97f4a7c15ULL;

    // D3D-style cube face order: +X, -X, +Y, -Y, +Z, -Z. Ups chosen to match the hardware's
    // sampled face for a given direction.
    static const glm::vec3 kFaceDir[kFacesPerCube] = {
        { 1, 0, 0}, {-1, 0, 0}, { 0, 1, 0}, { 0,-1, 0}, { 0, 0, 1}, { 0, 0,-1}
    };
    static const glm::vec3 kFaceUp[kFacesPerCube] = {
        { 0,-1, 0}, { 0,-1, 0}, { 0, 0, 1}, { 0, 0,-1}, { 0,-1, 0}, { 0,-1, 0}
    };

    // Fold a value into a slot's change signature (boost::hash_combine style). A slot re-renders
    // when its signature changes: light version + radius, and each in-range caster's id, world
    // version, and mesh.
    static void hashCombine(size_t& h, Uint64 v) {
        h ^= static_cast<size_t>(v) + kHashMix + (h << 6) + (h >> 2);
    }

    PointShadowRenderer::PointShadowRenderer(SDL_GPUDevice* inDevice, ResourceManager* inResources)
        : device(inDevice), resources(inResources) {}

    PointShadowRenderer::~PointShadowRenderer() {
        if (device && cubeArray) SDL_ReleaseGPUTexture(device, cubeArray);
    }

    void PointShadowRenderer::reset() {
        slotForLight.clear();
        lastActiveLights = 0;
        lastCasterDraws  = 0;
        lastCulled       = 0;
        lastRegenerated  = 0;
    }

    void PointShadowRenderer::ensureTexture() {
        maxShadowedPoints = std::clamp(maxShadowedPoints, 1, kMaxSlots);
        // currentRes/currentSlots record the last attempt (success OR failure), so a persistent
        // creation failure doesn't re-attempt and re-log every frame.
        if (currentRes == faceResolution && currentSlots == maxShadowedPoints) return;
        if (cubeArray) SDL_ReleaseGPUTexture(device, cubeArray);
        cubeArray = nullptr;

        SDL_GPUTextureCreateInfo info = {};
        info.type   = SDL_GPU_TEXTURETYPE_CUBE_ARRAY;
        info.format = resources->getShadowMapFormat();
        // Rendered to by the shadow pass, sampled by the lit shader.
        info.usage  = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        info.width  = static_cast<Uint32>(faceResolution);
        info.height = static_cast<Uint32>(faceResolution);
        info.layer_count_or_depth = static_cast<Uint32>(kFacesPerCube * maxShadowedPoints);
        info.num_levels = 1;

        cubeArray    = SDL_CreateGPUTexture(device, &info);
        currentRes   = faceResolution;      // record the attempt regardless of outcome
        currentSlots = maxShadowedPoints;
        cubeRecreated = true;               // cached layers are gone; generate() invalidates slots
        if (cubeArray == nullptr) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create point-shadow cube array: %s", SDL_GetError());
        }
    }

    void PointShadowRenderer::generate(SDL_GPUCommandBuffer* commandBuffer,
                                       const std::unordered_map<Uint32, std::unique_ptr<Entity>>& entities) {
        reset();                        // clears slotForLight + stats; slot cache (slots) persists
        if (cubeArray == nullptr) return;
        SDL_GPUGraphicsPipeline* pipeline = resources->getPipeline(PipelineType::PointShadowDepth);
        if (pipeline == nullptr) return;

        // Reconcile the slot table with the cap / a texture recreation (both drop the cache).
        if (static_cast<int>(slots.size()) != maxShadowedPoints) slots.assign(maxShadowedPoints, SlotState{});
        if (cubeRecreated) { for (SlotState& s : slots) s.valid = false; cubeRecreated = false; }

        struct CasterInfo { const Mesh* mesh; glm::mat4 model; glm::vec3 center; float radius; Uint32 id; Uint64 version; };
        // A shadow-casting point light, its in-range casters, and a signature of everything the
        // cube depends on (candidates with no visible caster are dropped before slot assignment).
        struct Candidate {
            Uint32 id;
            glm::vec3 lightPos;
            float farPlane;
            Uint64 lightVersion;
            std::vector<const CasterInfo*> visible;
            size_t signature = 0;
            int slotIndex = -1;
        };

        // Shadow-casting point lights first (cheap component checks), so a scene without any skips
        // the per-caster gathering below entirely.
        std::vector<Candidate> candidates;
        for (const auto& [id, entity] : entities) {
            if (entity == nullptr) continue;
            const auto* light = entity->getComponent<LightComponent>();
            const auto* xform = entity->getComponent<TransformComponent>();
            if (light == nullptr || xform == nullptr) continue;
            if (light->type != LightType::Point || !light->castsShadows) continue;

            const float farPlane = light->attenuation;
            if (farPlane <= kFaceNearPlane) continue; // degenerate radius; light stays unshadowed

            candidates.push_back({ id, glm::vec3(xform->worldMatrix()[3]), farPlane,
                                   xform->getWorldVersion() });
        }
        if (candidates.empty()) return; // stale slot owners get reconciled next time a light exists

        // World transform + bounding sphere for every shadow caster, computed once for the frame.
        std::vector<CasterInfo> allCasters;
        for (const auto& [castId, caster] : entities) {
            if (caster == nullptr) continue;
            const auto* renderComponent = caster->getComponent<RenderComponent>();
            const auto* casterXform = caster->getComponent<TransformComponent>();
            if (renderComponent == nullptr || casterXform == nullptr) continue;
            if (!renderComponent->castsShadow) continue;
            const auto& mesh = renderComponent->mesh;
            if (!mesh) continue;

            const glm::mat4 model = casterXform->worldMatrix();
            const glm::vec3 center = glm::vec3(model * glm::vec4((mesh->aabbMin + mesh->aabbMax) * 0.5f, 1.0f));
            const float localRadius = glm::length(mesh->aabbMax - mesh->aabbMin) * 0.5f;
            // Rotation preserves column length, so the longest column is the max axis scale.
            const float maxScale = glm::max(glm::length(glm::vec3(model[0])),
                                   glm::max(glm::length(glm::vec3(model[1])),
                                            glm::length(glm::vec3(model[2]))));
            allCasters.push_back({ mesh.get(), model, center, localRadius * maxScale,
                                   castId, casterXform->getWorldVersion() });
        }

        // Cull casters per light and build each candidate's signature. Caster contributions are
        // summed (commutative), so an entity-map rehash that reorders iteration doesn't change the
        // signature and spuriously re-render every slot.
        for (Candidate& cand : candidates) {
            size_t sig = kFnvOffsetBasis;
            hashCombine(sig, cand.lightVersion);  // light moved
            Uint32 farBits; SDL_memcpy(&farBits, &cand.farPlane, sizeof(farBits));
            hashCombine(sig, farBits);            // light radius changed
            size_t casterSum = 0;
            for (const CasterInfo& casterInfo : allCasters) {
                if (glm::length(casterInfo.center - cand.lightPos) > cand.farPlane + casterInfo.radius) {
                    ++lastCulled;
                    continue;
                }
                cand.visible.push_back(&casterInfo);
                size_t casterHash = kFnvOffsetBasis;
                hashCombine(casterHash, casterInfo.id);
                hashCombine(casterHash, casterInfo.version);
                hashCombine(casterHash, casterInfo.mesh->uid); // stable id; a pointer could be recycled
                casterSum += casterHash;
            }
            hashCombine(sig, casterSum);
            cand.signature = sig;
        }
        // No occluders in range: no shadow, no slot.
        std::erase_if(candidates, [](const Candidate& c) { return c.visible.empty(); });
        if (candidates.empty()) return;

        // --- Slot reconciliation (persistent ownership so caches survive across frames) ---
        auto isCandidate = [&](Uint32 owner) {
            for (const Candidate& c : candidates) if (c.id == owner) return true;
            return false;
        };
        // Free slots whose owner is no longer a candidate.
        for (SlotState& s : slots) {
            if (s.owner != 0 && !isCandidate(s.owner)) { s.owner = 0; s.valid = false; }
        }
        // Keep existing owners; assign free slots to candidates that don't have one yet.
        for (Candidate& c : candidates) {
            for (int i = 0; i < static_cast<int>(slots.size()); ++i) {
                if (slots[i].owner == c.id) { c.slotIndex = i; break; }
            }
        }
        for (Candidate& c : candidates) {
            if (c.slotIndex >= 0) continue;
            for (int i = 0; i < static_cast<int>(slots.size()); ++i) {
                if (slots[i].owner == 0) { slots[i].owner = c.id; slots[i].valid = false; c.slotIndex = i; break; }
            }
            // no free slot -> c.slotIndex stays -1 (over the cap; no shadow this frame)
        }

        // --- Decide which slots to re-render, honoring the per-frame budget ---
        // Never-generated (invalid) dirty slots render first so new lights don't sample garbage;
        // changed-but-valid slots are budgeted and rotated so no slot starves.
        std::vector<int> renderNow;   // candidate indices to regenerate
        std::vector<int> changed;     // candidate indices dirty-but-already-valid
        for (int ci = 0; ci < static_cast<int>(candidates.size()); ++ci) {
            const Candidate& c = candidates[ci];
            if (c.slotIndex < 0) continue;
            const SlotState& s = slots[c.slotIndex];
            if (!s.valid) renderNow.push_back(ci);              // first render is mandatory
            else if (s.signature != c.signature) changed.push_back(ci);
        }
        const int budget = std::clamp(refreshBudgetPerFrame, 1, static_cast<int>(slots.size()));
        int remaining = std::max(0, budget - static_cast<int>(renderNow.size()));
        if (!changed.empty() && remaining > 0) {
            const int start = refreshCursor % static_cast<int>(changed.size());
            for (int n = 0; n < static_cast<int>(changed.size()) && remaining > 0; ++n, --remaining) {
                renderNow.push_back(changed[(start + n) % static_cast<int>(changed.size())]);
            }
            refreshCursor = (refreshCursor + 1) % static_cast<int>(changed.size());
        }

        // --- Render the selected slots (6 faces each) ---
        for (int ci : renderNow) {
            const Candidate& c = candidates[ci];
            glm::mat4 proj = glm::perspective(glm::radians(kFaceFovDegrees), 1.0f, kFaceNearPlane, c.farPlane);
            proj[1][1] *= -1.0f; // cube-face vertical flip vs a normal framebuffer (SDL_GPU convention)

            for (int face = 0; face < kFacesPerCube; ++face) {
                const glm::mat4 viewProj =
                    proj * glm::lookAt(c.lightPos, c.lightPos + kFaceDir[face], kFaceUp[face]);

                SDL_GPUDepthStencilTargetInfo target = {};
                target.texture          = cubeArray;
                target.layer            = static_cast<Uint8>(c.slotIndex * kFacesPerCube + face);
                target.clear_depth      = 1.0f;
                target.load_op          = SDL_GPU_LOADOP_CLEAR;
                target.store_op         = SDL_GPU_STOREOP_STORE;
                target.stencil_load_op  = SDL_GPU_LOADOP_DONT_CARE;
                target.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
                target.cycle            = false;

                SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commandBuffer, nullptr, 0, &target);
                SDL_BindGPUGraphicsPipeline(pass, pipeline);

                // Per-light fragment uniform (slot 0): position + far plane for the distance write.
                struct { glm::vec3 lightPos; float farPlane; } fragU{ c.lightPos, c.farPlane };
                SDL_PushGPUFragmentUniformData(commandBuffer, 0, &fragU, sizeof(fragU));

                for (const CasterInfo* casterDraw : c.visible) {
                    SDL_GPUBufferBinding vertexBinding { .buffer = casterDraw->mesh->vertexBuffer, .offset = 0 };
                    SDL_GPUBufferBinding indexBinding  { .buffer = casterDraw->mesh->indexBuffer,  .offset = 0 };
                    SDL_BindGPUVertexBuffers(pass, 0, &vertexBinding, 1);
                    SDL_BindGPUIndexBuffer(pass, &indexBinding, casterDraw->mesh->indexSize);

                    struct { glm::mat4 faceViewProjModel; glm::mat4 model; }
                        vertU{ viewProj * casterDraw->model, casterDraw->model };
                    SDL_PushGPUVertexUniformData(commandBuffer, 0, &vertU, sizeof(vertU));

                    for (const Submesh& submesh : casterDraw->mesh->submeshes) {
                        SDL_DrawGPUIndexedPrimitives(pass, submesh.indexCount, 1, submesh.indexOffset, 0, 0);
                        ++lastCasterDraws;
                    }
                }

                SDL_EndGPURenderPass(pass);
            }

            slots[c.slotIndex].valid     = true;
            slots[c.slotIndex].signature = c.signature;
            ++lastRegenerated;
        }

        // --- Publish sampleable slots (valid ones only, so deferred/new slots read as unshadowed) ---
        for (const Candidate& c : candidates) {
            if (c.slotIndex >= 0 && slots[c.slotIndex].valid) slotForLight[c.id] = c.slotIndex;
        }
        lastActiveLights = static_cast<int>(slotForLight.size());
    }
} // ytail
