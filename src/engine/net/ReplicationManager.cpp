//
// Created by Peter Gilbert on 8/8/26.
//

#include "ReplicationManager.h"

#include <algorithm>
#include <cstring>
#include <iterator>

#include <SDL3/SDL.h>

#include "imgui.h"

#include "NetPeer.h"
#include "engine/Engine.h"
#include "engine/World.h"
#include "engine/components/NetworkComponent.h"
#include "engine/components/RigidbodyComponent.h"
#include "engine/components/TransformComponent.h"
#include "engine/render/DebugDraw.h"

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
        stream.serializeVarUInt(entity.hostEntityId);
        stream.serializeVarUInt(entity.ownerPeerId);
        stream.serializePose(entity.pose);
    }

    const EntitySnapshot* WorldSnapshot::find(const uint32_t netId) const {
        const auto it = std::lower_bound(entities.begin(), entities.end(), netId,
            [](const EntitySnapshot& entity, const uint32_t id) { return entity.netId < id; });
        if (it == entities.end() || it->netId != netId) return nullptr;
        return &*it;
    }

    void ReplicationManager::sampleBandwidth() {
        const Uint64 now = SDL_GetTicks();
        for (BandwidthMeter* meter : { &sentBytes, &receivedBytes }) {
            if (meter->windowStartMs == 0) {
                meter->windowStartMs = now;
                continue;
            }
            const Uint64 elapsed = now - meter->windowStartMs;
            if (elapsed < 500) continue;
            meter->bytesPerSecond = static_cast<float>(meter->bytesThisWindow) * 1000.0f
                                  / static_cast<float>(elapsed);
            meter->bytesThisWindow = 0;
            meter->windowStartMs = now;
        }
    }

    void ReplicationManager::capture(const Uint64 tick) {
        if (!isBound() || !peer->isActive()) return;
        sampleBandwidth();
        if (peer->isHosting()) {
            captureHost(tick);
        } else if (newestReceivedTick != 0 && tick % ClientStateSendInterval == 0) {
            sendClientState();
        } else {
            return;
        }
        peer->flush();
    }

    void ReplicationManager::captureHost(const Uint64 tick) {
        // History is kept at send granularity, not per tick: a client's acked baseline has to
        // survive long enough for a slow reliable transfer to land and be acknowledged.
        // Tick 0 is skipped so 0 stays unambiguous as the "nothing acked yet" sentinel.
        if (tick == 0 || tick % SnapshotSendInterval != 0) return;

        World& world = engine->getWorld();

        WorldSnapshot snapshot;
        snapshot.tick = tick;
        world.each<NetworkComponent, TransformComponent>(
            [&](const EntityId id, NetworkComponent& network, const TransformComponent& transform) {
                if (network.netId == 0) network.netId = nextNetId++;
                entityByNetId[network.netId] = id;
                EntitySnapshot entity;
                entity.netId = network.netId;
                entity.hostEntityId = id;
                entity.ownerPeerId = network.ownerPeerId;
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
            const auto client = clients.find(connection);
            if (client == clients.end()) continue;
            sendSnapshotTo(connection, client->second, latest);
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
            sentBytes.add(size);
            client.pendingBaselineTick = snapshot.tick;
            ++client.fullSnapshotsSent;
        } else {
            peer->sendUnreliable(connection, sendWords.data(), static_cast<uint32_t>(size));
            sentBytes.add(size);
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
                || previous->ownerPeerId != entity.ownerPeerId) {
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
        // Bounded before the arithmetic below: (size + 3) wraps on a huge size and under allocates.
        constexpr uint32_t MaxMessageBytes = 1024 * 1024;
        if (!isBound() || size < 1 || size > MaxMessageBytes) return;
        const auto* bytes = static_cast<const uint8_t*>(data);
        receivedBytes.add(static_cast<int>(size));

        // BitReader loads the trailing partial word in full, so the payload is copied into a word
        // sized buffer rather than read in place off Steam's arbitrary length allocation.
        // Only the padding in the final word needs clearing; the memcpy covers the rest, and the
        // buffer keeps its capacity between messages.
        const uint32_t wordCount = (size + 3) / 4;
        if (receiveWords.size() < wordCount) receiveWords.resize(wordCount);
        receiveWords[wordCount - 1] = 0;
        std::memcpy(receiveWords.data(), bytes, size);

        switch (static_cast<NetMessageType>(bytes[0])) {
            case NetMessageType::Snapshot: 
                onSnapshotMessage(static_cast<int>(size)); 
                break;
            case NetMessageType::ClientState: 
                onClientStateMessage(connection, static_cast<int>(size)); 
                break;
            case NetMessageType::Welcome:
                onWelcomeMessage(static_cast<int>(size));
                break;
            default: break;
        }
    }

    void ReplicationManager::onSnapshotMessage(const int size) {
        if (peer->isHosting()) return;

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

        const auto byNetId = [](const EntitySnapshot& left, const EntitySnapshot& right) {
            return left.netId < right.netId;
        };

        // Read first and merge once. Inserting each entity into the sorted vector as it arrives is
        // O(n) per entity, which a packet full of entities turns into O(n^2).
        std::vector<EntitySnapshot> changed;
        const uint32_t changedCount = reader.readVarUInt();
        for (uint32_t i = 0; i < changedCount; ++i) {
            EntitySnapshot entity;
            serializeEntity(reader, entity);
            if (reader.hasOverflowed()) return;
            changed.push_back(entity);
        }

        reader.serializeCheck(CheckRemoved);
        std::vector<uint32_t> removed;
        const uint32_t removedCount = reader.readVarUInt();
        for (uint32_t i = 0; i < removedCount; ++i) {
            uint32_t netId = 0;
            reader.serializeVarUInt(netId);
            if (reader.hasOverflowed()) return;
            removed.push_back(netId);
        }

        if (reader.hasOverflowed()) return;

        // stable_sort so a duplicated netId resolves to the last one sent.
        std::stable_sort(changed.begin(), changed.end(), byNetId);
        std::sort(removed.begin(), removed.end());

        std::vector<EntitySnapshot> merged;
        merged.reserve(snapshot.entities.size() + changed.size());
        auto existing = snapshot.entities.begin();
        for (auto update = changed.begin(); update != changed.end(); ++update) {
            if (std::next(update) != changed.end() && std::next(update)->netId == update->netId) continue;
            while (existing != snapshot.entities.end() && existing->netId < update->netId) {
                merged.push_back(*existing++);
            }
            if (existing != snapshot.entities.end() && existing->netId == update->netId) ++existing;
            merged.push_back(*update);
        }
        merged.insert(merged.end(), existing, snapshot.entities.end());

        std::erase_if(merged, [&removed](const EntitySnapshot& entity) {
            return std::binary_search(removed.begin(), removed.end(), entity.netId);
        });
        snapshot.entities = std::move(merged);

        lastReceivedChanged = static_cast<int>(changed.size());
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
            ClientSyncState state;
            state.peerId = nextPeerId++;
            clients[connection] = state;
            sendWelcome(connection, state.peerId);
        } else {
            resetReceivedState();
        }
    }

    void ReplicationManager::sendWelcome(const uint32_t connection, const uint32_t peerId) {
        sendWords.resize(SendBufferWords);
        BitWriter writer(sendWords.data(), SendBufferWords);
        writer.writeBits(static_cast<uint32_t>(NetMessageType::Welcome), 8);
        writer.writeVarUInt(peerId);
        if (writer.hasOverflowed()) return;
        writer.flush();
        peer->sendReliable(connection, sendWords.data(), static_cast<uint32_t>(writer.getBytesWritten()));
    }

    void ReplicationManager::onWelcomeMessage(const int size) {
        BitReader reader(receiveWords.data(), size);
        reader.readBits(8);
        const uint32_t peerId = reader.readVarUInt();
        if (reader.hasOverflowed() || peer->isHosting()) return;
        localPeerId = peerId;
        SDL_Log("Assigned peer id %u by the host.", peerId);
    }

    void ReplicationManager::onDisconnected(const uint32_t connection) {
        if (!isBound()) return;
        if (peer->isHosting()) {
            const auto entry = clients.find(connection);
            if (entry == clients.end()) return;

            // Anything this peer owned goes back to the host, or it would sit frozen waiting
            const uint32_t departed = entry->second.peerId;
            engine->getWorld().each<NetworkComponent>([&](const EntityId, NetworkComponent& network) {
                if (network.ownerPeerId != departed) return;
                network.ownerPeerId = 0;
                ownedPoseByNetId.erase(network.netId);
            });
            clients.erase(entry);
        } else {
            resetReceivedState();
        }
    }

    void ReplicationManager::setOwner(const EntityId id, const uint32_t peerId) {
        if (!isBound() || !peer->isHosting()) return;
        if (const auto network = engine->getWorld().get<NetworkComponent>(id)) {
            network->ownerPeerId = peerId;
            // Anything the previous owner reported is stale
            ownedPoseByNetId.erase(network->netId);
        }
    }

    void ReplicationManager::refreshAuthority() {
        const bool hosting = peer->isHosting();
        engine->getWorld().each<NetworkComponent>([&](const EntityId id, NetworkComponent& network) {
            NetAuthority authority;
            if (network.ownerPeerId == 0) {
                authority = hosting ? NetAuthority::Host : NetAuthority::Remote;
            } else {
                authority = network.ownerPeerId == localPeerId ? NetAuthority::Owner : NetAuthority::Remote;
            }
            // Handing an entity over takes a round trip. The host keeps simulating it until the new
            // owner reports a pose, so it is never kinematic on both machines with nobody driving it.
            if (hosting && authority == NetAuthority::Remote && !ownedPoseByNetId.contains(network.netId)) {
                authority = NetAuthority::Host;
            }
            network.authority = authority;

            // Exactly one peer simulates a body; everyone else follows it kinematically.
            if (RigidbodyComponent* rigidbody = engine->getWorld().get<RigidbodyComponent>(id)) {
                rigidbody->setNetworkDriven(authority == NetAuthority::Remote);
            }
        });
    }

    void ReplicationManager::resetReceivedState() {
        // Hand the bodies back to the local solver, or they stay kinematic with nothing driving
        // them. Every networked entity, not just the mapped ones: a connection that drops before
        // the first snapshot lands leaves entityByNetId empty with bodies already driven.
        if (engine != nullptr) {
            World& world = engine->getWorld();
            world.each<NetworkComponent>([&](const EntityId id, NetworkComponent& network) {
                network.authority = NetAuthority::Host;
                if (RigidbodyComponent* rigidbody = world.get<RigidbodyComponent>(id)) {
                    rigidbody->setNetworkDriven(false);
                }
            });
        }
        receivedHistory.clear();
        entityByNetId.clear();
        newestReceivedTick = 0;
        playbackTick = 0.0;
        clockStarted = false;
        localPeerId = 0;
    }

    void ReplicationManager::onClientStateMessage(const uint32_t connection, const int size) {
        BitReader reader(receiveWords.data(), size);
        reader.readBits(8);
        const Uint64 ackedTick = reader.readVarUInt();
        if (reader.hasOverflowed() || !peer->isHosting()) return;

        // An ack for a tick we never sent would pin this client on the full snapshot path forever.
        const Uint64 newestSent = sentHistory.empty() ? 0 : sentHistory.back().tick;
        if (ackedTick > newestSent) return;

        const auto entry = clients.find(connection);
        if (entry == clients.end()) return;
        ClientSyncState& client = entry->second;
        client.ackedTick = std::max(client.ackedTick, ackedTick);

        const uint32_t ownedCount = reader.readVarUInt();
        for (uint32_t i = 0; i < ownedCount; ++i) {
            uint32_t netId = 0;
            QuantizedPose pose;
            reader.serializeVarUInt(netId);
            reader.serializePose(pose);
            if (reader.hasOverflowed()) return;
            // Only accept a pose for something this peer actually owns. Peer ids start at 1, so
            // host-owned and unknown entities can never match a real client.
            if (ownerPeerIdFor(netId) != client.peerId) continue;

            const auto existing = ownedPoseByNetId.find(netId);
            if (existing == ownedPoseByNetId.end()) {
                ownedPoseByNetId[netId] = OwnedPose{ pose, pose, localTick };
            } else {
                existing->second.previous = existing->second.current;
                existing->second.current = pose;
                existing->second.arrivedTick = localTick;
            }
        }
    }

    uint32_t ReplicationManager::ownerPeerIdFor(const uint32_t netId) const {
        const auto it = entityByNetId.find(netId);
        if (it == entityByNetId.end()) return 0;
        const NetworkComponent* network = engine->getWorld().get<NetworkComponent>(it->second);
        return network != nullptr ? network->ownerPeerId : 0;
    }

    void ReplicationManager::sendClientState() {
        std::vector<EntitySnapshot> owned;
        engine->getWorld().each<NetworkComponent, TransformComponent>(
            [&](const EntityId, const NetworkComponent& network, const TransformComponent& transform) {
                if (network.authority != NetAuthority::Owner || network.netId == 0) return;
                EntitySnapshot entity;
                entity.netId = network.netId;
                entity.pose = quantizePose(transform.getPosition(), transform.getRotation());
                owned.push_back(entity);
            });

        sendWords.resize(SendBufferWords);
        BitWriter writer(sendWords.data(), SendBufferWords);
        writer.writeBits(static_cast<uint32_t>(NetMessageType::ClientState), 8);
        writer.writeVarUInt(static_cast<uint32_t>(newestReceivedTick));
        writer.writeVarUInt(static_cast<uint32_t>(owned.size()));
        for (EntitySnapshot& entity : owned) {
            writer.serializeVarUInt(entity.netId);
            writer.serializePose(entity.pose);
        }
        if (writer.hasOverflowed()) return;
        writer.flush();
        peer->broadcastUnreliable(sendWords.data(), static_cast<uint32_t>(writer.getBytesWritten()));
    }

    ReplicationManager::ReplicationManager(Engine *inEngine, NetPeer *inPeer) {
        engine = inEngine;
        peer = inPeer;
    }

    void ReplicationManager::applyReceived(const Uint64 tick) {
        if (!isBound() || !peer->isActive()) return;
        localTick = tick;

        if (peer->isHosting()) {
            refreshAuthority();
            applyOwnedPoses(tick, Engine::FIXED_DT);
            return;
        }
        if (newestReceivedTick == 0) return;
        refreshAuthority();
        advanceClock();
        applyInterpolated(Engine::FIXED_DT);
    }

    void ReplicationManager::applyOwnedPoses(const Uint64 tick, const float deltaTime) {
        World& world = engine->getWorld();
        world.each<NetworkComponent>([&](const EntityId id, const NetworkComponent& network) {
            if (network.authority != NetAuthority::Remote) return;
            const auto reported = ownedPoseByNetId.find(network.netId);
            if (reported == ownedPoseByNetId.end()) return;
            const OwnedPose& owned = reported->second;

            // Reports land every ClientStateSendInterval ticks. interpolate between them.
            const double elapsed = tick > owned.arrivedTick
                ? static_cast<double>(tick - owned.arrivedTick) : 0.0;
            const float blend = static_cast<float>(
                std::clamp(elapsed / static_cast<double>(ClientStateSendInterval), 0.0, 1.0));

            glm::vec3 fromPosition;
            glm::quat fromRotation;
            glm::vec3 position;
            glm::quat rotation;
            dequantizePose(owned.previous, fromPosition, fromRotation);
            dequantizePose(owned.current, position, rotation);
            position = glm::mix(fromPosition, position, blend);
            rotation = glm::slerp(fromRotation, rotation, blend);

            if (RigidbodyComponent* rigidbody = world.get<RigidbodyComponent>(id)) {
                rigidbody->driveTo(position, rotation, deltaTime);
            } else if (TransformComponent* transform = world.get<TransformComponent>(id)) {
                transform->setPosition(position);
                transform->setRotation(rotation);
            }
        });
    }

    EntityId ReplicationManager::resolveEntity(const uint32_t netId, const uint32_t hostEntityId) {
        const auto cached = entityByNetId.find(netId);
        if (cached != entityByNetId.end()) return cached->second;

        // Both peers load the same scene and scene loading preserves entity ids, so the host's id
        // names our copy too. A runtime spawned entity has no local copy and needs spawn data.
        World& world = engine->getWorld();
        if (world.getEntity(hostEntityId) == nullptr) return NULL_ENTITY;

        NetworkComponent* network = world.get<NetworkComponent>(hostEntityId);
        if (network == nullptr) return NULL_ENTITY;
        network->netId = netId;
        entityByNetId[netId] = hostEntityId;
        return hostEntityId;
    }

    void ReplicationManager::applyInterpolated(const float deltaTime) {
        if (receivedHistory.size() < 2) return;

        const WorldSnapshot* from = nullptr;
        const WorldSnapshot* to = nullptr;
        for (size_t i = 1; i < receivedHistory.size(); ++i) {
            if (static_cast<double>(receivedHistory[i].tick) >= playbackTick) {
                from = &receivedHistory[i - 1];
                to = &receivedHistory[i];
                break;
            }
        }
        // Playback has run past everything buffered; hold the last pose rather than extrapolate.
        if (from == nullptr) return;

        const double span = static_cast<double>(to->tick) - static_cast<double>(from->tick);
        const float blend = span > 0.0
            ? static_cast<float>(std::clamp((playbackTick - static_cast<double>(from->tick)) / span, 0.0, 1.0))
            : 1.0f;

        World& world = engine->getWorld();
        for (const EntitySnapshot& target : to->entities) {
            const EntityId id = resolveEntity(target.netId, target.hostEntityId);
            if (id == NULL_ENTITY) continue;

            NetworkComponent* network = world.get<NetworkComponent>(id);
            if (network == nullptr) continue;
            network->ownerPeerId = target.ownerPeerId;
            if (target.ownerPeerId == localPeerId && localPeerId != 0) continue;

            glm::vec3 position;
            glm::quat rotation;
            dequantizePose(target.pose, position, rotation);

            if (const EntitySnapshot* previous = from->find(target.netId)) {
                glm::vec3 fromPosition;
                glm::quat fromRotation;
                dequantizePose(previous->pose, fromPosition, fromRotation);
                position = glm::mix(fromPosition, position, blend);
                rotation = glm::slerp(fromRotation, rotation, blend);
            }

            if (RigidbodyComponent* rigidbody = world.get<RigidbodyComponent>(id)) {
                rigidbody->setNetworkDriven(true);
                rigidbody->driveTo(position, rotation, deltaTime);
            } else if (TransformComponent* transform = world.get<TransformComponent>(id)) {
                transform->setPosition(position);
                transform->setRotation(rotation);
            }
        }
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

        const auto drift = static_cast<float>(static_cast<double>(newestReceivedTick) - playbackTick);
        smoothedDrift = smoothedDrift * 0.95f + drift * 0.05f;
        driftHistory[driftHistoryIndex] = drift;
        driftHistoryIndex = (driftHistoryIndex + 1) % static_cast<int>(driftHistory.size());
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

    void ReplicationManager::drawSnapshotGhosts(DebugDraw& debug) const {
        if (!showGhosts || !isBound() || receivedHistory.empty()) return;

        const WorldSnapshot& newest = receivedHistory.back();
        const World& world = engine->getWorld();
        for (const EntitySnapshot& entity : newest.entities) {
            const auto mapped = entityByNetId.find(entity.netId);
            if (mapped == entityByNetId.end()) continue;
            const TransformComponent* transform = world.get<TransformComponent>(mapped->second);
            if (transform == nullptr) continue;

            glm::vec3 rawPosition;
            glm::quat rawRotation;
            dequantizePose(entity.pose, rawPosition, rawRotation);
            const glm::vec3 shown = glm::vec3(transform->worldMatrix()[3]);

            debug.wireSphere(rawPosition, 0.25f, glm::vec4(1.0f, 0.3f, 0.3f, 1.0f), 12);
            debug.line(rawPosition, shown, glm::vec4(1.0f, 1.0f, 0.3f, 1.0f));
        }
    }

    static void ownerLabel(const uint32_t peerId, char* buffer, const size_t size) {
        if (peerId == 0) {
            SDL_strlcpy(buffer, "host", size);
        } else {
            SDL_snprintf(buffer, size, "peer %u", peerId);
        }
    }

    void ReplicationManager::drawEntityTable() {
        if (!ImGui::TreeNode("Entities")) return;

        // Peers to offer in the owner picker, host first.
        const bool hosting = peer->isHosting();
        std::vector<uint32_t> peerIds;
        if (hosting) {
            peerIds.reserve(clients.size() + 1);
            peerIds.push_back(0);
            for (const auto& [connection, client] : clients) peerIds.push_back(client.peerId);
            std::sort(peerIds.begin(), peerIds.end());
        }
        // Applied after the pass: setOwner touches the component the iteration is walking.
        EntityId reassignEntity = NULL_ENTITY;
        uint32_t reassignPeerId = 0;

        if (ImGui::BeginTable("netentities", 4,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Net Id");
            ImGui::TableSetupColumn("Owner");
            ImGui::TableSetupColumn("Authority");
            ImGui::TableSetupColumn("Body");
            ImGui::TableHeadersRow();

            const char* authorityNames[] = { "Host", "Owner", "Remote" };
            World& world = engine->getWorld();
            world.each<NetworkComponent>([&](const EntityId id, const NetworkComponent& network) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%u", network.netId);
                ImGui::TableNextColumn();
                if (!hosting) {
                    if (network.ownerPeerId == 0) {
                        ImGui::TextUnformatted("host");
                    } else {
                        ImGui::Text("peer %u", network.ownerPeerId);
                    }
                } else {
                    char current[16];
                    ownerLabel(network.ownerPeerId, current, sizeof(current));
                    ImGui::PushID(static_cast<int>(id));
                    ImGui::SetNextItemWidth(-1.0f);
                    if (ImGui::BeginCombo("##owner", current)) {
                        for (const uint32_t peerId : peerIds) {
                            char item[16];
                            ownerLabel(peerId, item, sizeof(item));
                            if (ImGui::Selectable(item, network.ownerPeerId == peerId)) {
                                reassignEntity = id;
                                reassignPeerId = peerId;
                            }
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::PopID();
                }
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(authorityNames[static_cast<int>(network.authority)]);
                ImGui::TableNextColumn();
                const RigidbodyComponent* rigidbody = world.get<RigidbodyComponent>(id);
                if (rigidbody == nullptr) {
                    ImGui::TextUnformatted("-");
                } else {
                    ImGui::TextUnformatted(rigidbody->isNetworkDriven() ? "driven" : "simulated");
                }
            });
            ImGui::EndTable();
        }
        if (reassignEntity != NULL_ENTITY) setOwner(reassignEntity, reassignPeerId);
        ImGui::TreePop();
    }

    void ReplicationManager::drawDebugUI() {
        if (!isBound() || !ImGui::CollapsingHeader("Replication")) return;

        ImGui::Text("Bandwidth: %.1f KB/s out, %.1f KB/s in",
                    sentBytes.bytesPerSecond / 1024.0f, receivedBytes.bytesPerSecond / 1024.0f);
        ImGui::Checkbox("Show snapshot ghosts", &showGhosts);
        ImGui::Separator();

        if (peer->isHosting()) {
            ImGui::Text("Peer id: host");
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
            ImGui::Text("Peer id: %u", localPeerId);
            ImGui::Text("Newest tick: %llu", static_cast<unsigned long long>(newestReceivedTick));
            ImGui::Text("Playback tick: %.1f", playbackTick);
            // Sawtooths by one send interval, so this is smoothed. Climbing means the clock is losing.
            ImGui::Text("Drift: %.1f ticks (target %.1f)", smoothedDrift,
                        static_cast<double>(SnapshotSendInterval) * InterpolationDelayIntervals);
            ImGui::PlotLines("##drift", driftHistory.data(), static_cast<int>(driftHistory.size()),
                             driftHistoryIndex, nullptr, 0.0f,
                             static_cast<float>(SnapshotSendInterval) * InterpolationDelayIntervals * 2.0f,
                             ImVec2(0.0f, 40.0f));
            ImGui::Text("Buffered: %d snapshots", static_cast<int>(receivedHistory.size()));
            ImGui::Text("Mapped entities: %d", static_cast<int>(entityByNetId.size()));
            // Zero while nothing moves is delta compression working.
            ImGui::Text("Changed last snapshot: %d", lastReceivedChanged);
            ImGui::Text("Dropped (no baseline): %d", droppedForMissingBaseline);
        }

        drawEntityTable();
    }
} // ytail::net
