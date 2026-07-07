# Shader authoring

Yellowtail ships HLSL shaders in `assets/shaders/` and compiles them at runtime via SDL_shadercross calling DXC. DXC emits three formats:

- DXIL for D3D12 (Windows)
- SPIR-V for Vulkan (Windows / Linux)
- MSL for Metal (macOS)

Pattern-#1 workflow: edit HLSL, re-run, no offline build step.

## SDL_GPU HLSL conventions

Follow these or shaders will fail to bind on some backends.

### Vertex inputs use `TEXCOORD<N>` semantics

SDL_GPU on the D3D12 backend maps `SDL_GPUVertexAttribute.location = N` to HLSL semantic `TEXCOORD<N>`. Do not use `POSITION`, `NORMAL`, or other stock semantics for vertex inputs. D3D12 will refuse to bind them.

```hlsl
struct Input
{
    float3 position : TEXCOORD0;   // location 0
    float3 normal   : TEXCOORD1;   // location 1
    float2 uv       : TEXCOORD2;   // location 2
};
```

`SV_Position`, `SV_Target`, `SV_TargetN` are still standard. Those are system semantics, not user-authored ones.

### Uniform buffer register spaces

Shadercross expects a specific space layout:

| Stage    | Resource                | Space     |
| -------- | ----------------------- | --------- |
| Vertex   | uniform buffers (cbuf)  | `space1`  |
| Vertex   | samplers / textures     | `space0`  |
| Fragment | samplers / textures     | `space2`  |
| Fragment | uniform buffers (cbuf)  | `space3`  |

Example from `BlinnPhongLit.vert.hlsl`:

```hlsl
cbuffer Camera : register(b0, space1)   // vertex uniform buffer 0
{
    float4x4 mvp;
    float4x4 model;
    float4x4 normalMatrix;
};
```

The `space` numbers must match this pattern. Shadercross's reflection uses them to route resources to SDL_GPU's slots.

### Uniform buffer packing

HLSL cbuffers pack in 16-byte units. A `float3` followed by a `float` shares one 16-byte slot; a `float3` followed by another `float3` does not, because the second one starts a fresh 16-byte slot. Match the C++ struct layout exactly.

```hlsl
cbuffer Lighting : register(b0, space3)
{
    float3 viewPos;   float shininess;   // 16B slot
    float3 lightPos;  float _pad0;       // 16B slot
    float3 lightAmbient;  float _pad1;
};
```

The `_padN` members exist purely to force alignment. Skipping them silently corrupts uniform data.

## Interstage-signature mismatch (D3D12 only)

Symptom on D3D12:

```
D3D12 ERROR: ID3D12Device::CreateGraphicsPipelineState:
  Vertex Shader - Pixel Shader linkage error: Signatures between stages are
  incompatible. Semantic 'TEXCOORD' is defined for mismatched hardware
  registers between the output stage and input stage.
  [ STATE_CREATION ERROR #660: CREATEGRAPHICSPIPELINESTATE_SHADER_LINKAGE_REGISTERINDEX ]
```

The same shaders run fine on Vulkan.

### What's happening

DXC's dead-code elimination strips PS input fields the fragment shader doesn't read. The remaining fields get repacked into hardware input registers (`v0`, `v1`, ...) in declaration order. If `TEXCOORD0` is dropped, the packer moves `TEXCOORD1` to `v0`. But the VS still writes `TEXCOORD1` to `v1`. D3D12's pipeline linker checks that each semantic maps to the same hardware register on both sides, and rejects the pipeline.

Vulkan doesn't hit this because shadercross passes `-fspv-preserve-interface` to DXC on the SPIR-V path, which pins each input to its declared `layout(location = N)` regardless of use.

### Fix 1: pin unused inputs with a `discard` branch

Constant-folding sinks (`* 0.0f`, `+ 0.0f * input.foo`) don't work. DXC proves they're no-ops and strips the reads anyway. The reliable pattern is a `discard` whose condition depends on the input's runtime value. DXC can't prove the branch is always taken, so it has to keep the read.

```hlsl
float4 main(Input input) : SV_Target
{
    // Pin unused inputs into the DXIL signature.
    if (input.fragPos.x > 1e30f) discard;
    if (input.uv.x      > 1e30f) discard;

    float3 n = normalize(input.normal);
    return float4(n * 0.5f + 0.5f, 1.0f);
}
```

Thresholds are chosen to never fire in practice. Only add pins for fields the fragment shader declares but doesn't consume.

### Fix 2: declare only what you use

If a fragment shader never intends to use `fragPos` or `uv`, the cleanest option is to not declare them in `Input`. The VS still writes them to output registers. D3D12 only requires the PS input set to be a subset of the VS output set, not equal. This produces smaller DXIL. Use the pin pattern only when the fragment shader's Input struct must stay symmetric with the vertex shader's Output, for example a family of interchangeable fragment shaders sharing one vertex shader.

## Debugging pipeline creation errors

SDL_GPU errors like `Could not create graphics pipeline state! Error Code: The parameter is incorrect. (0x80070057)` are useless on their own. The D3D12 debug layer gives the real reason, but its output goes through `OutputDebugStringW`, and nothing appears in stdout.

To see it:

1. Install Windows Settings, Apps, Optional features, "Graphics Tools" (about 100 MB, no reboot).
2. Grab [DbgView64.exe](https://download.sysinternals.com/files/DebugView.zip) from Sysinternals. Run as Administrator.
3. In DebugView: Capture, Capture Global Win32, and Capture Kernel.
4. Run `yellowtail.exe`. D3D12 messages stream into the DebugView window with the specific error code (`#NNN`) and description.

Alternative: run yellowtail under CLion's Debug (bug icon). The bundled LLDB should pick up `OutputDebugString`. If it doesn't, use DebugView.

## Why Vulkan works when D3D12 doesn't

Shadercross's Vulkan path uses `-fspv-preserve-interface`, which locks SPIR-V input/output locations to the declared `layout(location = N)` values regardless of dead-code elimination. There's no DXIL equivalent. DXC always repacks the DXIL signature based on live uses.

Practical consequence: a shader can work on Vulkan and fail on D3D12. Test both backends before assuming a shader is correct. On Windows during development, force one or the other with:

```powershell
$env:SDL_GPU_DRIVER = "vulkan";     .\yellowtail.exe
$env:SDL_GPU_DRIVER = "direct3d12"; .\yellowtail.exe
```

Ship with both working.
