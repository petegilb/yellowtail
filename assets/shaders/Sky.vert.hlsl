struct Input
{
    float3 position : TEXCOORD0;   // location 0  (aPos)
    float3 normal   : TEXCOORD1;   // location 1  (aNormal)
    float2 uv       : TEXCOORD2;   // location 2  (aTexCoords)
};

struct Output
{
    float3 direction : TEXCOORD0;
    float4 position  : SV_Position;
};

// Vertex-stage uniform buffers live in space1 for SDL_shadercross.
// projection * view with the view's translation stripped, so the dome stays centred on the
// camera and never gets nearer to one side of itself.
cbuffer SkyCamera : register(b0, space1)
{
    float4x4 skyViewProj;
};

Output main(Input input)
{
    Output output;

    // The mesh only carries directions, so its local position is the view ray. Sky.frag
    // normalizes it, which is why the shape of the mesh doesn't matter.
    output.direction = input.position;

    const float4 clip = mul(skyViewProj, float4(input.position, 1.0f));
    // z == w makes every sky pixel land exactly on the far plane, so the LESS_OR_EQUAL depth
    // test keeps it only where nothing else drew.
    output.position = clip.xyww;

    return output;
}
