//
// Created by Peter Gilbert on 6/28/26.
//

#ifndef YELLOWTAIL_TRANSFORMCOMPONENT_H
#define YELLOWTAIL_TRANSFORMCOMPONENT_H
#include "../Component.h"
#include "glm/vec3.hpp"
#include "glm/mat4x4.hpp"
#include "glm/gtc/quaternion.hpp"        // defines glm::quat (fwd.hpp only declares it)
#include "glm/gtc/matrix_transform.hpp"  // translate / scale


namespace ytail {
    class TransformComponent : public Component {
public:
        // Local position/rotation/scale. Getters return const ref; setters bump the version on change.
        [[nodiscard]] const glm::vec3& getPosition() const { return position; }
        [[nodiscard]] const glm::quat& getRotation() const { return rotation; }
        [[nodiscard]] const glm::vec3& getScale()    const { return scale; }

        void setPosition(const glm::vec3& p) { if (p != position) { position = p; ++localVersion; } }
        void setRotation(const glm::quat& r) { if (r != rotation) { rotation = r; ++localVersion; } }
        void setScale(const glm::vec3& s)    { if (s != scale)    { scale = s;    ++localVersion; } }
        void translate(const glm::vec3& delta) { setPosition(position + delta); }

        // Set rotation from Euler angles in degrees (x = pitch, y = yaw, z = roll).
        void setRotationEuler(const glm::vec3& eulerDegrees) {
            setRotation(glm::quat(glm::radians(eulerDegrees)));
        }
        // Rotation as Euler degrees; one of many equivalent triples for a given orientation.
        [[nodiscard]] glm::vec3 getRotationEuler() const {
            return glm::degrees(glm::eulerAngles(rotation));
        }

        // T * R * S in the entity's own space (relative to its parent). Cheap, computed on demand.
        [[nodiscard]] glm::mat4 localMatrix() const {
            return glm::translate(glm::mat4(1.0f), position)
                 * glm::mat4_cast(rotation)
                 * glm::scale(glm::mat4(1.0f), scale);
        }

        // localMatrix folded through every ancestor. Cached; recomputed lazily when this transform
        // or an ancestor changes. Returns a ref into the cache.
        [[nodiscard]] const glm::mat4& worldMatrix() const;

        // transpose(inverse(world)) for correct normals under non-uniform scale. Cached off the world.
        [[nodiscard]] const glm::mat4& normalMatrix() const;

        // Monotonic counter that changes whenever the world matrix changes (self or any ancestor
        // moved). Consumers store their last-seen value to detect change without hashing.
        [[nodiscard]] Uint64 getWorldVersion() const;

        static constexpr const char* SerialId = "transform";
        void serialize(Archive& ar) override;
        [[nodiscard]] const char* serialId() const override { return SerialId; }

        [[nodiscard]] const char* getTypeName() const override { return "Transform"; }
        void drawInspector() override;

    private:
        glm::vec3 position{0.0f};
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
        glm::vec3 scale{1.0f};
        Uint64 localVersion = 1;

        // Lazy world/normal cache. mutable so the const accessors can refresh it.
        mutable glm::mat4 cachedWorld{1.0f};
        mutable glm::mat4 cachedNormal{1.0f};
        mutable Uint64 cachedLocalVer = 0;       // localVersion the cached world was built from
        mutable Uint64 cachedParentWorldVer = 0; // parent worldVersion the cached world was built from
        mutable const TransformComponent* cachedParentXform = nullptr; // parent identity at last build (reparent detection)
        mutable Uint64 worldVersion = 0;         // bumps whenever cachedWorld changes
        mutable bool normalValid = false;

        // Refresh cachedWorld/worldVersion if this transform or its parent moved since last build.
        void ensureWorld() const;
    };
} // ytail

#endif //YELLOWTAIL_TRANSFORMCOMPONENT_H
