//
// Created by Peter Gilbert on 6/28/26.
//

#include "ResourceManager.h"
#include <SDL3_image/SDL_image.h>
#include <SDL3_shadercross/SDL_shadercross.h>
#include <cgltf.h>

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
        textures.insert({path, std::make_shared<Texture>(device, gpuTexture, imageWidth, imageHeight)});
        return textures[path];
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
            if (primitive.indices) {
                for (size_t k = 0; k < primitive.indices->count; ++k)
                    indices.push_back(vertexStart + cgltf_accessor_read_index(primitive.indices, k));
                submeshes.push_back({ (Uint32)indexStart, (Uint32)primitive.indices->count, 0 });
            } else {
                for (size_t v = 0; v < count; ++v)
                    indices.push_back((Uint32)(vertexStart + v));
                submeshes.push_back({ (Uint32)indexStart, (Uint32)count, 0 });
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
        meshes[path] = mesh;
        cgltf_free(data);
        return mesh;
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

        char fullPath[256];
        SDL_snprintf(fullPath, sizeof(fullPath), "%sassets/shaders/%s.hlsl", BasePath, shaderFilename);

        size_t codeSize;
        void* code = SDL_LoadFile(fullPath, &codeSize);
        if (code == nullptr)
        {
            SDL_Log("Failed to load shader from disk! %s", fullPath);
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

    void ResourceManager::initializePipelines() {
        SDL_Log("initializePipelines");
        // need to set pipeline SDL_GPUFrontFace to SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE to match GLTF

        // TODO implement LitSkeletal


        { // LitStatic
            // load shaders
            // vertex : 1 uniform buffer  (Camera cbuffer @ b0 space1)
            // fragment : 2 samplers (diffuse t0, specular t1) + 2 uniform buffers
            // (FrameLighting b0 space3, Material b1 space3)
            SDL_GPUShader* vertexShader   = loadShader(device, "BlinnPhongLit.vert", 0,
                1, 0, 0);
            SDL_GPUShader* fragmentShader = loadShader(device, "BlinnPhongLit.frag", 2,
                2, 0, 0);
            if (vertexShader == nullptr || fragmentShader == nullptr) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to load LitStatic shaders");
                return;
            }

            // Vertex layout - must match the Vertex struct (vec3 position, vec3 normal, vec2 uv).
            SDL_GPUVertexBufferDescription vbDesc = {};
            vbDesc.slot = 0;
            vbDesc.pitch = sizeof(Vertex);
            vbDesc.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
            vbDesc.instance_step_rate = 0;

            SDL_GPUVertexAttribute attributes[3] = {};
            attributes[0].location = 0;  // position -> TEXCOORD0
            attributes[0].buffer_slot = 0;
            attributes[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
            attributes[0].offset = offsetof(Vertex, position);
            attributes[1].location = 1;  // normal -> TEXCOORD1
            attributes[1].buffer_slot = 0;
            attributes[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
            attributes[1].offset = offsetof(Vertex, normal);
            attributes[2].location = 2;  // uv -> TEXCOORD2
            attributes[2].buffer_slot = 0;
            attributes[2].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
            attributes[2].offset = offsetof(Vertex, uv);

            SDL_GPUVertexInputState vertexInput = {};
            vertexInput.vertex_buffer_descriptions = &vbDesc;
            vertexInput.num_vertex_buffers = 1;
            vertexInput.vertex_attributes = attributes;
            vertexInput.num_vertex_attributes = 3;

            // Color target = the window's swapchain format.
            SDL_GPUColorTargetDescription colorTarget = {};
            colorTarget.format = SDL_GetGPUSwapchainTextureFormat(device, window);

            SDL_GPUGraphicsPipelineCreateInfo pipelineCreateInfo = {};
            pipelineCreateInfo.vertex_shader = vertexShader;
            pipelineCreateInfo.fragment_shader = fragmentShader;
            pipelineCreateInfo.vertex_input_state = vertexInput;
            pipelineCreateInfo.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
            pipelineCreateInfo.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
            pipelineCreateInfo.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_BACK;
            pipelineCreateInfo.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE; // like gltf winding

            // Standard z-buffer, no stencil. Only outlined objects touch the stencil buffer,
            // and they use LitStaticStencil instead.
            pipelineCreateInfo.depth_stencil_state.enable_depth_test = true;
            pipelineCreateInfo.depth_stencil_state.enable_depth_write = true;
            pipelineCreateInfo.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS;

            pipelineCreateInfo.target_info.color_target_descriptions = &colorTarget;
            pipelineCreateInfo.target_info.num_color_targets = 1;
            pipelineCreateInfo.target_info.has_depth_stencil_target = true;
            pipelineCreateInfo.target_info.depth_stencil_format = depthStencilFormat;

            // create pipeline
            SDL_GPUGraphicsPipeline* pipeline = SDL_CreateGPUGraphicsPipeline(device, &pipelineCreateInfo);
            if (pipeline == nullptr) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create LitStatic pipeline: %s", SDL_GetError());
            }
            pipelines[static_cast<size_t>(PipelineType::LitStatic)] = pipeline;

            // unload shaders
            SDL_ReleaseGPUShader(device, vertexShader);
            SDL_ReleaseGPUShader(device, fragmentShader);
        }


        // Same shaders and state as LitStatic, but stamps the stencil buffer with the reference
        // value (set to 1 at draw time) for outlined objects. REPLACE on both pass and depth-fail
        // stamps the full silhouette - including parts hidden behind nearer geometry so the
        // outline pass reads it as a ring rather than filling the occluded region.
        { // LitStaticStencil
            SDL_GPUShader* vertexShader   = loadShader(device, "BlinnPhongLit.vert", 0,
                1, 0, 0);
            SDL_GPUShader* fragmentShader = loadShader(device, "BlinnPhongLit.frag", 2,
                2, 0, 0);
            if (vertexShader == nullptr || fragmentShader == nullptr) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to load LitStaticStencil shaders");
                return;
            }

            // Vertex layout - must match the Vertex struct (vec3 position, vec3 normal, vec2 uv).
            SDL_GPUVertexBufferDescription vbDesc = {};
            vbDesc.slot = 0;
            vbDesc.pitch = sizeof(Vertex);
            vbDesc.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
            vbDesc.instance_step_rate = 0;

            SDL_GPUVertexAttribute attributes[3] = {};
            attributes[0].location = 0;  // position -> TEXCOORD0
            attributes[0].buffer_slot = 0;
            attributes[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
            attributes[0].offset = offsetof(Vertex, position);
            attributes[1].location = 1;  // normal -> TEXCOORD1
            attributes[1].buffer_slot = 0;
            attributes[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
            attributes[1].offset = offsetof(Vertex, normal);
            attributes[2].location = 2;  // uv -> TEXCOORD2
            attributes[2].buffer_slot = 0;
            attributes[2].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
            attributes[2].offset = offsetof(Vertex, uv);

            SDL_GPUVertexInputState vertexInput = {};
            vertexInput.vertex_buffer_descriptions = &vbDesc;
            vertexInput.num_vertex_buffers = 1;
            vertexInput.vertex_attributes = attributes;
            vertexInput.num_vertex_attributes = 3;

            // Color target = the window's swapchain format.
            SDL_GPUColorTargetDescription colorTarget = {};
            colorTarget.format = SDL_GetGPUSwapchainTextureFormat(device, window);

            SDL_GPUGraphicsPipelineCreateInfo pipelineCreateInfo = {};
            pipelineCreateInfo.vertex_shader = vertexShader;
            pipelineCreateInfo.fragment_shader = fragmentShader;
            pipelineCreateInfo.vertex_input_state = vertexInput;
            pipelineCreateInfo.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
            pipelineCreateInfo.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
            pipelineCreateInfo.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_BACK;
            pipelineCreateInfo.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE; // like gltf winding

            pipelineCreateInfo.depth_stencil_state.enable_depth_test = true;
            pipelineCreateInfo.depth_stencil_state.enable_depth_write = true;
            pipelineCreateInfo.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS;
            pipelineCreateInfo.depth_stencil_state.enable_stencil_test = true;
            pipelineCreateInfo.depth_stencil_state.compare_mask = 0xFF;
            pipelineCreateInfo.depth_stencil_state.write_mask = 0xFF;
            pipelineCreateInfo.depth_stencil_state.front_stencil_state.compare_op = SDL_GPU_COMPAREOP_ALWAYS;
            pipelineCreateInfo.depth_stencil_state.front_stencil_state.pass_op = SDL_GPU_STENCILOP_REPLACE;
            pipelineCreateInfo.depth_stencil_state.front_stencil_state.fail_op = SDL_GPU_STENCILOP_KEEP;
            pipelineCreateInfo.depth_stencil_state.front_stencil_state.depth_fail_op = SDL_GPU_STENCILOP_REPLACE;
            pipelineCreateInfo.depth_stencil_state.back_stencil_state =
                pipelineCreateInfo.depth_stencil_state.front_stencil_state;

            pipelineCreateInfo.target_info.color_target_descriptions = &colorTarget;
            pipelineCreateInfo.target_info.num_color_targets = 1;
            pipelineCreateInfo.target_info.has_depth_stencil_target = true;
            pipelineCreateInfo.target_info.depth_stencil_format = depthStencilFormat;

            // create pipeline
            SDL_GPUGraphicsPipeline* pipeline = SDL_CreateGPUGraphicsPipeline(device, &pipelineCreateInfo);
            if (pipeline == nullptr) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create LitStaticStencil pipeline: %s", SDL_GetError());
            }
            pipelines[static_cast<size_t>(PipelineType::LitStaticStencil)] = pipeline;

            // unload shaders
            SDL_ReleaseGPUShader(device, vertexShader);
            SDL_ReleaseGPUShader(device, fragmentShader);
        }


        // reuses BlinnPhongLit.vert (it already passes the normal through); Unlit.frag outputs
        // normal-as-color, so no textures or lighting are needed. Good for a first cube.
        // vertex   : 1 uniform buffer (Camera cbuffer @ b0 space1)
        // fragment : no samplers, no uniform buffers
        { // UnlitStatic
            SDL_GPUShader* vertexShader   = loadShader(device, "BlinnPhongLit.vert", 0,
                1, 0, 0);
            SDL_GPUShader* fragmentShader = loadShader(device, "Unlit.frag", 0,
                0, 0, 0);
            if (vertexShader == nullptr || fragmentShader == nullptr) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to load UnlitStatic shaders");
                return;
            }

            // Vertex layout - must match the Vertex struct (vec3 position, vec3 normal, vec2 uv).
            SDL_GPUVertexBufferDescription vbDesc = {};
            vbDesc.slot = 0;
            vbDesc.pitch = sizeof(Vertex);
            vbDesc.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
            vbDesc.instance_step_rate = 0;

            SDL_GPUVertexAttribute attributes[3] = {};
            attributes[0].location = 0;  // position -> TEXCOORD0
            attributes[0].buffer_slot = 0;
            attributes[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
            attributes[0].offset = offsetof(Vertex, position);
            attributes[1].location = 1;  // normal -> TEXCOORD1
            attributes[1].buffer_slot = 0;
            attributes[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
            attributes[1].offset = offsetof(Vertex, normal);
            attributes[2].location = 2;  // uv -> TEXCOORD2
            attributes[2].buffer_slot = 0;
            attributes[2].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
            attributes[2].offset = offsetof(Vertex, uv);

            SDL_GPUVertexInputState vertexInput = {};
            vertexInput.vertex_buffer_descriptions = &vbDesc;
            vertexInput.num_vertex_buffers = 1;
            vertexInput.vertex_attributes = attributes;
            vertexInput.num_vertex_attributes = 3;

            // Color target = the window's swapchain format.
            SDL_GPUColorTargetDescription colorTarget = {};
            colorTarget.format = SDL_GetGPUSwapchainTextureFormat(device, window);

            SDL_GPUGraphicsPipelineCreateInfo pipelineCreateInfo = {};
            pipelineCreateInfo.vertex_shader = vertexShader;
            pipelineCreateInfo.fragment_shader = fragmentShader;
            pipelineCreateInfo.vertex_input_state = vertexInput;
            pipelineCreateInfo.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
            pipelineCreateInfo.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
            pipelineCreateInfo.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_BACK;
            pipelineCreateInfo.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE; // like gltf winding

            // Standard z-buffer, no stencil (unlit objects don't participate in outlining).
            pipelineCreateInfo.depth_stencil_state.enable_depth_test = true;
            pipelineCreateInfo.depth_stencil_state.enable_depth_write = true;
            pipelineCreateInfo.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS;

            pipelineCreateInfo.target_info.color_target_descriptions = &colorTarget;
            pipelineCreateInfo.target_info.num_color_targets = 1;
            pipelineCreateInfo.target_info.has_depth_stencil_target = true;
            pipelineCreateInfo.target_info.depth_stencil_format = depthStencilFormat;

            // create pipeline
            SDL_GPUGraphicsPipeline* pipeline = SDL_CreateGPUGraphicsPipeline(device, &pipelineCreateInfo);
            if (pipeline == nullptr) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create UnlitStatic pipeline: %s", SDL_GetError());
            }
            pipelines[static_cast<size_t>(PipelineType::UnlitStatic)] = pipeline;

            // unload shaders
            SDL_ReleaseGPUShader(device, vertexShader);
            SDL_ReleaseGPUShader(device, fragmentShader);
        }


        // reuses BlinnPhongLit.vert (fed a scaled MVP so the shell pokes out past the mesh);
        // CustomColor.frag returns a flat uniform color. Drawn only where the stencil was NOT
        // stamped (just outside an outlined object's silhouette), with depth testing off so it
        // overdraws.
        // vertex   : 1 uniform buffer (Camera cbuffer @ b0 space1)
        // fragment : no samplers, 1 uniform buffer (CustomColor @ b0 space3)
        { // Outline
            SDL_GPUShader* vertexShader   = loadShader(device, "BlinnPhongLit.vert", 0,
                1, 0, 0);
            SDL_GPUShader* fragmentShader = loadShader(device, "CustomColor.frag", 0,
                1, 0, 0);
            if (vertexShader == nullptr || fragmentShader == nullptr) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to load Outline shaders");
                return;
            }

            // Vertex layout - must match the Vertex struct (vec3 position, vec3 normal, vec2 uv).
            SDL_GPUVertexBufferDescription vbDesc = {};
            vbDesc.slot = 0;
            vbDesc.pitch = sizeof(Vertex);
            vbDesc.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
            vbDesc.instance_step_rate = 0;

            SDL_GPUVertexAttribute attributes[3] = {};
            attributes[0].location = 0;  // position -> TEXCOORD0
            attributes[0].buffer_slot = 0;
            attributes[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
            attributes[0].offset = offsetof(Vertex, position);
            attributes[1].location = 1;  // normal -> TEXCOORD1
            attributes[1].buffer_slot = 0;
            attributes[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
            attributes[1].offset = offsetof(Vertex, normal);
            attributes[2].location = 2;  // uv -> TEXCOORD2
            attributes[2].buffer_slot = 0;
            attributes[2].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
            attributes[2].offset = offsetof(Vertex, uv);

            SDL_GPUVertexInputState vertexInput = {};
            vertexInput.vertex_buffer_descriptions = &vbDesc;
            vertexInput.num_vertex_buffers = 1;
            vertexInput.vertex_attributes = attributes;
            vertexInput.num_vertex_attributes = 3;

            // Color target = the window's swapchain format.
            SDL_GPUColorTargetDescription colorTarget = {};
            colorTarget.format = SDL_GetGPUSwapchainTextureFormat(device, window);

            SDL_GPUGraphicsPipelineCreateInfo pipelineCreateInfo = {};
            pipelineCreateInfo.vertex_shader = vertexShader;
            pipelineCreateInfo.fragment_shader = fragmentShader;
            pipelineCreateInfo.vertex_input_state = vertexInput;
            pipelineCreateInfo.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
            pipelineCreateInfo.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
            pipelineCreateInfo.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_BACK;
            pipelineCreateInfo.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE; // like gltf winding

            // No depth (overdraws on top), and draw only where stencil != reference (1) so the
            // shell shows just outside the silhouette. write_mask 0 leaves the stencil untouched.
            pipelineCreateInfo.depth_stencil_state.enable_depth_test = false;
            pipelineCreateInfo.depth_stencil_state.enable_depth_write = false;
            pipelineCreateInfo.depth_stencil_state.enable_stencil_test = true;
            pipelineCreateInfo.depth_stencil_state.compare_mask = 0xFF;
            pipelineCreateInfo.depth_stencil_state.write_mask = 0x00;
            pipelineCreateInfo.depth_stencil_state.front_stencil_state.compare_op = SDL_GPU_COMPAREOP_NOT_EQUAL;
            pipelineCreateInfo.depth_stencil_state.front_stencil_state.pass_op = SDL_GPU_STENCILOP_KEEP;
            pipelineCreateInfo.depth_stencil_state.front_stencil_state.fail_op = SDL_GPU_STENCILOP_KEEP;
            pipelineCreateInfo.depth_stencil_state.front_stencil_state.depth_fail_op = SDL_GPU_STENCILOP_KEEP;
            pipelineCreateInfo.depth_stencil_state.back_stencil_state =
                pipelineCreateInfo.depth_stencil_state.front_stencil_state;

            pipelineCreateInfo.target_info.color_target_descriptions = &colorTarget;
            pipelineCreateInfo.target_info.num_color_targets = 1;
            pipelineCreateInfo.target_info.has_depth_stencil_target = true;
            pipelineCreateInfo.target_info.depth_stencil_format = depthStencilFormat;

            // create pipeline
            SDL_GPUGraphicsPipeline* pipeline = SDL_CreateGPUGraphicsPipeline(device, &pipelineCreateInfo);
            if (pipeline == nullptr) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create Outline pipeline: %s", SDL_GetError());
            }
            pipelines[static_cast<size_t>(PipelineType::Outline)] = pipeline;

            // unload shaders
            SDL_ReleaseGPUShader(device, vertexShader);
            SDL_ReleaseGPUShader(device, fragmentShader);
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
    }
} // ytail