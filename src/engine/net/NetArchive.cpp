//
// Created by Peter Gilbert on 8/9/26.
//

#include "NetArchive.h"

#include <algorithm>
#include <cmath>

#include <SDL3/SDL.h>

#include <glm/common.hpp>
#include <glm/geometric.hpp>

namespace ytail::net {
    namespace {
        // Any component that is not the largest is bounded by 1/sqrt(2): if |b| <= |a| and
        // a^2 + b^2 <= 1 then 2b^2 <= 1. So the three we keep always fit this range.
        constexpr float SmallestThreeRange = 0.70710678f;
        // 2 bits for the dropped index + 3 * 10 fills a uint32 exactly.
        constexpr int RotationComponentBits = 10;

        // 1ull, not 1u: shifting a 32-bit literal by 32 is undefined.
        uint32_t quantizeRange(const float value, const float range, const int bits) {
            const float maxQuantized = static_cast<float>((1ull << bits) - 1ull);
            const float normalized = (glm::clamp(value, -range, range) / range + 1.0f) * 0.5f;
            return static_cast<uint32_t>(std::lround(normalized * maxQuantized));
        }

        float dequantizeRange(const uint32_t quantized, const float range, const int bits) {
            const float maxQuantized = static_cast<float>((1ull << bits) - 1ull);
            return ((static_cast<float>(quantized) / maxQuantized) * 2.0f - 1.0f) * range;
        }

        uint32_t packRotation(const glm::quat& rotation) {
            // normalize() on a zero quaternion yields NaN, which would poison every comparison below.
            const float lengthSquared = glm::dot(rotation, rotation);
            const glm::quat normalized = lengthSquared > 1e-12f
                ? rotation * (1.0f / std::sqrt(lengthSquared))
                : glm::quat::wxyz(1.0f, 0.0f, 0.0f, 0.0f);
            const float components[4] = { normalized.x, normalized.y, normalized.z, normalized.w };

            int largest = 0;
            for (int i = 1; i < 4; ++i) {
                if (std::abs(components[i]) > std::abs(components[largest])) largest = i;
            }
            // q and -q are the same rotation, so force the dropped component positive and it can be
            // rebuilt from the other three without storing its sign.
            const float sign = components[largest] < 0.0f ? -1.0f : 1.0f;

            uint32_t packed = static_cast<uint32_t>(largest);
            int shift = 2;
            for (int i = 0; i < 4; ++i) {
                if (i == largest) continue;
                packed |= quantizeRange(components[i] * sign, SmallestThreeRange, RotationComponentBits) << shift;
                shift += RotationComponentBits;
            }
            return packed;
        }

        glm::quat unpackRotation(const uint32_t packed) {
            const int largest = static_cast<int>(packed & 0x3u);
            constexpr uint32_t componentMask = (1u << RotationComponentBits) - 1u;

            float components[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            float sumSquares = 0.0f;
            int shift = 2;
            for (int i = 0; i < 4; ++i) {
                if (i == largest) continue;
                components[i] = dequantizeRange((packed >> shift) & componentMask,
                                                SmallestThreeRange, RotationComponentBits);
                sumSquares += components[i] * components[i];
                shift += RotationComponentBits;
            }
            components[largest] = std::sqrt(glm::max(0.0f, 1.0f - sumSquares));

            return glm::quat::wxyz(components[3], components[0], components[1], components[2]);
        }
    }

    namespace {
        int32_t quantizeAxis(const float value, const float unitsPerUnit,
                             const int32_t min, const int32_t max) {
            const long rounded = std::lround(value * unitsPerUnit);
            return static_cast<int32_t>(std::clamp<long>(rounded, min, max));
        }
    }

    QuantizedState quantizeState(const glm::vec3& position, const glm::quat& rotation,
                                 const glm::vec3& linear, const glm::vec3& angular, const bool atRest) {
        QuantizedState state;
        for (int axis = 0; axis < 3; ++axis) {
            state.position[axis] = quantizeAxis(position[axis], PositionUnitsPerMeter, PositionMin, PositionMax);
        }
        state.rotation = packRotation(rotation);
        state.atRest = atRest;
        // Left at zero when at rest, so two settled bodies compare equal and stay off the wire.
        if (!atRest) {
            for (int axis = 0; axis < 3; ++axis) {
                state.linear[axis] = quantizeAxis(linear[axis], LinearUnitsPerMeterPerSecond, LinearMin, LinearMax);
                state.angular[axis] = quantizeAxis(angular[axis], AngularUnitsPerRadianPerSecond, AngularMin, AngularMax);
            }
        }
        return state;
    }

