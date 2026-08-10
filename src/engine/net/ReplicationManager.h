//
// Created by Peter Gilbert on 8/8/26.
//

#ifndef YELLOWTAIL_REPLICATIONMANAGER_H
#define YELLOWTAIL_REPLICATIONMANAGER_H

#include <array>
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <vector>

#include <SDL3/SDL_stdinc.h>

#include "NetArchive.h"
#include "engine/Entity.h"

namespace ytail {
    class Engine;
    class DebugDraw;
}

namespace ytail::net {
    class NetPeer;

    // Bytes over a sampling window, turned into a rate for the debug panel.
    struct BandwidthMeter {
        int bytesThisWindow = 0;
        float bytesPerSecond = 0.0f;
        Uint64 windowStartMs = 0;

        void add(const int bytes) { bytesThisWindow += bytes; }
    };

    struct EntitySnapshot {
        uint32_t netId = 0;
        // The host's EntityId. Scene loading preserves ids, so a client can match this to its own
        // copy. TODO only needed the first time a client sees a netId; move it into spawn records
        // along with the component data a runtime-spawned entity needs.
        uint32_t hostEntityId = 0;
        uint32_t ownerPeerId = 0;
        QuantizedPose pose;
    };

    struct OwnedPose {
        QuantizedPose previous;
        QuantizedPose current;
        Uint64 arrivedTick = 0;
    };

    // Kept sorted by netId so delta-ing two snapshots is a linear merge, not a lookup per entity.
    struct WorldSnapshot {
        Uint64 tick = 0;
        std::vector<EntitySnapshot> entities;

        [[nodiscard]] const EntitySnapshot* find(uint32_t netId) const;
    };

    // A client gets one full snapshot reliably, then nothing until it acks
    struct ClientSyncState {
        uint32_t peerId = 0;
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
        ReplicationManager() = default;
        ReplicationManager(Engine* inEngine, NetPeer* inPeer);
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
        // Wire markers at the newest snapshot pose, with a line to where the entity actually is.
        // The gap between them is the interpolation delay made visible.
        void drawSnapshotGhosts(DebugDraw& debug) const;
        bool showGhosts = false;

        // Host only. Hands an entity to a peer, or back to the host with peerId 0. The change
        // reaches everyone through the snapshot, which repeats it until acked.
        void setOwner(EntityId id, uint32_t peerId);
        [[nodiscard]] uint32_t getLocalPeerId() const { return localPeerId; }
        // Host only: the peer number assigned to a connection, or 0 if it has none yet.
        [[nodiscard]] uint32_t getPeerId(uint32_t connection) const;

        // Fixed ticks between snapshots: 6 at 60Hz is 10 per second.
        static constexpr Uint64 SnapshotSendInterval = 6;
        // Owned poses go out faster than snapshots so the host's physics sees smooth motion from
        // them, but not every tick: the host downsamples to the snapshot rate when it rebroadcasts.
        static constexpr Uint64 ClientStateSendInterval = 2;
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
        // Client to host: the snapshot ack plus poses for entities this peer owns.
        void onClientStateMessage(uint32_t connection, int size);
        void sendClientState();
        void onWelcomeMessage(int size);
        void sendWelcome(uint32_t connection, uint32_t peerId);
        // Recomputes each entity's authority from its owner and who we are.
        void refreshAuthority();
        [[nodiscard]] uint32_t ownerPeerIdFor(uint32_t netId) const;
        void advanceClock();
        // Blends the two snapshots straddling playbackTick onto the world.
        void applyInterpolated(float deltaTime);
        // Host side: drives entities a client owns from the pose that client reported.
        void applyOwnedPoses(Uint64 tick, float deltaTime);
        // The local entity for a netId, or NULL_ENTITY if this peer has no copy of it.
        EntityId resolveEntity(uint32_t netId, uint32_t hostEntityId);
        void resetReceivedState();

        [[nodiscard]] const WorldSnapshot* findSent(Uint64 tick) const;
        [[nodiscard]] const WorldSnapshot* findReceived(Uint64 tick) const;
        void sampleBandwidth();
        void drawEntityTable();

        Engine* engine = nullptr;
        NetPeer* peer = nullptr;
        // The tick applyReceived last ran, so messages decoded between fixed steps can be stamped.
        Uint64 localTick = 0;

        // Reused so a snapshot costs no allocation, and word sized because BitWriter/BitReader
        // spill whole words. receiveWords also pads a message up to a word boundary.
        std::vector<uint32_t> sendWords;
        std::vector<uint32_t> receiveWords;

        uint32_t nextNetId = 1;
        uint32_t nextPeerId = 1;
        std::deque<WorldSnapshot> sentHistory;
        std::unordered_map<uint32_t, ClientSyncState> clients;
        // Latest pose a peer reported for an entity it owns, applied on the host each tick.
        std::unordered_map<uint32_t, OwnedPose> ownedPoseByNetId;

        uint32_t localPeerId = 0;

        std::deque<WorldSnapshot> receivedHistory;
        std::unordered_map<uint32_t, EntityId> entityByNetId;
        Uint64 newestReceivedTick = 0;
        double playbackTick = 0.0;
        bool clockStarted = false;

        int lastSnapshotEntities = 0;
        int droppedForMissingBaseline = 0;
        int lastReceivedChanged = 0;
        // Drift sawtooths by one send interval, so the panel shows a smoothed value and a plot
        // rather than a number that is unreadable at frame rate.
        float smoothedDrift = 0.0f;
        std::array<float, 120> driftHistory{};
        int driftHistoryIndex = 0;
        BandwidthMeter sentBytes;
        BandwidthMeter receivedBytes;
    };
} // ytail::net

#endif //YELLOWTAIL_REPLICATIONMANAGER_H
