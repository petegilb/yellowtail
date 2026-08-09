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
    // Poses are quantized, and deltas compare these integers rather than the source
    // floats. Raw floats jitter in their low bits and never compare equal
    // reference: https://www.gafferongames.com/post/snapshot_compression/
    struct QuantizedPose {
        int32_t position[3] = { 0, 0, 0 };
        uint32_t rotation = 0;

        bool operator==(const QuantizedPose& other) const = default;
    };

    QuantizedPose quantizePose(const glm::vec3& position, const glm::quat& rotation);
    void dequantizePose(const QuantizedPose& pose, glm::vec3& outPosition, glm::quat& outRotation);

    // Bits needed to hold every value in [min, max].
    constexpr int bitsRequired(const int32_t min, const int32_t max) {
        if (min >= max) return 0;
        const uint64_t range = static_cast<uint64_t>(static_cast<int64_t>(max) - min);
        int bits = 0;
        while ((1ull << bits) <= range) ++bits;
        return bits;
    }

    // Section markers, so read/write drift is caught instead of corrupting later fields.
    inline constexpr bool NetSerializeChecks = true;
    inline constexpr uint32_t CheckRemoved = 0x5A;

    // Bits accumulate in a 64-bit scratch and spill to the buffer a 32-bit word at a time, so a
    // field costs one shift-or rather than a write per bit. Storage is uint32 for that reason:
    // the buffer must be word aligned and a whole number of words.
    // reference: https://www.gafferongames.com/post/reading_and_writing_packets/
    class BitWriter {
    public:
        BitWriter(uint32_t* inWords, int wordCount);

        void writeBits(uint32_t value, int bitCount);
        void writeBool(bool value);
        void writeUInt32(uint32_t value);
        void writeInt32(int32_t value);
        // 7 bits of payload per byte, high bit continues. Small ids cost one byte.
        void writeVarUInt(uint32_t value);
        void writePose(const QuantizedPose& pose);

        // Spills the partial word still in scratch. Call once before reading the bytes out.
        void flush();

        [[nodiscard]] int getBytesWritten() const { return (bitsWritten + 7) / 8; }
        [[nodiscard]] bool hasOverflowed() const { return overflowed; }

        // Matching surface with BitReader, so one templated function serializes both directions.
        static constexpr bool IsWriting = true;
        void serializeBool(bool& value) { writeBool(value); }
        void serializeVarUInt(uint32_t& value) { writeVarUInt(value); }
        void serializeInt(int32_t& value, int32_t min, int32_t max);
        void serializePose(QuantizedPose& pose) { writePose(pose); }
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
        uint32_t readUInt32();
        int32_t readInt32();
        uint32_t readVarUInt();
        QuantizedPose readPose();

        // Set once a read runs past the end, or a check marker did not match. Every read after that
        // returns 0, so a truncated or hostile packet is discarded rather than acted on.
        [[nodiscard]] bool hasOverflowed() const { return overflowed; }

        static constexpr bool IsWriting = false;
        void serializeBool(bool& value) { value = readBool(); }
        void serializeVarUInt(uint32_t& value) { value = readVarUInt(); }
        void serializeInt(int32_t& value, int32_t min, int32_t max);
        void serializePose(QuantizedPose& pose) { pose = readPose(); }
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
