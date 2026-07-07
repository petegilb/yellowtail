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
        glm::vec3 position{0.0f};
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
        glm::vec3 scale{1.0f};

        // Set rotation from Euler angles in degrees (x = pitch, y = yaw, z = roll).
        // Stored internally as a quaternion to avoid gimbal lock / order ambiguity.
        void setRotationEuler(const glm::vec3& eulerDegrees) {
            rotation = glm::quat(glm::radians(eulerDegrees));
        }

        // Rotation as Euler angles in degrees (x = pitch, y = yaw, z = roll).
        // Note: this is one of many equivalent Euler triples for a given orientation,
        // so it won't always match what was passed to setRotationEuler.
        [[nodiscard]] glm::vec3 getRotationEuler() const {
            return glm::degrees(glm::eulerAngles(rotation));
        }

        // Local-to-world transform: T * R * S. Feeds the MVP for rendering, and its
        // inverse is the view matrix when this transform belongs to a camera.
        [[nodiscard]] glm::mat4 modelMatrix() const {
            return glm::translate(glm::mat4(1.0f), position)
                 * glm::mat4_cast(rotation)
                 * glm::scale(glm::mat4(1.0f), scale);
        }

        // Transform for normals: transpose(inverse(model)). Needed so non-uniform scale
        // doesn't skew normals. Returned as mat4 to match the cbuffer; the shader casts
        // it to float3x3.
        [[nodiscard]] glm::mat4 normalMatrix() const {
            return glm::transpose(glm::inverse(modelMatrix()));
        }
    };
} // ytail

#endif //YELLOWTAIL_TRANSFORMCOMPONENT_H