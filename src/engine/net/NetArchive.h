//
// Created by Peter Gilbert on 8/9/26.
//

#ifndef YELLOWTAIL_NETARCHIVE_H
#define YELLOWTAIL_NETARCHIVE_H

#include <cstddef>
#include <cstdint>

#include <glm/vec3.hpp>
#include <glm/gtc/quaternion.hpp>

namespace ytail::net {
    // Bits needed to hold every value in [min, max].
    constexpr int bitsRequired(const int32_t min, const int32_t max) {
        if (min >= max) return 0;
        const uint64_t range = static_cast<uint64_t>(static_cast<int64_t>(max) - min);
        int bits = 0;
        while ((1ull << bits) <= range) ++bits;
        return bits;
    }

    // NOTE: DESIGN LIMIT: the replicated world is a cube of +/- WorldExtentMeters about the origin.
    // Position is range encoded, so a body outside it clamps to the boundary and freezes there for
    // every remote peer while the owner keeps simulating it. Levels must fit, and anything that can
    // leave (knocked off a ledge) needs a kill volume. Growing the world is a one-line change here
    // since every bit width below follows from bitsRequired.
    inline constexpr float WorldExtentMeters = 256.0f;
    // might need to tune this depending on how much positions change when a Jolt body is at rest
    // so delta compression doesn't fire when it doesn't need to
    inline constexpr float PositionUnitsPerMeter = 512.0f;
    inline constexpr int32_t PositionMax = static_cast<int32_t>(WorldExtentMeters * PositionUnitsPerMeter) - 1;
    inline constexpr int32_t PositionMin = -PositionMax - 1;

    // Velocities are bounded well below BodyProperties::maxLinearVelocity: this covers what a body
    // realistically reaches, not the theoretical cap, since every extra bit is paid per update.
    inline constexpr float MaxLinearSpeed = 128.0f;
    inline constexpr float LinearUnitsPerMeterPerSecond = 16.0f;
    inline constexpr int32_t LinearMax = static_cast<int32_t>(MaxLinearSpeed * LinearUnitsPerMeterPerSecond) - 1;
    inline constexpr int32_t LinearMin = -LinearMax - 1;

    inline constexpr float MaxAngularSpeed = 64.0f;
    inline constexpr float AngularUnitsPerRadianPerSecond = 16.0f;
    inline constexpr int32_t AngularMax = static_cast<int32_t>(MaxAngularSpeed * AngularUnitsPerRadianPerSecond) - 1;
    inline constexpr int32_t AngularMin = -AngularMax - 1;

    // State is quantized, and deltas compare these integers rather than the source floats. Raw
    // floats jitter in their low bits and never compare equal.
    // reference: https://www.gafferongames.com/post/snapshot_compression/
    //
    // Velocity travels with the pose because remote peers simulate this body rather than
    // interpolate it, and a body handed a position with no momentum coasts to a halt between
    // updates. atRest carries the two velocities implicitly and keeps settled bodies off the wire.
    // reference: https://www.gafferongames.com/post/state_synchronization/
    struct QuantizedState {
        int32_t position[3] = { 0, 0, 0 };
        uint32_t rotation = 0;
        int32_t linear[3] = { 0, 0, 0 };
        int32_t angular[3] = { 0, 0, 0 };
        bool atRest = false;

        bool operator==(const QuantizedState& other) const = default;
    };

    // Clamps to the wire bounds here rather than in the serializer, so serializeInt's clamp warning
    // stays a genuine programmer-error signal instead of firing per axis per entity per packet.
    QuantizedState quantizeState(const glm::vec3& position, const glm::quat& rotation,
                                 const glm::vec3& linear, const glm::vec3& angular, bool atRest);
    void dequantizeState(const QuantizedState& state, glm::vec3& outPosition, glm::quat& outRotation,
                         glm::vec3& outLinear, glm::vec3& outAngular);

    // One definition for both directions, so a read can never drift from a write.
    template<typename Stream>
    void serializeState(Stream& stream, QuantizedState& state) {
        for (int32_t& axis : state.position) stream.serializeInt(axis, PositionMin, PositionMax);
        stream.serializeBits(state.rotation, 32);
        stream.serializeBool(state.atRest);
        if (state.atRest) {
            // The reader has to zero these itself; nothing was written for them.
            for (int axis = 0; axis < 3; ++axis) {
                state.linear[axis] = 0;
                state.angular[axis] = 0;
            }
            return;
        }
        for (int32_t& axis : state.linear) stream.serializeInt(axis, LinearMin, LinearMax);
        for (int32_t& axis : state.angular) stream.serializeInt(axis, AngularMin, AngularMax);
    }

