//
// Created by Peter Gilbert on 6/28/26.
//

#ifndef YELLOWTAIL_RENDERCOMPONENT_H
#define YELLOWTAIL_RENDERCOMPONENT_H
#include <vector>

#include "../Component.h"
#include "../render/Material.h"

namespace ytail {
    class RenderComponent : public Component {
public:
        std::vector<std::shared_ptr<Material>> materials;
    };
} // ytail


#endif //YELLOWTAIL_RENDERCOMPONENT_H