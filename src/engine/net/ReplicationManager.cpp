//
// Created by Peter Gilbert on 8/8/26.
//

#include "ReplicationManager.h"

#include <algorithm>
#include <cstring>

#include <SDL3/SDL.h>

#include "imgui.h"

#include "NetPeer.h"
#include "engine/Engine.h"
#include "engine/World.h"
#include "engine/components/NetworkComponent.h"
#include "engine/components/TransformComponent.h"

namespace ytail::net {
    // Steam fragments unreliable messages, so this is not an MTU limit. It only has to be
    // larger than any snapshot we build.
    constexpr int SendBufferWords = 16 * 1024;
    constexpr int SoftPacketBytes = 1200;

    // One definition for both directions, so a field can never be added to the write side and
    // forgotten on the read side.
    template<typename Stream>
    void serializeEntity(Stream& stream, EntitySnapshot& entity) {
        stream.serializeVarUInt(entity.netId);
        stream.serializeVarUInt(entity.ownerConnection);
        stream.serializePose(entity.pose);
    }

    const EntitySnapshot* WorldSnapshot::find(const uint32_t netId) const {
        const auto it = std::lower_bound(entities.begin(), entities.end(), netId,
            [](const EntitySnapshot& entity, const uint32_t id) { return entity.netId < id; });
        if (it == entities.end() || it->netId != netId) return nullptr;
        return &*it;
    }

    void ReplicationManager::capture(const Uint64 tick) {
        if (peer == nullptr) return;
        if (!isBound() || !peer->isActive()) return;
        if (peer->isHosting()) {
            captureHost(tick);
        } else if (tick % SnapshotSendInterval == 0) {
            sendInput();
        } else {
            return;
        }
        peer->flush();
    }

    void ReplicationManager::captureHost(const Uint64 tick) {
        // History is kept at send granularity, not per tick: a client's acked baseline has to
        // survive long enough for a slow reliable transfer to land and be acknowledged.
        if (tick % SnapshotSendInterval != 0) return;

        World& world = engine->getWorld();

        WorldSnapshot snapshot;
        snapshot.tick = tick;
        world.each<NetworkComponent, TransformComponent>(
            [&](const EntityId, NetworkComponent& network, const TransformComponent& transform) {
                if (network.netId == 0) network.netId = nextNetId++;
                EntitySnapshot entity;
                entity.netId = network.netId;
                entity.ownerConnection = network.ownerConnection;
                entity.pose = quantizePose(transform.getPosition(), transform.getRotation());
                snapshot.entities.push_back(entity);
            });
        std::sort(snapshot.entities.begin(), snapshot.entities.end(),
            [](const EntitySnapshot& left, const EntitySnapshot& right) { return left.netId < right.netId; });

        sentHistory.push_back(std::move(snapshot));
        while (sentHistory.size() > MaxSnapshotHistory) sentHistory.pop_front();

        const std::vector<uint32_t>& connections = peer->getConnections();
        std::erase_if(clients, [&connections](const auto& entry) {
            return std::find(connections.begin(), connections.end(), entry.first) == connections.end();
        });

        const WorldSnapshot& latest = sentHistory.back();
        for (const uint32_t connection : connections) {
            sendSnapshotTo(connection, clients[connection], latest);
        }
    }

    void ReplicationManager::sendSnapshotTo(const uint32_t connection, ClientSyncState& client,
                                            const WorldSnapshot& snapshot) {
        // Still waiting on the reliable full snapshot to be acknowledged
        if (client.awaitingBaseline() && client.ackedTick < client.pendingBaselineTick) return;
        client.pendingBaselineTick = 0;

        const WorldSnapshot* baseline = client.ackedTick == 0 ? nullptr : findSent(client.ackedTick);

        sendWords.resize(SendBufferWords);
        BitWriter writer(sendWords.data(), SendBufferWords);
        writer.writeBits(static_cast<uint32_t>(NetMessageType::Snapshot), 8);
        int changedEntities = 0;
        if (!writeSnapshot(writer, snapshot, baseline, changedEntities)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Snapshot for tick %llu overflowed the send buffer",
                         static_cast<unsigned long long>(snapshot.tick));
            return;
        }
        writer.flush();

        const int size = writer.getBytesWritten();
        lastSnapshotEntities = static_cast<int>(snapshot.entities.size());
        client.lastSentBytes = size;
        client.lastChangedEntities = changedEntities;
        client.lastSendWasFull = baseline == nullptr;

