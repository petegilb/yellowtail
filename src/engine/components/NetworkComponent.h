//
// Created by Peter Gilbert on 8/8/26.
//

#ifndef SEABREAM_NETWORKCOMPONENT_H
#define SEABREAM_NETWORKCOMPONENT_H

#include <cstdint>

#include "../Component.h"

namespace ytail {
    enum class NetAuthority : uint8_t { Host, Owner, Remote };

    // Marks an entity for replication. Both ids are assigned at runtime by the host, so nothing
    // here is saved with the scene.
    class NetworkComponent : public Component {
    public:
        uint32_t netId = 0;
        // Host-assigned peer number, 0 = the host. Connection handles are host-local.
        uint32_t ownerPeerId = 0;
        NetAuthority authority = NetAuthority::Host;

        [[nodiscard]] bool isAuthoritative() const { return authority != NetAuthority::Remote; }

        static constexpr const char* SerialId = "network";
        [[nodiscard]] const char* serialId() const override { return SerialId; }

        [[nodiscard]] const char* getTypeName() const override { return "Network"; }
        void drawInspector() override;
    };
} // ytail

#endif //SEABREAM_NETWORKCOMPONENT_H
