//
// Created by Peter Gilbert on 6/28/26.
//

#ifndef YELLOWTAIL_TRANSFORMCOMPONENT_H
#define YELLOWTAIL_TRANSFORMCOMPONENT_H
#include "../Component.h"
#include "glm/vec3.hpp"
#include "glm/gtc/quaternion.hpp"   // defines glm::quat (fwd.hpp only declares it)


namespace ytail {
    class TransformComponent : public Component {
public:
        glm::vec3 position{0.0f};
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
        glm::vec3 scale{1.0f};
    };
} // ytail

#endif //YELLOWTAIL_TRANSFORMCOMPONENT_H