// Fragment-stage uniform buffers live in space3.
cbuffer CustomColor : register(b0, space3)
{
    float4 color;   // linear-space RGBA, pushed per draw
};

// a solid uniform-colored fill is needed
float4 main() : SV_Target
{
    return color;
}
