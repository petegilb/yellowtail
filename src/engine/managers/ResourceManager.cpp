//
// Created by Peter Gilbert on 6/28/26.
//

#include "ResourceManager.h"
#include <fstream>
#include <SDL3_image/SDL_image.h>
#include <SDL3_shadercross/SDL_shadercross.h>
#include <cgltf.h>

#include "../render/JoltDebugVertex.h"
#include "../serialize/EnumJson.h"

namespace ytail {
    ResourceManager::ResourceManager(SDL_GPUDevice* inDevice, SDL_Window* inWindow, const char* inBasePath) {
        device = inDevice;
        window = inWindow;
        BasePath = inBasePath;

        // Pick a depth+stencil format the device supports. Stencil is required for the outline
        // mask. One of D24_S8 / D32_S8 is guaranteed; prefer D24_S8 for the smaller footprint.
        if (SDL_GPUTextureSupportsFormat(device, SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT,
                SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET)) {
            depthStencilFormat = SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT;
        } else {
            depthStencilFormat = SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT;
        }

        // Sampleable depth format for the shadow map. Prefer D32_FLOAT, fall back to D16_UNORM.
        if (SDL_GPUTextureSupportsFormat(device, SDL_GPU_TEXTUREFORMAT_D32_FLOAT, SDL_GPU_TEXTURETYPE_2D,
                SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER)) {
            shadowMapFormat = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
        } else {
            shadowMapFormat = SDL_GPU_TEXTUREFORMAT_D16_UNORM;
        }

        initializeSamplers();
        initializePipelines();
    }

    std::shared_ptr<Texture> ResourceManager::getTexture(const std::string &path, bool srgb) {
        // a decent reference: https://glusoft.com/sdl3-tutorials/display-texture-sdl3_gpu/
        if (textures.contains(path)) {
            return textures[path];
        }
        // Load the image (path is assets-relative, e.g. "textures/crate.png")
        SDL_Surface* imageData = IMG_Load(resolveAssetPath(path).c_str());
        if (imageData == nullptr){
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Could not load image data! %s", SDL_GetError());
            return nullptr;
        }
        // reference: SDL_gpu_examples TexturedQuad.c

        // put it into a SDL_GPUTexture
        // TODO give options for how to load this texture? how do i change the sampler?
        // Debug name for the texture - shows up in GPU debuggers (RenderDoc, Xcode, etc.).
        SDL_PropertiesID props = SDL_CreateProperties();
        SDL_SetStringProperty(props, SDL_PROP_GPU_TEXTURE_CREATE_NAME_STRING, path.c_str());

        // sRGB for color textures (gamma-correct sampling), linear UNORM for data textures.
        // Same byte layout either way, so the RGBA32 conversion below is unaffected.
        const SDL_GPUTextureFormat format = srgb
            ? SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB
            : SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;

        // Named local (compound literals are C99, not C++), fields in declaration order.
        const SDL_GPUTextureCreateInfo texInfo = {
            .type                 = SDL_GPU_TEXTURETYPE_2D,
            .format               = format,
            .usage                = SDL_GPU_TEXTUREUSAGE_SAMPLER,
            .width                = static_cast<Uint32>(imageData->w),
            .height               = static_cast<Uint32>(imageData->h),
            .layer_count_or_depth = 1,
            .num_levels           = 1,
            .props                = props,
        };
        SDL_GPUTexture* gpuTexture = SDL_CreateGPUTexture(device, &texInfo);
        SDL_DestroyProperties(props);   // texture copied what it needs; free the props object

        // upload the pixels into the GPUTexture that we made

        // Convert to RGBA32 so the bytes match the R8G8B8A8 format above.
        SDL_Surface* converted = SDL_ConvertSurface(imageData, SDL_PIXELFORMAT_RGBA32);
        SDL_DestroySurface(imageData);
        if (converted == nullptr) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Could not convert surface! %s", SDL_GetError());
            SDL_ReleaseGPUTexture(device, gpuTexture);
            return nullptr;
        }

        const Uint32 imageWidth  = static_cast<Uint32>(converted->w);
        const Uint32 imageHeight = static_cast<Uint32>(converted->h);
        // 4 bytes/pixel (RGBA8)
        const Uint32 byteSize = imageWidth * imageHeight * 4;

        // Stage the pixels in a transfer buffer.
        SDL_GPUTransferBufferCreateInfo tbInfo = {};
        tbInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        tbInfo.size  = byteSize;
        SDL_GPUTransferBuffer* transferBuffer = SDL_CreateGPUTransferBuffer(device, &tbInfo);

        void* mapped = SDL_MapGPUTransferBuffer(device, transferBuffer, false);
        SDL_memcpy(mapped, converted->pixels, byteSize);
        SDL_UnmapGPUTransferBuffer(device, transferBuffer);

        // Copy pass: transfer buffer -> texture (VRAM).
        SDL_GPUCommandBuffer* uploadCmd = SDL_AcquireGPUCommandBuffer(device);
        SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(uploadCmd);

        SDL_GPUTextureTransferInfo src = {};
        src.transfer_buffer = transferBuffer;
        // 0 pixels_per_row/rows_per_layer = tightly packed
        src.offset = 0;

        SDL_GPUTextureRegion dst = {};
        dst.texture = gpuTexture;
        dst.w = imageWidth;
        dst.h = imageHeight;
        dst.d = 1;

