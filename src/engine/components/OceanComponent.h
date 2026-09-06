//
// Created by Peter Gilbert on 8/14/26.
//

#ifndef YELLOWTAIL_OCEANCOMPONENT_H
#define YELLOWTAIL_OCEANCOMPONENT_H

#include "../Component.h"
#include "../ocean/OceanSettings.h"

namespace ytail {
    // Marks a scene as having an ocean and carries its settings. The engine draws the first one it
    // finds and feeds the same settings to the CPU sampler, so the drawn surface and the one
    // physics feels are the same surface by construction.
    //
    // The entity's transform is ignored: the ocean is global and infinite, and its height comes
    // from OceanSettings::seaLevel. Parenting one to something that moves would do nothing.
    class OceanComponent : public Component {
    public:
        ocean::OceanSettings settings;

        static constexpr const char* SerialId = "ocean";
        void serialize(Archive& ar) override;
        [[nodiscard]] const char* serialId() const override { return SerialId; }

        [[nodiscard]] const char* getTypeName() const override { return "Ocean"; }
        void drawInspector() override;

        // Recompute the cascade band edges from the patch sizes. They are derived, so they are
        // neither saved nor edited directly; anything that moves a patch size calls this.
        void rebuildCascadeBands();
    };
} // ytail

#endif //YELLOWTAIL_OCEANCOMPONENT_H