    void dequantizeState(const QuantizedState& state, glm::vec3& outPosition, glm::quat& outRotation,
                         glm::vec3& outLinear, glm::vec3& outAngular) {
        for (int axis = 0; axis < 3; ++axis) {
            outPosition[axis] = static_cast<float>(state.position[axis]) / PositionUnitsPerMeter;
            outLinear[axis] = static_cast<float>(state.linear[axis]) / LinearUnitsPerMeterPerSecond;
            outAngular[axis] = static_cast<float>(state.angular[axis]) / AngularUnitsPerRadianPerSecond;
        }
        outRotation = unpackRotation(state.rotation);
    }

    BitWriter::BitWriter(uint32_t* inWords, const int wordCount)
        : words(inWords), numBits(wordCount * 32) {}

    void BitWriter::writeBits(uint32_t value, const int bitCount) {
        if (bitCount <= 0 || bitCount > 32) return;
        if (bitsWritten + bitCount > numBits) {
            overflowed = true;
            return;
        }
        value &= static_cast<uint32_t>((1ull << bitCount) - 1ull);
        scratch |= static_cast<uint64_t>(value) << scratchBits;
        scratchBits += bitCount;
        if (scratchBits >= 32) {
            words[wordIndex++] = static_cast<uint32_t>(scratch);
            scratch >>= 32;
            scratchBits -= 32;
        }
        bitsWritten += bitCount;
    }

    void BitWriter::flush() {
        if (scratchBits <= 0) return;
        words[wordIndex++] = static_cast<uint32_t>(scratch);
        scratch = 0;
        scratchBits = 0;
    }

    void BitWriter::writeBool(const bool value) { writeBits(value ? 1u : 0u, 1); }

    void BitWriter::writeVarUInt(uint32_t value) {
        do {
            const uint32_t chunk = value & 0x7Fu;
            value >>= 7;
            writeBits(chunk | (value != 0 ? 0x80u : 0u), 8);
        } while (value != 0);
    }

    void BitWriter::serializeInt(int32_t& value, const int32_t min, const int32_t max) {
        const int bits = bitsRequired(min, max);
        if (bits == 0) return;
        const int32_t clamped = std::clamp(value, min, max);
        if (clamped != value) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Net value %d clamped to [%d, %d]", value, min, max);
        }
        writeBits(static_cast<uint32_t>(static_cast<int64_t>(clamped) - min), bits);
    }

    void BitWriter::serializeCheck(const uint32_t marker) {
        if (NetSerializeChecks) writeBits(marker, 8);
    }

    BitReader::BitReader(const uint32_t* inWords, const int byteCount)
        : words(inWords), numBits(byteCount > 0 ? byteCount * 8 : 0) {
        if (byteCount <= 0) overflowed = true;
    }

    uint32_t BitReader::readBits(const int bitCount) {
        if (bitCount <= 0 || bitCount > 32) return 0;
        if (bitsRead + bitCount > numBits) {
            overflowed = true;
            return 0;
        }
        bitsRead += bitCount;
        if (scratchBits < bitCount) {
            scratch |= static_cast<uint64_t>(words[wordIndex++]) << scratchBits;
            scratchBits += 32;
        }
        const auto value = static_cast<uint32_t>(scratch & ((1ull << bitCount) - 1ull));
        scratch >>= bitCount;
        scratchBits -= bitCount;
        return value;
    }

    bool BitReader::readBool() { return readBits(1) != 0; }

    uint32_t BitReader::readVarUInt() {
        uint32_t value = 0;
        for (int shift = 0; shift < 35; shift += 7) {
            const uint32_t chunk = readBits(8);
            if (overflowed) return 0;
            value |= (chunk & 0x7Fu) << shift;
            if ((chunk & 0x80u) == 0) break;
        }
        return value;
    }

    void BitReader::serializeInt(int32_t& value, const int32_t min, const int32_t max) {
        const int bits = bitsRequired(min, max);
        if (bits == 0) {
            value = min;
            return;
        }
        value = static_cast<int32_t>(static_cast<int64_t>(readBits(bits)) + min);
    }

    void BitReader::serializeCheck(const uint32_t marker) {
        if (!NetSerializeChecks) return;
        if (readBits(8) != marker) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Net serialize check failed (expected 0x%02X)", marker);
            overflowed = true;
        }
    }
} // ytail::net
