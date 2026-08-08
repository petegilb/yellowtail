//
// Created by Peter Gilbert on 8/8/26.
//

#ifndef YELLOWTAIL_REPLICATIONMANAGER_H
#define YELLOWTAIL_REPLICATIONMANAGER_H

#include <cstdint>

#include <SDL3/SDL_stdinc.h>

namespace ytail {
    class Engine;
    class NetPeer;

    // Snapshot replication. The host captures world state each tick and sends it delta-compressed
    // against the last snapshot each client acknowledged; clients buffer snapshots and interpolate
    // between them. Owned by Engine, inert until bind() supplies a peer.
    class ReplicationManager {
    public:
        void attach(Engine* inEngine) { engine = inEngine; }
        void bind(NetPeer* inPeer) { peer = inPeer; }
        [[nodiscard]] bool isBound() const { return engine != nullptr && peer != nullptr; }

        // Runs before the physics step, so kinematic targets apply to the step that follows.
        void applyReceived(Uint64 tick);
        // Runs after the world's fixed tick and its deferred flush.
        void capture(Uint64 tick);
        void onMessage(uint32_t connection, const void* data, uint32_t size);
        void drawDebugUI();

    private:
        Engine* engine = nullptr;
        NetPeer* peer = nullptr;
    };
} // ytail

#endif //YELLOWTAIL_REPLICATIONMANAGER_H
