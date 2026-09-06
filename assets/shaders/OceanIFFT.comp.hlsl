#include "OceanCommon.hlsli"

// One axis of a 2D inverse FFT, transforming both packed complex slots at once.
//
// Stockham autosort rather than Cooley-Tukey: the output lands in natural order, so there is no
// bit-reversal permutation pass. A whole row (or column) fits in groupshared memory at N = 256, so
// all eight stages run inside a single dispatch with barriers between them instead of one dispatch
// per stage.
//
// No 1/N normalization anywhere. The surface is literally sum over k of h(k) e^(ik.x), which is
// the unnormalized inverse transform, and the physical amplitude already lives in the spectrum's
// per-bin variance.

Texture2DArray<float4> spectrumIn : register(t0, space0);
RWTexture2D<float4> spectrumOut : register(u0, space1);

// Two buffers to ping-pong between stages: Stockham reads and writes different indices, so it
// cannot work in place.
groupshared float4 gsData[2][OCEAN_FFT_SIZE];

[numthreads(OCEAN_FFT_SIZE / 2, 1, 1)]
void main(uint3 groupId : SV_GroupID, uint threadId : SV_GroupThreadID)
{
    const uint lineIndex = groupId.y;
    const uint tid = threadId.x;
    const uint halfSize = OCEAN_FFT_SIZE / 2;

    // fftAxis picks whether this group owns a row or a column, which is what lets one shader do
    // both passes. The column pass reads with a stride; at this size that costs less than the
    // separate transpose pass and texture it would replace.
    const uint2 lowIndex = fftAxis == 0 ? uint2(tid, lineIndex) : uint2(lineIndex, tid);
    const uint2 highIndex = fftAxis == 0 ? uint2(tid + halfSize, lineIndex)
                                         : uint2(lineIndex, tid + halfSize);

    gsData[0][tid] = spectrumIn.Load(int4(int2(lowIndex), int(cascadeIndex), 0));
    gsData[0][tid + halfSize] = spectrumIn.Load(int4(int2(highIndex), int(cascadeIndex), 0));
    GroupMemoryBarrierWithGroupSync();

    uint src = 0;
    for (uint stage = 0u; stage < OCEAN_FFT_LOG2; ++stage)
    {
        const uint subSize = 1u << stage;         // width of the sub-transforms feeding this stage
        const uint blockIndex = tid / subSize;    // which sub-transform this thread belongs to
        const uint offset = tid & (subSize - 1u); // position inside it

        // Stockham's read pair is always (tid, tid + N/2); only the write positions move.
        const uint writeLow = tid + blockIndex * subSize;
        const uint writeHigh = writeLow + subSize;

        // Positive exponent: this is the inverse transform.
        const float angle = kPi * float(offset) / float(subSize);
        float sinAngle, cosAngle;
        sincos(angle, sinAngle, cosAngle);
        const float2 twiddle = float2(cosAngle, sinAngle);

        const float4 a = gsData[src][tid];
        const float4 b = gsData[src][tid + halfSize];

        // Both packed slots ride the same butterfly, which is the whole point of packing them.
        const float2 twiddledLow = complexMul(twiddle, b.xy);
        const float2 twiddledHigh = complexMul(twiddle, b.zw);

        const uint dst = src ^ 1u;
        gsData[dst][writeLow] = float4(a.xy + twiddledLow, a.zw + twiddledHigh);
        gsData[dst][writeHigh] = float4(a.xy - twiddledLow, a.zw - twiddledHigh);

        src = dst;
        GroupMemoryBarrierWithGroupSync();
    }

    spectrumOut[lowIndex] = gsData[src][tid];
    spectrumOut[highIndex] = gsData[src][tid + halfSize];
}
