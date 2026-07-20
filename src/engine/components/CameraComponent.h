//
// Created by Peter Gilbert on 6/28/26.
//

#ifndef YELLOWTAIL_CAMERACOMPONENT_H
#define YELLOWTAIL_CAMERACOMPONENT_H
#include "../Component.h"
#include "glm/mat4x4.hpp"
#include "glm/gtc/matrix_transform.hpp"  // perspective / ortho

namespace ytail {
    // The "lens" half of a camera. Position/orientation (the view matrix) come from
    // the sibling TransformComponent on the same entity; this holds only projection.
    class CameraComponent : public Component {
public:
        float fovYDegrees = 60.0f;
        float nearPlane   = 0.1f;
        float farPlane    = 1000.0f;

        // aspect = viewport width / height. GLM_FORCE_DEPTH_ZERO_TO_ONE (set project-wide via CMake)
        // makes this produce the [0,1] clip-space Z that SDL_GPU expects.
        [[nodiscard]] glm::mat4 projectionMatrix(float aspect) const {
            return glm::perspective(glm::radians(fovYDegrees), aspect, nearPlane, farPlane);
        }

        static constexpr const char* SerialId = "camera";
        void serialize(Archive& ar) override;
        [[nodiscard]] const char* serialId() const override { return SerialId; }

        [[nodiscard]] const char* getTypeName() const override { return "Camera"; }
        void drawInspector() override;
    };
} // ytail

#endif //YELLOWTAIL_CAMERACOMPONENT_H
