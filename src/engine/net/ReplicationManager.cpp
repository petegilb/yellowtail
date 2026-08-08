//
// Created by Peter Gilbert on 8/8/26.
//

#include "ReplicationManager.h"

#include "NetPeer.h"
#include "engine/Engine.h"

namespace ytail {
    void ReplicationManager::applyReceived(Uint64 tick) {
        if (!isBound()) return;
    }

    void ReplicationManager::capture(Uint64 tick) {
        if (!isBound()) return;
    }

    void ReplicationManager::onMessage(uint32_t connection, const void* data, uint32_t size) {
        if (!isBound()) return;
    }

    void ReplicationManager::drawDebugUI() {
    }
} // ytail
