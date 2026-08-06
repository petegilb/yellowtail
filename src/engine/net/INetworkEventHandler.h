//
// Created by Peter Gilbert on 8/6/26.
//

#ifndef YELLOWTAIL_INETWORKEVENTHANDLER_H
#define YELLOWTAIL_INETWORKEVENTHANDLER_H

#include <cstdint>

namespace ytail {
    // Receives connection lifecycle and message events from NetPeer. The game implements this now;
    // the replication system implements it later. The data passed to onMessage is valid only for the
    // duration of the call, so copy anything you keep.
    class INetworkEventHandler {
    public:
        virtual ~INetworkEventHandler() = default;
        virtual void onConnected(uint32_t connection) {}
        virtual void onDisconnected(uint32_t connection) {}
        virtual void onMessage(uint32_t connection, const void* data, uint32_t size) {}
    };
} // ytail

#endif //YELLOWTAIL_INETWORKEVENTHANDLER_H