        if (baseline == nullptr) {
            // Reliable so a joining client is guaranteed its baseline. Unreliable would fragment
            // across many packets with no retransmission, and losing any one drops the whole thing.
            peer->sendReliable(connection, sendWords.data(), static_cast<uint32_t>(size));
            client.pendingBaselineTick = snapshot.tick;
            ++client.fullSnapshotsSent;
        } else {
            peer->sendUnreliable(connection, sendWords.data(), static_cast<uint32_t>(size));
        }
    }

    bool ReplicationManager::writeSnapshot(BitWriter& writer, const WorldSnapshot& snapshot,
                                           const WorldSnapshot* baseline, int& outChangedEntities) const {
        writer.writeVarUInt(static_cast<uint32_t>(snapshot.tick));
        writer.writeVarUInt(baseline == nullptr ? 0u : static_cast<uint32_t>(baseline->tick));

        std::vector<const EntitySnapshot*> changed;
        std::vector<uint32_t> removed;
        for (const EntitySnapshot& entity : snapshot.entities) {
            const EntitySnapshot* previous = baseline == nullptr ? nullptr : baseline->find(entity.netId);
            if (previous == nullptr || previous->pose != entity.pose
                || previous->ownerConnection != entity.ownerConnection) {
                changed.push_back(&entity);
            }
        }
        if (baseline != nullptr) {
            for (const EntitySnapshot& entity : baseline->entities) {
                if (snapshot.find(entity.netId) == nullptr) removed.push_back(entity.netId);
            }
        }

        outChangedEntities = static_cast<int>(changed.size());
        writer.writeVarUInt(static_cast<uint32_t>(changed.size()));
        for (const EntitySnapshot* entity : changed) {
            EntitySnapshot copy = *entity;
            serializeEntity(writer, copy);
        }
        writer.serializeCheck(CheckRemoved);
        writer.writeVarUInt(static_cast<uint32_t>(removed.size()));
        for (uint32_t netId : removed) writer.serializeVarUInt(netId);

        return !writer.hasOverflowed();
    }

    void ReplicationManager::onMessage(const uint32_t connection, const void* data, const uint32_t size) {
        if (!isBound() || size < 1) return;
        const auto* bytes = static_cast<const uint8_t*>(data);

        // BitReader loads the trailing partial word in full, so the payload is copied into a word
        // sized buffer rather than read in place off Steam's arbitrary length allocation.
        receiveWords.assign((size + 3) / 4, 0);
        std::memcpy(receiveWords.data(), bytes, size);

        switch (static_cast<NetMessageType>(bytes[0])) {
            case NetMessageType::Snapshot: onSnapshotMessage(static_cast<int>(size)); break;
            case NetMessageType::Input:    onInputMessage(connection, static_cast<int>(size)); break;
            default: break;
        }
    }

    void ReplicationManager::onSnapshotMessage(const int size) {
        BitReader reader(receiveWords.data(), size);
        reader.readBits(8);

        WorldSnapshot snapshot;
        snapshot.tick = reader.readVarUInt();
        const Uint64 baselineTick = reader.readVarUInt();

        // Unreliable delivery can duplicate or reorder, and a second copy would just take a buffer slot.
        if (findReceived(snapshot.tick) != nullptr) return;

        const WorldSnapshot* baseline = baselineTick == 0 ? nullptr : findReceived(baselineTick);
        if (baselineTick != 0 && baseline == nullptr) {
            // The host falls back to a full snapshot once it sees our ack has not moved.
            ++droppedForMissingBaseline;
            return;
        }
        if (baseline != nullptr) snapshot.entities = baseline->entities;

        const uint32_t changedCount = reader.readVarUInt();
        for (uint32_t i = 0; i < changedCount; ++i) {
            EntitySnapshot entity;
            serializeEntity(reader, entity);
            if (reader.hasOverflowed()) return;

            const auto it = std::lower_bound(snapshot.entities.begin(), snapshot.entities.end(), entity.netId,
                [](const EntitySnapshot& existing, const uint32_t id) { return existing.netId < id; });
            if (it != snapshot.entities.end() && it->netId == entity.netId) {
                *it = entity;
            } else {
                snapshot.entities.insert(it, entity);
            }
        }

        reader.serializeCheck(CheckRemoved);
        const uint32_t removedCount = reader.readVarUInt();
        for (uint32_t i = 0; i < removedCount; ++i) {
            uint32_t netId = 0;
            reader.serializeVarUInt(netId);
            if (reader.hasOverflowed()) return;
            const auto it = std::lower_bound(snapshot.entities.begin(), snapshot.entities.end(), netId,
                [](const EntitySnapshot& existing, const uint32_t id) { return existing.netId < id; });
            if (it != snapshot.entities.end() && it->netId == netId) snapshot.entities.erase(it);
        }

        if (reader.hasOverflowed()) return;

        newestReceivedTick = std::max(newestReceivedTick, snapshot.tick);
        receivedHistory.push_back(std::move(snapshot));
        std::sort(receivedHistory.begin(), receivedHistory.end(),
            [](const WorldSnapshot& left, const WorldSnapshot& right) { return left.tick < right.tick; });
        while (receivedHistory.size() > MaxSnapshotHistory) receivedHistory.pop_front();
    }

    void ReplicationManager::onConnected(const uint32_t connection) {
        if (!isBound()) return;
        if (peer->isHosting()) {
            // Fresh state, so the first send is a full snapshot even if this handle was used before.
            clients[connection] = ClientSyncState{};
        } else {
            resetReceivedState();
        }
    }

    void ReplicationManager::onDisconnected(const uint32_t connection) {
        if (!isBound()) return;
        if (peer->isHosting()) {
            clients.erase(connection);
            // TODO destroy entities owned by this peer once ownership is assigned, or they linger
            // on the host and every remaining client.
        } else {
            resetReceivedState();
        }
    }

    void ReplicationManager::resetReceivedState() {
        receivedHistory.clear();
        entityByNetId.clear();
        newestReceivedTick = 0;
        playbackTick = 0.0;
        clockStarted = false;
    }

    void ReplicationManager::onInputMessage(const uint32_t connection, const int size) {
        BitReader reader(receiveWords.data(), size);
        reader.readBits(8);
        const Uint64 ackedTick = reader.readVarUInt();
        if (reader.hasOverflowed() || !peer->isHosting()) return;

        // An ack for a tick we never sent would pin this client on the full snapshot path forever.
        const Uint64 newestSent = sentHistory.empty() ? 0 : sentHistory.back().tick;
        if (ackedTick > newestSent) return;

        ClientSyncState& client = clients[connection];
        client.ackedTick = std::max(client.ackedTick, ackedTick);
    }

    void ReplicationManager::sendInput() {
        sendWords.resize(SendBufferWords);
        BitWriter writer(sendWords.data(), SendBufferWords);
        writer.writeBits(static_cast<uint32_t>(NetMessageType::Input), 8);
        writer.writeVarUInt(static_cast<uint32_t>(newestReceivedTick));
        if (writer.hasOverflowed()) return;
        writer.flush();
        peer->broadcastUnreliable(sendWords.data(), static_cast<uint32_t>(writer.getBytesWritten()));
    }

    void ReplicationManager::applyReceived(Uint64) {
        if (!isBound() || !peer->isActive() || peer->isHosting()) return;
        advanceClock();
    }

    void ReplicationManager::advanceClock() {
        if (receivedHistory.empty()) return;
        const double target =
            static_cast<double>(newestReceivedTick)
            - static_cast<double>(SnapshotSendInterval) * InterpolationDelayIntervals;
        if (!clockStarted) {
            playbackTick = target;
            clockStarted = true;
            return;
        }
        // Slewed, never snapped: jumping the clock shows up as a visible time warp.
        playbackTick += 1.0 + std::clamp((target - playbackTick) * 0.05, -0.25, 0.25);
    }

    const WorldSnapshot* ReplicationManager::findSent(const Uint64 tick) const {
        for (const WorldSnapshot& snapshot : sentHistory) {
            if (snapshot.tick == tick) return &snapshot;
        }
        return nullptr;
    }

    const WorldSnapshot* ReplicationManager::findReceived(const Uint64 tick) const {
        for (const WorldSnapshot& snapshot : receivedHistory) {
            if (snapshot.tick == tick) return &snapshot;
        }
        return nullptr;
    }

    void ReplicationManager::drawDebugUI() {
        if (!isBound() || !ImGui::CollapsingHeader("Replication")) return;

        if (peer->isHosting()) {
            ImGui::Text("Replicated: %d entities, %d snapshots buffered",
                        lastSnapshotEntities, static_cast<int>(sentHistory.size()));
            for (const auto& [connection, client] : clients) {
                if (!ImGui::TreeNode(reinterpret_cast<void*>(static_cast<uintptr_t>(connection)),
                                     "conn %u  %d B  %d changed%s", connection, client.lastSentBytes,
                                     client.lastChangedEntities, client.lastSendWasFull ? "  FULL" : "")) {
                    continue;
                }
                ImGui::Text("Acked tick: %llu", static_cast<unsigned long long>(client.ackedTick));
                if (client.awaitingBaseline()) {
                    ImGui::Text("Awaiting baseline %llu",
                                static_cast<unsigned long long>(client.pendingBaselineTick));
                }
                // More than one means fragments, and an unreliable snapshot is lost if any is.
                ImGui::Text("Fragments: ~%d",
                            (client.lastSentBytes + SoftPacketBytes - 1) / SoftPacketBytes);
                ImGui::Text("Full snapshots sent: %d", client.fullSnapshotsSent);
                ImGui::TreePop();
            }
        } else {
            ImGui::Text("Newest tick: %llu", static_cast<unsigned long long>(newestReceivedTick));
            ImGui::Text("Playback tick: %.1f", playbackTick);
            ImGui::Text("Drift: %.1f ticks", static_cast<double>(newestReceivedTick) - playbackTick);
            ImGui::Text("Buffered: %d snapshots", static_cast<int>(receivedHistory.size()));
            ImGui::Text("Dropped (no baseline): %d", droppedForMissingBaseline);
        }
    }
} // ytail::net