        SDL_UploadToGPUTexture(copyPass, &src, &dst, false);
        SDL_EndGPUCopyPass(copyPass);
        SDL_SubmitGPUCommandBuffer(uploadCmd);
        SDL_ReleaseGPUTransferBuffer(device, transferBuffer);
        SDL_DestroySurface(converted);

        // create texture and return it
        auto texture = std::make_shared<Texture>(device, gpuTexture, imageWidth, imageHeight);
        texture->sourcePath = path;
        textures.insert({path, texture});
        return texture;
    }

    std::shared_ptr<Texture> ResourceManager::getSolidTexture(Uint8 r, Uint8 g, Uint8 b, Uint8 a) {
        // Cache in the same map as file textures, keyed by a synthetic name so identical colors
        // share one GPU texture (the "__" prefix can't collide with an assets-relative path).
        char key[24];
        SDL_snprintf(key, sizeof(key), "__solid_%02x%02x%02x%02x", r, g, b, a);
        if (textures.contains(key)) return textures[key];

        // Linear UNORM (not sRGB): these stand in for data masks like specular, which sample linear.
        const SDL_GPUTextureCreateInfo texInfo = {
            .type                 = SDL_GPU_TEXTURETYPE_2D,
            .format               = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
            .usage                = SDL_GPU_TEXTUREUSAGE_SAMPLER,
            .width                = 1,
            .height               = 1,
            .layer_count_or_depth = 1,
            .num_levels           = 1,
        };
        SDL_GPUTexture* gpuTexture = SDL_CreateGPUTexture(device, &texInfo);

        // Stage the single RGBA8 texel and copy it into the texture.
        const Uint8 pixel[4] = { r, g, b, a };
        SDL_GPUTransferBufferCreateInfo tbInfo = {};
        tbInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        tbInfo.size  = sizeof(pixel);
        SDL_GPUTransferBuffer* transferBuffer = SDL_CreateGPUTransferBuffer(device, &tbInfo);

        void* mapped = SDL_MapGPUTransferBuffer(device, transferBuffer, false);
        SDL_memcpy(mapped, pixel, sizeof(pixel));
        SDL_UnmapGPUTransferBuffer(device, transferBuffer);

        SDL_GPUCommandBuffer* uploadCmd = SDL_AcquireGPUCommandBuffer(device);
        SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(uploadCmd);

        SDL_GPUTextureTransferInfo src = {};
        src.transfer_buffer = transferBuffer;
        src.offset = 0;

        SDL_GPUTextureRegion dst = {};
        dst.texture = gpuTexture;
        dst.w = 1;
        dst.h = 1;
        dst.d = 1;

        SDL_UploadToGPUTexture(copyPass, &src, &dst, false);
        SDL_EndGPUCopyPass(copyPass);
        SDL_SubmitGPUCommandBuffer(uploadCmd);
        SDL_ReleaseGPUTransferBuffer(device, transferBuffer);

        textures.insert({ key, std::make_shared<Texture>(device, gpuTexture, 1, 1) });
        return textures[key];
    }

    std::shared_ptr<Texture> ResourceManager::getSolidTexture(Uint8 x){
        return getSolidTexture(x, x, x, 255);
    }

    std::shared_ptr<Mesh> ResourceManager::getMesh(const std::string &path) {
        if (meshes.contains(path)) return meshes[path];

        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Loading mesh from: %s", path.c_str());

        // load mesh from disk (path is assets-relative, e.g. "models/cube.gltf")
        const std::string fullPath = resolveAssetPath(path);
        cgltf_options options = {};
        cgltf_data* data = nullptr;
        cgltf_result result = cgltf_parse_file(&options, fullPath.c_str(), &data);
        // TODO how should I handle importing scenes? should I limit this to just 1 mesh for now?
        // what are all the fields I need to store?
        if (result != cgltf_result_success) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Could not parse gltf %s | cgltf_result: %d",
                fullPath.c_str(), result);
            cgltf_free(data);
            return nullptr;
        }
        // A .gltf file is JSON that describes structure (how many meshes, what accessors exist, etc.) but the actual
        // vertex/index bytes live elsewhere - in a separate .bin file, or an external file, or (for .glb)
        // a binary chunk, or base64 embedded in the JSON. That's the "external buffer data."
        // load the actual buffers into the file. (fullPath so cgltf can resolve a relative .bin)
        result = cgltf_load_buffers(&options, data, fullPath.c_str());
        if (result != cgltf_result_success) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Could not load buffers for gltf %s | cgltf_result: %d",
                path.c_str(), result);
            cgltf_free(data);
            return nullptr;
        }

        // extract the vertex attributes, indices, per-primitive grouping (for submeshes)

        std::vector<Vertex> vertices;
        std::vector<Uint32> indices;
        std::vector<Submesh> submeshes;

        // TODO allow importing more than one mesh (we're naively getting only the first index)
        cgltf_mesh& meshData = data->meshes[0];
        std::string name = meshData.name ? meshData.name : "";

        // Map each distinct glTF material to a dense slot in first-appearance order, so a
        // multi-material mesh draws submesh N with materials[slot(N)] in RenderComponent.
        // Primitives without a material (nullptr) share one slot.
        std::vector<const cgltf_material*> slotMaterials;
        auto materialSlotFor = [&slotMaterials](const cgltf_material* gltfMaterial) -> Uint32 {
            for (size_t s = 0; s < slotMaterials.size(); ++s) {
                if (slotMaterials[s] == gltfMaterial) return static_cast<Uint32>(s);
            }
            slotMaterials.push_back(gltfMaterial);
            return static_cast<Uint32>(slotMaterials.size() - 1);
        };

        for (size_t p=0; p < meshData.primitives_count; p++) {
            cgltf_primitive& primitive = meshData.primitives[p];

            // export all the different attributes we need using accessors
            // see: cgltf_attribute_type
            cgltf_accessor* pos = nullptr;
            cgltf_accessor* nrm = nullptr;
            cgltf_accessor* uv = nullptr;
            for (size_t a = 0; a < primitive.attributes_count; ++a) {
                auto& attr = primitive.attributes[a];
                if (attr.type == cgltf_attribute_type_position) pos = attr.data;
                else if (attr.type == cgltf_attribute_type_normal) nrm = attr.data;
                else if (attr.type == cgltf_attribute_type_texcoord) uv = attr.data;
            }

            // do we need to handle index offsetting?
            if (pos == nullptr) continue;
            const size_t vertexStart = vertices.size();
            const size_t count = pos->count;
            for (size_t v = 0; v < count; ++v) {
                Vertex vert{};
                cgltf_accessor_read_float(pos, v, &vert.position.x, 3);
                if (nrm) cgltf_accessor_read_float(nrm, v, &vert.normal.x, 3);
                if (uv)  cgltf_accessor_read_float(uv,  v, &vert.uv.x, 2);
                vertices.push_back(vert);
            }

            // indices (offset them by vertexStart since we concatenate primitives into one buffer)
            const size_t indexStart = indices.size();
            // handle case if the mesh is not indexed
            // push one Submesh per primitive
            const Uint32 materialSlot = materialSlotFor(primitive.material);
            if (primitive.indices) {
                for (size_t k = 0; k < primitive.indices->count; ++k)
                    indices.push_back(vertexStart + cgltf_accessor_read_index(primitive.indices, k));
                submeshes.push_back({ (Uint32)indexStart, (Uint32)primitive.indices->count, materialSlot });
            } else {
                for (size_t v = 0; v < count; ++v)
                    indices.push_back((Uint32)(vertexStart + v));
                submeshes.push_back({ (Uint32)indexStart, (Uint32)count, materialSlot });
            }
        }

        // upload the vertices and indices to the GPU
        // inspired by TexturedQuad.c in the SDL_GPU examples.

        // create the GPU Buffers based on the sizes of each array
        const auto vertexBytes = static_cast<Uint32>(vertices.size() * sizeof(Vertex));
        const auto indexBytes  = static_cast<Uint32>(indices.size()  * sizeof(Uint32));

        SDL_GPUBufferCreateInfo vbInfo = {};
        vbInfo.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
        vbInfo.size  = vertexBytes;
        SDL_GPUBuffer* vertexBuffer = SDL_CreateGPUBuffer(device, &vbInfo);

        SDL_GPUBufferCreateInfo ibInfo = {};
        ibInfo.usage = SDL_GPU_BUFFERUSAGE_INDEX;
        ibInfo.size  = indexBytes;
        SDL_GPUBuffer* indexBuffer = SDL_CreateGPUBuffer(device, &ibInfo);

        // create the transfer buffer that we can use for both
        SDL_GPUTransferBufferCreateInfo tbInfo = {};
        tbInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        tbInfo.size  = vertexBytes + indexBytes;
        SDL_GPUTransferBuffer* transferBuffer = SDL_CreateGPUTransferBuffer(device, &tbInfo);

        // copy over the data from our vertices and indices into the transfer buffer (on the CPU side)
        void* mapped = SDL_MapGPUTransferBuffer(device, transferBuffer, false);
        SDL_memcpy(mapped, vertices.data(), vertexBytes);
        SDL_memcpy(static_cast<Uint8*>(mapped) + vertexBytes, indices.data(), indexBytes);
        SDL_UnmapGPUTransferBuffer(device, transferBuffer);

        // copy over to the GPU
        SDL_GPUCommandBuffer* uploadCmd = SDL_AcquireGPUCommandBuffer(device);
        SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(uploadCmd);

        SDL_GPUTransferBufferLocation vbSrc = { .transfer_buffer = transferBuffer, .offset = 0 };
        SDL_GPUBufferRegion vbDst = { .buffer = vertexBuffer, .offset = 0, .size = vertexBytes };
        SDL_UploadToGPUBuffer(copyPass, &vbSrc, &vbDst, false);

        SDL_GPUTransferBufferLocation ibSrc = { .transfer_buffer = transferBuffer, .offset = vertexBytes };
        SDL_GPUBufferRegion ibDst = { .buffer = indexBuffer, .offset = 0, .size = indexBytes };
        SDL_UploadToGPUBuffer(copyPass, &ibSrc, &ibDst, false);

        SDL_EndGPUCopyPass(copyPass);
        SDL_SubmitGPUCommandBuffer(uploadCmd);

        // Release the transfer buffer
        SDL_ReleaseGPUTransferBuffer(device, transferBuffer);

        // create mesh object and make sure we can store everything properly
        std::shared_ptr<Mesh> mesh = std::make_shared<Mesh>(device, name, vertexBuffer, indexBuffer, submeshes);
        mesh->sourcePath = path;

        // local-space bounds over all vertices (for an aabb)
        if (!vertices.empty()) {
            glm::vec3 aabbMin = vertices[0].position;
            glm::vec3 aabbMax = vertices[0].position;
            for (const Vertex& vert : vertices) {
                aabbMin = glm::min(aabbMin, vert.position);
                aabbMax = glm::max(aabbMax, vert.position);
            }
            mesh->aabbMin = aabbMin;
            mesh->aabbMax = aabbMax;
        }

        meshes[path] = mesh;
        cgltf_free(data);
        return mesh;
    }

    std::shared_ptr<Material> ResourceManager::getMaterial(const std::string& path) {
        if (materials.contains(path)) return materials[path];

        // Read the .mat file (e.g. "materials/crate.mat").
        std::ifstream file(resolveAssetPath(path));
        if (!file.is_open()) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Could not open material file: %s", path.c_str());
            return nullptr;
        }
        nlohmann::json j;
        try {
            file >> j;
        } catch (const std::exception& e) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Could not parse material %s: %s", path.c_str(), e.what());
            return nullptr;
        }

        auto material = std::make_shared<Material>();
        material->sourcePath = path;
        if (j.contains("pipeline")) j.at("pipeline").get_to(material->pipelineType);

        // Each texture entry has a sampler, and is either a file path or a solid color.
        if (j.contains("textures")) {
            for (const auto& t : j.at("textures")) {
                const SamplerType samplerType = t.value("sampler", SamplerType::LinearWrap);
                SDL_GPUSampler* sampler = getSampler(samplerType);

                std::shared_ptr<Texture> texture;
                if (t.contains("solid")) {
                    const auto& c = t.at("solid");
                    if (c.size() == 1) {
                        texture = getSolidTexture(c.at(0).get<Uint8>());
                    } else if (c.size() >= 3) {
                        const Uint8 a = c.size() >= 4 ? c.at(3).get<Uint8>() : 255;
                        texture = getSolidTexture(c.at(0).get<Uint8>(), c.at(1).get<Uint8>(),
                                                  c.at(2).get<Uint8>(), a);
                    }
                } else if (t.contains("path")) {
                    texture = getTexture(t.at("path").get<std::string>(), t.value("srgb", false));
                }
                if (!texture){
                    // Keep the material usable (and cached) with an obvious placeholder rather
                    // than failing it whole and re-parsing the file on every lookup.
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                        "Bad texture entry in material %s; using magenta fallback", path.c_str());
                    texture = getSolidTexture(255, 0, 255);
                    if (!texture) return nullptr; // GPU texture creation failing is unrecoverable
                }
                material->textures.push_back({ texture, sampler, samplerType });
            }
        }

        // Uniform values (uvScale/uvOffset/shininess); missing fields keep their defaults.
        MaterialUniform uniform{};
        if (j.contains("uniform")) j.at("uniform").get_to(uniform);
        material->setUniform(uniform);

        materials[path] = material;
        return material;
    }

    std::shared_ptr<Material> ResourceManager::reloadMaterial(const std::string& path) {
        materials.erase(path);
        return getMaterial(path);
    }

    SDL_GPUGraphicsPipeline* ResourceManager::getPipeline(PipelineType type, bool outline) {
        if (outline && type == PipelineType::LitStatic) {
            type = PipelineType::LitStaticStencil;
        }
        return pipelines[static_cast<size_t>(type)];
    }

    SDL_GPUShader * ResourceManager::loadShader(SDL_GPUDevice *inDevice, const char *shaderFilename,
        Uint32 samplerCount, Uint32 uniformBufferCount, Uint32 storageBufferCount, Uint32 storageTextureCount) {
        // We ship HLSL source and cross-compile to the backend's format at runtime
        // via SDL_shadercross (HLSL -> SPIR-V -> native), so the same .hlsl works on
        // Vulkan/D3D12/Metal with no offline compile step.
        SDL_ShaderCross_ShaderStage stage;
        if (SDL_strstr(shaderFilename, ".vert"))
        {
            stage = SDL_SHADERCROSS_SHADERSTAGE_VERTEX;
        }
        else if (SDL_strstr(shaderFilename, ".frag"))
        {
            stage = SDL_SHADERCROSS_SHADERSTAGE_FRAGMENT;
        }
        else
        {
            SDL_Log("Invalid shader stage!");
            return nullptr;
        }

        // std::string, not a fixed buffer: a long install path would silently truncate.
        const std::string fullPath = std::string(BasePath) + "assets/shaders/" + shaderFilename + ".hlsl";

        size_t codeSize;
        void* code = SDL_LoadFile(fullPath.c_str(), &codeSize);
        if (code == nullptr)
        {
            SDL_Log("Failed to load shader from disk! %s", fullPath.c_str());
            return nullptr;
        }

        // 1) HLSL -> SPIR-V (DXC). SDL_LoadFile null-terminates, so it's a valid C string.
        SDL_ShaderCross_HLSL_Info hlslInfo = {};
        hlslInfo.source = (const char *)code;
        hlslInfo.entrypoint = "main";
        hlslInfo.shader_stage = stage;

        size_t spirvSize;
        void* spirv = SDL_ShaderCross_CompileSPIRVFromHLSL(&hlslInfo, &spirvSize);
        SDL_free(code);
        if (spirv == nullptr)
        {
            SDL_Log("Failed to compile HLSL to SPIR-V: %s", SDL_GetError());
            return nullptr;
        }

        // 2) SPIR-V -> native SDL_GPUShader (transpiles to MSL/DXIL/SPIRV for this device).
        SDL_ShaderCross_SPIRV_Info spirvInfo = {};
        spirvInfo.bytecode = (const Uint8 *)spirv;
        spirvInfo.bytecode_size = spirvSize;
        spirvInfo.entrypoint = "main";
        spirvInfo.shader_stage = stage;

        SDL_ShaderCross_GraphicsShaderResourceInfo resourceInfo = {};
        resourceInfo.num_samplers = samplerCount;
        resourceInfo.num_uniform_buffers = uniformBufferCount;
        resourceInfo.num_storage_buffers = storageBufferCount;
        resourceInfo.num_storage_textures = storageTextureCount;

        SDL_GPUShader* shader =
                SDL_ShaderCross_CompileGraphicsShaderFromSPIRV(inDevice, &spirvInfo, &resourceInfo, 0);
        SDL_free(spirv);
        if (shader == nullptr)
        {
            SDL_Log("Failed to create shader!");
            return nullptr;
        }

        return shader;
    }

    // The two vertex formats our pipelines consume.
    enum class VertexLayout {
        Mesh,  // Vertex: vec3 position, vec3 normal, vec2 uv
        Line,  // JoltDebugVertex: vec3 position, vec4 color
    };

    // Fills a SDL_GPUGraphicsPipelineCreateInfo with the defaults every pipeline here shares:
    // one swapchain-format color target, a single depth+stencil target, opaque depth test+write
    // (LESS), triangle list, back-face cull, CCW winding. Callers override info.* for anything
    // else (primitive, cull, depth, stencil, ...) and can call enableAlphaBlend() for the common
    // translucent case. Owns the structs info points at, so keep it alive until createPipeline().
    struct PipelineBuilder {
        SDL_GPUVertexBufferDescription vbDesc{};
        SDL_GPUVertexAttribute attributes[3]{};
        SDL_GPUColorTargetDescription colorTarget{};
        SDL_GPUGraphicsPipelineCreateInfo info{};

        PipelineBuilder(const PipelineBuilder&) = delete;
        PipelineBuilder& operator=(const PipelineBuilder&) = delete;

        PipelineBuilder(SDL_GPUDevice* device, SDL_Window* window,
                        SDL_GPUShader* vertexShader, SDL_GPUShader* fragmentShader,
                        VertexLayout layout, SDL_GPUTextureFormat depthStencilFormat) {
            vbDesc.slot = 0;
            vbDesc.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
            vbDesc.instance_step_rate = 0;

            Uint32 attrCount = 0;
            if (layout == VertexLayout::Mesh) {
                vbDesc.pitch = sizeof(Vertex);
                attributes[0] = { 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(Vertex, position) };
                attributes[1] = { 1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(Vertex, normal) };
                attributes[2] = { 2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(Vertex, uv) };
                attrCount = 3;
            } else {
                vbDesc.pitch = sizeof(JoltDebugVertex);
                attributes[0] = { 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(JoltDebugVertex, position) };
                attributes[1] = { 1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, offsetof(JoltDebugVertex, color) };
                attrCount = 2;
            }

            info.vertex_shader = vertexShader;
            info.fragment_shader = fragmentShader;
            info.vertex_input_state.vertex_buffer_descriptions = &vbDesc;
            info.vertex_input_state.num_vertex_buffers = 1;
            info.vertex_input_state.vertex_attributes = attributes;
            info.vertex_input_state.num_vertex_attributes = attrCount;

            info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
            info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
            info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_BACK;
            info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE; // gltf winding

            info.depth_stencil_state.enable_depth_test = true;
            info.depth_stencil_state.enable_depth_write = true;
            info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS;

            colorTarget.format = SDL_GetGPUSwapchainTextureFormat(device, window);
            info.target_info.color_target_descriptions = &colorTarget;
            info.target_info.num_color_targets = 1;
            info.target_info.has_depth_stencil_target = true;
            info.target_info.depth_stencil_format = depthStencilFormat;
        }

        // Depth-only pipeline (shadow pass): no color target, its own depth format.
        void depthOnly(SDL_GPUTextureFormat shadowFormat) {
            info.target_info.color_target_descriptions = nullptr;
            info.target_info.num_color_targets = 0;
            info.target_info.depth_stencil_format = shadowFormat;
        }

        // Standard straight-alpha blend on the color target.
        void enableAlphaBlend() {
            colorTarget.blend_state.enable_blend = true;
            colorTarget.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
            colorTarget.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
            colorTarget.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
            colorTarget.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
            colorTarget.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
            colorTarget.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        }
    };

    // Create the pipeline, logging (but not aborting) on failure so one bad pipeline doesn't
    // silently look like a crash.
    static SDL_GPUGraphicsPipeline* createPipeline(SDL_GPUDevice* device, PipelineBuilder& builder,
                                                   const char* name) {
        SDL_GPUGraphicsPipeline* pipeline = SDL_CreateGPUGraphicsPipeline(device, &builder.info);
        if (pipeline == nullptr) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create %s pipeline: %s",
                         name, SDL_GetError());
        }
        return pipeline;
    }

    void ResourceManager::initializePipelines() {
        SDL_Log("initializePipelines");
        // need to set pipeline SDL_GPUFrontFace to SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE to match GLTF

        // TODO implement LitSkeletal


        { // LitStatic
            // vertex : 1 uniform buffer (Camera @ b0 space1)
            // fragment : 4 samplers (diffuse t0, specular t1, sun shadow t2, point cube t3) + 3
            // uniform buffers (FrameLighting b0 space3, Material b1 space3, Shadow b2 space3)
            SDL_GPUShader* vs = loadShader(device, "BlinnPhongLit.vert", 0, 1, 0, 0);
            SDL_GPUShader* fs = loadShader(device, "BlinnPhongLit.frag", 4, 3, 0, 0);
            if (vs == nullptr || fs == nullptr) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to load LitStatic shaders");
            }
            else{
                // Pure defaults: opaque, depth test+write, back-face cull.
                PipelineBuilder builder(device, window, vs, fs, VertexLayout::Mesh, depthStencilFormat);
                pipelines[static_cast<size_t>(PipelineType::LitStatic)] = createPipeline(device, builder, "LitStatic");
            }

            if (vs) SDL_ReleaseGPUShader(device, vs);
            if (fs) SDL_ReleaseGPUShader(device, fs);
        }


        // Same shaders and state as LitStatic, but stamps the stencil buffer with the reference
        // value (set to 1 at draw time) for outlined objects. REPLACE on both pass and depth-fail
        // stamps the full silhouette - including parts hidden behind nearer geometry so the
        // outline pass reads it as a ring rather than filling the occluded region.
        { // LitStaticStencil
            SDL_GPUShader* vs = loadShader(device, "BlinnPhongLit.vert", 0, 1, 0, 0);
            SDL_GPUShader* fs = loadShader(device, "BlinnPhongLit.frag", 4, 3, 0, 0);
            if (vs == nullptr || fs == nullptr) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to load LitStaticStencil shaders");
            }
            else{
                // Same as LitStatic, but stamp the stencil (ref set to 1 at draw time) for outlined
                // objects. REPLACE on pass and depth-fail marks the full silhouette, including parts
                // hidden behind nearer geometry, so the outline pass reads it as a ring.
                PipelineBuilder builder(device, window, vs, fs, VertexLayout::Mesh, depthStencilFormat);
                auto& ds = builder.info.depth_stencil_state;
                ds.enable_stencil_test = true;
                ds.compare_mask = 0xFF;
                ds.write_mask = 0xFF;
                ds.front_stencil_state.compare_op = SDL_GPU_COMPAREOP_ALWAYS;
                ds.front_stencil_state.pass_op = SDL_GPU_STENCILOP_REPLACE;
                ds.front_stencil_state.fail_op = SDL_GPU_STENCILOP_KEEP;
                ds.front_stencil_state.depth_fail_op = SDL_GPU_STENCILOP_REPLACE;
                ds.back_stencil_state = ds.front_stencil_state;
                pipelines[static_cast<size_t>(PipelineType::LitStaticStencil)] =
                    createPipeline(device, builder, "LitStaticStencil");
            }
            if (vs) SDL_ReleaseGPUShader(device, vs);
            if (fs) SDL_ReleaseGPUShader(device, fs);
        }


        // reuses BlinnPhongLit.vert (it already passes the normal through); Unlit.frag outputs
        // normal-as-color, so no textures or lighting are needed. Good for a first cube.
        // vertex   : 1 uniform buffer (Camera cbuffer @ b0 space1)
        // fragment : no samplers, no uniform buffers
        { // UnlitStatic
            SDL_GPUShader* vs = loadShader(device, "BlinnPhongLit.vert", 0, 1, 0, 0);
            SDL_GPUShader* fs = loadShader(device, "Unlit.frag", 0, 0, 0, 0);
            if (vs == nullptr || fs == nullptr) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to load UnlitStatic shaders");
            }
            else{
                // Pure defaults (unlit objects don't participate in outlining, so no stencil).
                PipelineBuilder builder(device, window, vs, fs, VertexLayout::Mesh, depthStencilFormat);
                pipelines[static_cast<size_t>(PipelineType::UnlitStatic)] = createPipeline(device, builder, "UnlitStatic");
            }
            if (vs) SDL_ReleaseGPUShader(device, vs);
            if (fs) SDL_ReleaseGPUShader(device, fs);
        }


        // reuses BlinnPhongLit.vert (fed a scaled MVP so the shell pokes out past the mesh);
        // CustomColor.frag returns a flat uniform color. Drawn only where the stencil was NOT
        // stamped (just outside an outlined object's silhouette), with depth testing off so it
        // overdraws.
        // vertex   : 1 uniform buffer (Camera cbuffer @ b0 space1)
        // fragment : no samplers, 1 uniform buffer (CustomColor @ b0 space3)
        { // Outline
            SDL_GPUShader* vs = loadShader(device, "BlinnPhongLit.vert", 0, 1, 0, 0);
            SDL_GPUShader* fs = loadShader(device, "CustomColor.frag", 0, 1, 0, 0);
            if (vs == nullptr || fs == nullptr) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to load Outline shaders");
            }
            else{
                // No depth (overdraws on top), and draw only where stencil != reference (1) so the
                // shell shows just outside the silhouette. write_mask 0 leaves the stencil untouched.
                PipelineBuilder builder(device, window, vs, fs, VertexLayout::Mesh, depthStencilFormat);
                auto& ds = builder.info.depth_stencil_state;
                ds.enable_depth_test = false;
                ds.enable_depth_write = false;
                ds.enable_stencil_test = true;
                ds.compare_mask = 0xFF;
                ds.write_mask = 0x00;
                ds.front_stencil_state.compare_op = SDL_GPU_COMPAREOP_NOT_EQUAL;
                ds.front_stencil_state.pass_op = SDL_GPU_STENCILOP_KEEP;
                ds.front_stencil_state.fail_op = SDL_GPU_STENCILOP_KEEP;
                ds.front_stencil_state.depth_fail_op = SDL_GPU_STENCILOP_KEEP;
                ds.back_stencil_state = ds.front_stencil_state;
                pipelines[static_cast<size_t>(PipelineType::Outline)] = createPipeline(device, builder, "Outline");
            }

            if (vs) SDL_ReleaseGPUShader(device, vs);
            if (fs) SDL_ReleaseGPUShader(device, fs);
        }


        // Physics debug wireframe. Vertices are world-space (Jolt bakes in the body transform), so
        // the vertex shader only needs view*proj. Drawn as a line list, depth-tested against the
        // scene but not writing depth, no culling.
        // vertex   : 1 uniform buffer (Camera cbuffer @ b0 space1)
        // fragment : SolidColor.frag just passes the vertex color at TEXCOORD0 through
        { // DebugLine
            SDL_GPUShader* vs = loadShader(device, "DebugLine.vert", 0, 1, 0, 0);
            SDL_GPUShader* fs = loadShader(device, "SolidColor.frag", 0, 0, 0, 0);
            if (vs == nullptr || fs == nullptr) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to load DebugLine shaders");
            }
            else{
                // Translucent line list, depth-tested against the scene but not writing depth.
                PipelineBuilder builder(device, window, vs, fs, VertexLayout::Line, depthStencilFormat);
                builder.info.primitive_type = SDL_GPU_PRIMITIVETYPE_LINELIST;
                builder.info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
                builder.info.depth_stencil_state.enable_depth_write = false;
                builder.info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
                builder.enableAlphaBlend();
                pipelines[static_cast<size_t>(PipelineType::DebugLine)] = createPipeline(device, builder, "DebugLine");
            }

            if (vs) SDL_ReleaseGPUShader(device, vs);
            if (fs) SDL_ReleaseGPUShader(device, fs);
        }

        // Editor grid: same line layout as DebugLine, but the fragment shader fades each line
        // per-pixel from a GridFade uniform so lines can be full-length (few vertices, no overdraw
        // from per-cell splitting).
        { // Grid
            SDL_GPUShader* vs = loadShader(device, "GridLine.vert", 0, 1, 0, 0);
            SDL_GPUShader* fs = loadShader(device, "GridLine.frag", 0, 1, 0, 0);
            if (vs == nullptr || fs == nullptr) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to load Grid shaders");
            }
            else{
                // Same state as DebugLine; the fragment shader handles the per-pixel fade.
                PipelineBuilder builder(device, window, vs, fs, VertexLayout::Line, depthStencilFormat);
                builder.info.primitive_type = SDL_GPU_PRIMITIVETYPE_LINELIST;
                builder.info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
                builder.info.depth_stencil_state.enable_depth_write = false;
                builder.info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
                builder.enableAlphaBlend();
                pipelines[static_cast<size_t>(PipelineType::Grid)] = createPipeline(device, builder, "Grid");
            }

            if (vs) SDL_ReleaseGPUShader(device, vs);
            if (fs) SDL_ReleaseGPUShader(device, fs);
        }

        // Camera-facing editor sprites (light / camera icons). Reuses BlinnPhongLit.vert (the model
        // matrix is built camera-facing on the CPU); Billboard.frag samples the icon. Alpha-blended,
        // depth-tested but no depth write, no culling so either winding shows.
        // vertex   : 1 uniform buffer (Camera cbuffer @ b0 space1)
        // fragment : 1 sampler (icon @ t0 space2), no uniform buffers
        { // Billboard
            SDL_GPUShader* vs = loadShader(device, "BlinnPhongLit.vert", 0, 1, 0, 0);
            SDL_GPUShader* fs = loadShader(device, "Billboard.frag", 1, 0, 0, 0);
            if (vs == nullptr || fs == nullptr) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to load Billboard shaders");
            }
            else{
                // Alpha-blended sprite, depth-tested but not writing depth, no culling (either winding).
                PipelineBuilder builder(device, window, vs, fs, VertexLayout::Mesh, depthStencilFormat);
                builder.info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
                builder.info.depth_stencil_state.enable_depth_write = false;
                builder.info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
                builder.enableAlphaBlend();
                pipelines[static_cast<size_t>(PipelineType::Billboard)] = createPipeline(device, builder, "Billboard");
            }

            if (vs) SDL_ReleaseGPUShader(device, vs);
            if (fs) SDL_ReleaseGPUShader(device, fs);
        }

        // Shadow pass: scene depth from the sun's POV into a depth-only target. Reuses the Mesh
        // layout (position only); the fragment shader is a no-op.
        // vertex   : 1 uniform buffer (LightMVP @ b0 space1 = lightViewProj * model)
        // fragment : none
        { // ShadowDepth
            SDL_GPUShader* vs = loadShader(device, "ShadowDepth.vert", 0, 1, 0, 0);
            SDL_GPUShader* fs = loadShader(device, "Empty.frag", 0, 0, 0, 0);
            if (vs == nullptr || fs == nullptr) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to load ShadowDepth shaders");
            }
            else{
                // Front-face cull writes casters' back faces, keeping self-shadow acne off the lit side.
                PipelineBuilder builder(device, window, vs, fs, VertexLayout::Mesh, depthStencilFormat);
                builder.info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_FRONT;
                builder.depthOnly(shadowMapFormat);
                pipelines[static_cast<size_t>(PipelineType::ShadowDepth)] = createPipeline(device, builder, "ShadowDepth");
            }

            if (vs) SDL_ReleaseGPUShader(device, vs);
            if (fs) SDL_ReleaseGPUShader(device, fs);
        }

        // Omnidirectional point-light shadows. Same depth-only, front-face-cull state as
        // ShadowDepth, but a perspective projection (per cube face) and a real fragment shader
        // that writes linear distance-to-light into SV_Depth.
        // vertex   : 1 uniform buffer (PointLightMVP @ b0 space1 = faceViewProj*model + model)
        // fragment : 1 uniform buffer (PointLightDepth @ b0 space3 = lightPos + farPlane)
        { // PointShadowDepth
            SDL_GPUShader* vs = loadShader(device, "PointShadowDepth.vert", 0, 1, 0, 0);
            SDL_GPUShader* fs = loadShader(device, "PointShadowDepth.frag", 0, 1, 0, 0);
            if (vs == nullptr || fs == nullptr) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to load PointShadowDepth shaders");
            }
            else{
                // Render back faces to keep self-shadow acne off the lit side (same trick as the sun
                // map's FRONT cull). The proj Y-flip in PointShadowRenderer reverses winding, so BACK
                // here is what FRONT is for the sun.
                PipelineBuilder builder(device, window, vs, fs, VertexLayout::Mesh, depthStencilFormat);
                builder.info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_BACK;
                builder.depthOnly(shadowMapFormat);
                pipelines[static_cast<size_t>(PipelineType::PointShadowDepth)] =
                    createPipeline(device, builder, "PointShadowDepth");
            }

            if (vs) SDL_ReleaseGPUShader(device, vs);
            if (fs) SDL_ReleaseGPUShader(device, fs);
        }
    }

    void ResourceManager::initializeSamplers() {
        // NOTE: fields are assigned (not designated-initialized) so declaration order
        // doesn't matter - in particular max_anisotropy must precede enable_anisotropy.

        SDL_GPUSamplerCreateInfo pointClamp = {};
        pointClamp.min_filter = SDL_GPU_FILTER_NEAREST;
        pointClamp.mag_filter = SDL_GPU_FILTER_NEAREST;
        pointClamp.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
        pointClamp.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        pointClamp.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        pointClamp.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        samplers[static_cast<size_t>(SamplerType::PointClamp)] = SDL_CreateGPUSampler(device, &pointClamp);

        SDL_GPUSamplerCreateInfo pointWrap = {};
        pointWrap.min_filter = SDL_GPU_FILTER_NEAREST;
        pointWrap.mag_filter = SDL_GPU_FILTER_NEAREST;
        pointWrap.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
        pointWrap.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
        pointWrap.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
        pointWrap.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
        samplers[static_cast<size_t>(SamplerType::PointWrap)] = SDL_CreateGPUSampler(device, &pointWrap);

        SDL_GPUSamplerCreateInfo linearClamp = {};
        linearClamp.min_filter = SDL_GPU_FILTER_LINEAR;
        linearClamp.mag_filter = SDL_GPU_FILTER_LINEAR;
        linearClamp.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
        linearClamp.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        linearClamp.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        linearClamp.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        samplers[static_cast<size_t>(SamplerType::LinearClamp)] = SDL_CreateGPUSampler(device, &linearClamp);

        SDL_GPUSamplerCreateInfo linearWrap = {};
        linearWrap.min_filter = SDL_GPU_FILTER_LINEAR;
        linearWrap.mag_filter = SDL_GPU_FILTER_LINEAR;
        linearWrap.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
        linearWrap.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
        linearWrap.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
        linearWrap.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
        samplers[static_cast<size_t>(SamplerType::LinearWrap)] = SDL_CreateGPUSampler(device, &linearWrap);

        SDL_GPUSamplerCreateInfo anisoClamp = {};
        anisoClamp.min_filter = SDL_GPU_FILTER_LINEAR;
        anisoClamp.mag_filter = SDL_GPU_FILTER_LINEAR;
        anisoClamp.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
        anisoClamp.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        anisoClamp.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        anisoClamp.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        anisoClamp.max_anisotropy = 4;
        anisoClamp.enable_anisotropy = true;
        samplers[static_cast<size_t>(SamplerType::AnisotropicClamp)] = SDL_CreateGPUSampler(device, &anisoClamp);

        SDL_GPUSamplerCreateInfo anisoWrap = {};
        anisoWrap.min_filter = SDL_GPU_FILTER_LINEAR;
        anisoWrap.mag_filter = SDL_GPU_FILTER_LINEAR;
        anisoWrap.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
        anisoWrap.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
        anisoWrap.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
        anisoWrap.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
        anisoWrap.max_anisotropy = 4;
        anisoWrap.enable_anisotropy = true;
        samplers[static_cast<size_t>(SamplerType::AnisotropicWrap)] = SDL_CreateGPUSampler(device, &anisoWrap);

        // Shadow comparison sampler: linear + compare gives hardware 2x2 PCF; clamp so out-of-
        // frustum lookups sample the border. LESS_OR_EQUAL matches the stored depth test.
        SDL_GPUSamplerCreateInfo shadowPcf = {};
        shadowPcf.min_filter = SDL_GPU_FILTER_LINEAR;
        shadowPcf.mag_filter = SDL_GPU_FILTER_LINEAR;
        shadowPcf.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
        shadowPcf.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        shadowPcf.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        shadowPcf.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        shadowPcf.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
        shadowPcf.enable_compare = true;
        samplers[static_cast<size_t>(SamplerType::ShadowPCF)] = SDL_CreateGPUSampler(device, &shadowPcf);
    }
} // ytail