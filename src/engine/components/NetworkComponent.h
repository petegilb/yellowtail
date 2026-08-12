//
// Created by Peter Gilbert on 8/8/26.
//

#ifndef YELLOWTAIL_NETWORKCOMPONENT_H
#define YELLOWTAIL_NETWORKCOMPONENT_H

#include <cstdint>

#include "../Component.h"

namespace ytail {
    // Marks an entity for replication. Both ids are assigned at runtime by the host, so nothing
    // here is saved with the scene.
    class NetworkComponent : public Component {
    public:
        uint32_t netId = 0;
        // Host-assigned peer number: 0 is nobody, the host is 1. Whoever is named here supplies this
        // entity's input on every peer. Connection handles are host-local.
        uint32_t ownerPeerId = 0;

        static constexpr const char* SerialId = "network";
        [[nodiscard]] const char* serialId() const override { return SerialId; }

        [[nodiscard]] const char* getTypeName() const override { return "Network"; }
        void drawInspector() override;
    };
} // ytail

#endif //YELLOWTAIL_NETWORKCOMPONENT_H