    // What a field will cost before it is written, so a sender can fill a packet to a byte budget
    // rather than to a worst case. Both mirror the writers above by hand and have to move with them.
    constexpr int varUIntBits(uint32_t value) {
        int bytes = 1;
        while (value >= 0x80u) {
            value >>= 7;
            ++bytes;
        }
        return bytes * 8;
    }

    constexpr int stateBits(const QuantizedState& state) {
        constexpr int poseBits = 3 * bitsRequired(PositionMin, PositionMax) + 32 + 1;
        if (state.atRest) return poseBits;
        return poseBits + 3 * bitsRequired(LinearMin, LinearMax) + 3 * bitsRequired(AngularMin, AngularMax);
    }

    // One tick of a player's input, as an opaque bit field: the engine moves it around and the game
    // decides what the bits mean. Replicated rather than derived, because a peer simulating someone
    // else's body has to know what is driving it or the body coasts to a halt between updates.
    struct NetInputFrame {
        uint32_t buttons = 0;

        // Bits meaning "pressed on this tick" rather than "held", set once by the game. A frame
        // carried forward to stand in for a tick we have no input for keeps the held bits and drops
        // these, so an edge fires on the one tick it belongs to however long it is repeated. Without
        // it a peer extrapolating a player jumps again every tick until their next frame lands.
        inline static uint32_t edgeButtons = 0;

        bool operator==(const NetInputFrame& other) const = default;
    };

    inline constexpr int NetInputButtonBits = 8;
    // Every packet repeats the last few frames, so a lost packet costs nothing as long as one of
    // the next few arrives. Same trick as deterministic lockstep.
    inline constexpr int NetInputRedundancy = 16;

    // Section markers, so read/write drift is caught instead of corrupting later fields.
    inline constexpr bool NetSerializeChecks = true;
    inline constexpr uint32_t CheckInput = 0xA5;

    // Bits accumulate in a 64-bit scratch and spill to the buffer a 32-bit word at a time, so a
    // field costs one shift-or rather than a write per bit. Storage is uint32 for that reason:
    // the buffer must be word aligned and a whole number of words.
    // reference: https://www.gafferongames.com/post/reading_and_writing_packets/
    class BitWriter {
    public:
        BitWriter(uint32_t* inWords, int wordCount);

        void writeBits(uint32_t value, int bitCount);
        void writeBool(bool value);
        // 7 bits of payload per byte, high bit continues. Small ids cost one byte.
        void writeVarUInt(uint32_t value);

        // Spills the partial word still in scratch. Call once before reading the bytes out.
        void flush();

        [[nodiscard]] int getBytesWritten() const { return (bitsWritten + 7) / 8; }
        // For a budget: the writer cannot rewind, so a caller has to know the cost before writing.
        [[nodiscard]] int getBitsWritten() const { return bitsWritten; }
        [[nodiscard]] bool hasOverflowed() const { return overflowed; }

        // Matching surface with BitReader, so one templated function serializes both directions.
        static constexpr bool IsWriting = true;
        void serializeBool(bool& value) { writeBool(value); }
        void serializeBits(uint32_t& value, const int bitCount) { writeBits(value, bitCount); }
        void serializeVarUInt(uint32_t& value) { writeVarUInt(value); }
        void serializeInt(int32_t& value, int32_t min, int32_t max);
        void serializeCheck(uint32_t marker);

    private:
        uint32_t* words;
        uint64_t scratch = 0;
        int numBits;
        int bitsWritten = 0;
        int scratchBits = 0;
        int wordIndex = 0;
        bool overflowed = false;
    };

    class BitReader {
    public:
        // byteCount is the logical payload size, but the buffer behind it must still be a whole
        // number of words, since the last partial word is loaded in full.
        BitReader(const uint32_t* inWords, int byteCount);

        uint32_t readBits(int bitCount);
        bool readBool();
        uint32_t readVarUInt();

        // Set once a read runs past the end, or a check marker did not match. Every read after that
        // returns 0, so a truncated or hostile packet is discarded rather than acted on.
        [[nodiscard]] bool hasOverflowed() const { return overflowed; }

        static constexpr bool IsWriting = false;
        void serializeBool(bool& value) { value = readBool(); }
        void serializeBits(uint32_t& value, const int bitCount) { value = readBits(bitCount); }
        void serializeVarUInt(uint32_t& value) { value = readVarUInt(); }
        void serializeInt(int32_t& value, int32_t min, int32_t max);
        void serializeCheck(uint32_t marker);

    private:
        const uint32_t* words;
        uint64_t scratch = 0;
        int numBits;
        int bitsRead = 0;
        int scratchBits = 0;
        int wordIndex = 0;
        bool overflowed = false;
    };
} // ytail::net

#endif //YELLOWTAIL_NETARCHIVE_H
