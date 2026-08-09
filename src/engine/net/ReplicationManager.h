//
// Created by Peter Gilbert on 8/8/26.
//

#ifndef YELLOWTAIL_REPLICATIONMANAGER_H
#define YELLOWTAIL_REPLICATIONMANAGER_H

#include <cstdint>
#include <deque>
#include <unordered_map>
#include <vector>

#include <SDL3/SDL_stdinc.h>

#include "NetArchive.h"
#include "engine/Entity.h"

namespace ytail {
    class Engine;
}

namespace ytail::net {
    class NetPeer;

    struct EntitySnapshot {
        uint32_t netId = 0;
        uint32_t ownerConnection = 0;
        QuantizedPose pose;
    };

    // Kept sorted by netId so delta-ing two snapshots is a linear merge, not a lookup per entity.
    struct WorldSnapshot {
        Uint64 tick = 0;
        std::vector<EntitySnapshot> entities;

        [[nodiscard]] const EntitySnapshot* find(uint32_t netId) const;
    };

    // A client gets one full snapshot reliably, then nothing until it acks
    struct ClientSyncState {
        Uint64 ackedTick = 0;
        Uint64 pendingBaselineTick = 0;

        int lastSentBytes = 0;
        int lastChangedEntities = 0;
        bool lastSendWasFull = false;
        int fullSnapshotsSent = 0;

        [[nodiscard]] bool awaitingBaseline() const { return pendingBaselineTick != 0; }
    };

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

        // TODO interest management, needed before entity counts get large. Every entity that
        // changed goes to every client right now, so cost scales with moving entities times
        // clients. Quake 3 solves this with PVS culling; without a BSP the cheap equivalent is
        // distance culling plus a per-snapshot byte budget, sending the highest priority entities
        // first (nearest, and longest since last sent so far ones don't starve). That turns "too
        // many entities" into far objects updating late instead of snapshots that fragment and
        // drop. It makes the snapshot per-connection rather than one shared capture, so it is much
        // cheaper to build in than to retrofit.
        // Runs after the world's fixed tick and its deferred flush.
        void capture(Uint64 tick);
        void onMessage(uint32_t connection, const void* data, uint32_t size);
        void onConnected(uint32_t connection);
        void onDisconnected(uint32_t connection);
        void drawDebugUI();

        // Fixed ticks between snapshots: 6 at 60Hz is 10 per second.
        static constexpr Uint64 SnapshotSendInterval = 6;
        // How far behind the newest snapshot a client plays back, in send intervals.
        static constexpr float InterpolationDelayIntervals = 2.0f;
        // Snapshots older than this are dropped from either ring.
        static constexpr size_t MaxSnapshotHistory = 32;

    private:
        void captureHost(Uint64 tick);
        void sendSnapshotTo(uint32_t connection, ClientSyncState& client, const WorldSnapshot& snapshot);
        // Writes snapshot as a delta against baseline, or in full when baseline is null.
        bool writeSnapshot(BitWriter& writer, const WorldSnapshot& snapshot,
                           const WorldSnapshot* baseline, int& outChangedEntities) const;
        void onSnapshotMessage(int size);
        // Client to host: the snapshot ack today, plus input and owned entity poses later.
        void onInputMessage(uint32_t connection, int size);
        void sendInput();
        void advanceClock();
        void resetReceivedState();

        [[nodiscard]] const WorldSnapshot* findSent(Uint64 tick) const;
        [[nodiscard]] const WorldSnapshot* findReceived(Uint64 tick) const;

        Engine* engine = nullptr;
        NetPeer* peer = nullptr;

        // Reused so a snapshot costs no allocation, and word sized because BitWriter/BitReader
        // spill whole words. receiveWords also pads a message up to a word boundary.
        std::vector<uint32_t> sendWords;
        std::vector<uint32_t> receiveWords;

        uint32_t nextNetId = 1;
        std::deque<WorldSnapshot> sentHistory;
        std::unordered_map<uint32_t, ClientSyncState> clients;

        std::deque<WorldSnapshot> receivedHistory;
        std::unordered_map<uint32_t, EntityId> entityByNetId;
        Uint64 newestReceivedTick = 0;
        double playbackTick = 0.0;
        bool clockStarted = false;

        int lastSnapshotEntities = 0;
        int droppedForMissingBaseline = 0;
    };
} // ytail::net

#endif //YELLOWTAIL_REPLICATIONMANAGER_H
