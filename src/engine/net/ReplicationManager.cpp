//
// Created by Peter Gilbert on 8/8/26.
//

#include "ReplicationManager.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

#include <SDL3/SDL.h>

#include "imgui.h"

#include "NetPeer.h"
#include "engine/Engine.h"
#include "engine/World.h"
#include "engine/components/NetworkComponent.h"
#include "engine/components/RigidbodyComponent.h"
#include "engine/components/TransformComponent.h"
#include "engine/managers/PhysicsManager.h"
#include "engine/render/DebugDraw.h"

// See docs/netcode.md for the model, the wire format, and how the delays interact.
namespace ytail::net {
    // Steam fragments unreliable messages, so this is not an MTU limit. It only has to be larger
    // than any snapshot we build.
    constexpr int SendBufferWords = 16 * 1024;
    constexpr int SoftPacketBytes = 1200;

    // Meant to hold together up to about 300ms rather than be tuned for it. Reasoning for each of
    // these is in docs/netcode.md, "Tuning defaults".
    Uint64 ReplicationManager::snapshotSendInterval = 6;
    Uint64 ReplicationManager::minJitterTicks = 2;
    Uint64 ReplicationManager::jitterMargin = 2;
    int ReplicationManager::jitterWindowPackets = 30;
    Uint64 ReplicationManager::inputDelayTicks = 4;
    bool ReplicationManager::adaptiveInputDelay = true;
    bool ReplicationManager::lateInputRollback = true;
    bool ReplicationManager::peerMesh = true;
    Uint64 ReplicationManager::maxInputDelayTicks = 8;
    float ReplicationManager::predictionTolerance = 0.05f;
    float ReplicationManager::predictionAngleTolerance = 0.14f;

    // One definition for both directions, so a field cannot be added to the write side and
    // forgotten on the read side.
    template<typename Stream>
    void serializeEntity(Stream& stream, EntitySnapshot& entity) {
        stream.serializeVarUInt(entity.netId);
        stream.serializeVarUInt(entity.hostEntityId);
        stream.serializeVarUInt(entity.ownerPeerId);
        serializeState(stream, entity.state);
    }

    int entityBits(const EntitySnapshot& entity) {
        return varUIntBits(entity.netId) + varUIntBits(entity.hostEntityId) + varUIntBits(entity.ownerPeerId)
             + stateBits(entity.state);
    }

    constexpr Uint64 WarnIntervalMs = 5000;

    bool ThrottledWarning::due() {
        const Uint64 now = SDL_GetTicks();
        if (lastLoggedMs != 0 && now - lastLoggedMs < WarnIntervalMs) {
            ++suppressed;
            return false;
        }
        lastLoggedMs = now;
        return true;
    }

    void ClientSyncState::grow(const size_t netIdCount) {
        if (priority.size() >= netIdCount) return;
        priority.resize(netIdCount, 0.0f);
        lastSentState.resize(netIdCount);
        lastSentOwner.resize(netIdCount, ReplicationManager::NoOwner);
        pendingSends.resize(netIdCount, ReplicationManager::RestRedundancy);
        lastSentTick.resize(netIdCount, 0);
    }

    void InputHistory::write(const Uint64 tick, const NetInputFrame& frame) {
        const size_t slot = tick % Length;
        // Only forwards: a redundant copy of a frame we already hold must not overwrite a newer one
        // that happens to share the slot.
        if (ticks[slot] > tick) return;

        // We already simulated this tick with something other than what actually happened, so the
        // press is not lost, only late.
        if (usedTicks[slot] == tick && !(used[slot] == frame) && ticks[slot] != tick) {
            mispredictedTick = mispredictedTick == 0 ? tick : std::min(mispredictedTick, tick);
        }
        ticks[slot] = tick;
        frames[slot] = frame;
        newestTick = std::max(newestTick, tick);
    }

    NetInputFrame InputHistory::at(const Uint64 tick) const {
        const size_t slot = tick % Length;
        return ticks[slot] == tick ? frames[slot] : NetInputFrame{};
    }

    // Records what it hands back, so a frame arriving later for this tick can be recognised as
    // contradicting a guess rather than simply filed away.
    // See docs/netcode.md, "Late input", for why a gap carries forward instead of going neutral.
    NetInputFrame InputHistory::read(const Uint64 tick) {
        const size_t slot = tick % Length;
        usedTicks[slot] = tick;
        if (ticks[slot] == tick) {
            used[slot] = frames[slot];
            return frames[slot];
        }

        // Nothing for this tick, so hold the newest one we do have. Normal for another player, so
        // only giving up entirely counts as starvation.
        if (newestTick == 0 || tick < newestTick
            || tick - newestTick > ReplicationManager::MaxInputStaleTicks) {
            ++starved;
            used[slot] = NetInputFrame{};
            return {};
        }
        const size_t newestSlot = newestTick % Length;
        if (ticks[newestSlot] != newestTick) return {};

        // Safe only because every button is held rather than pulsed. An edge-triggered bit would be
        // lost here every time a peer's input ran late.
        ++repeated;
        used[slot] = frames[newestSlot];
        return frames[newestSlot];
    }

    // Shortest angle between two orientations. The absolute value folds q and -q together, which
    // name the same rotation.
    static float angleBetween(const glm::quat& from, const glm::quat& to) {
        return 2.0f * std::acos(std::clamp(std::abs(glm::dot(from, to)), 0.0f, 1.0f));
    }

    void PredictedPoses::write(const Uint64 tick, const glm::vec3& position, const glm::quat& rotation) {
        const size_t slot = tick % Length;
        ticks[slot] = tick;
        positions[slot] = position;
        rotations[slot] = rotation;
    }

    bool PredictedPoses::poseAt(const Uint64 tick, glm::vec3& outPosition, glm::quat& outRotation) const {
        const size_t slot = tick % Length;
        if (ticks[slot] != tick) return false;
        outPosition = positions[slot];
        outRotation = rotations[slot];
        return true;
    }

    bool PredictedPoses::matches(const Uint64 tick, const glm::vec3& position, const glm::quat& rotation) const {
        glm::vec3 predictedPosition;
        glm::quat predictedRotation;
        if (!poseAt(tick, predictedPosition, predictedRotation)) return false;
        return glm::distance(predictedPosition, position) <= ReplicationManager::predictionTolerance
            && angleBetween(predictedRotation, rotation) <= ReplicationManager::predictionAngleTolerance;
    }

    // Up at once, down slowly. A packet later than the hold pops whatever it moves, while giving
    // slack back breaks nothing.
    void JitterEstimator::observe(const Uint64 transit) {
        worstTransit = std::max(worstTransit, transit);
        const Uint64 needed = std::max(transit + ReplicationManager::jitterMargin,
                                       ReplicationManager::minJitterTicks);
        windowMax = std::max(windowMax, needed);
        target = std::max(target, needed);

        if (++windowPackets < ReplicationManager::jitterWindowPackets) return;
        // One tick per window, and never below what this window actually needed.
        if (target > windowMax) --target;
        windowMax = 0;
        windowPackets = 0;
        target = std::min(target, ReplicationManager::MaxJitterTicks);
    }

    void ReplicationManager::openTrace(const std::string& path) {
        traceFile.open(path, std::ios::trunc);
        if (!traceFile.is_open()) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Could not open net trace %s", path.c_str());
            return;
        }
        traceFile << "tick,peer,netId,owner,x,y,z,qx,qy,qz,qw\n";
        SDL_Log("Writing net trace to %s", path.c_str());
    }

    // This peer's own view, not the host's. Joining two peers' traces on (tick, netId) gives the
    // prediction error directly.
    void ReplicationManager::writeTrace(const Uint64 tick) {
        if (!traceFile.is_open()) return;
        engine->getWorld().each<NetworkComponent, TransformComponent>(
            [&](const EntityId, const NetworkComponent& network, const TransformComponent& transform) {
                if (network.netId == 0) return;
                const glm::vec3 position = transform.getPosition();
                const glm::quat rotation = transform.getRotation();
                traceFile << tick << ',' << localPeerId << ',' << network.netId << ','
                          << network.ownerPeerId << ','
                          << position.x << ',' << position.y << ',' << position.z << ','
                          << rotation.x << ',' << rotation.y << ',' << rotation.z << ',' << rotation.w
                          << '\n';
            });
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

    // Filed against a tick in the future, so it has that many ticks of head start to reach everyone
    // before they reach the tick it belongs to. See docs/netcode.md, "The three delays".
    void ReplicationManager::setLocalInput(const Uint64 tick, const NetInputFrame& frame) {
        if (localPeerId == NoOwner) return;
        InputHistory& history = inputByPeer[localPeerId];
        const Uint64 target = tick + inputDelayTicks;

        // Raising the delay steps over a tick, which every peer would otherwise read as a moment of
        // no input at all.
        Uint64 first = target;
        if (history.newestTick > 0 && history.newestTick < target
            && target - history.newestTick <= InputHistory::Length) {
            first = history.newestTick + 1;
        }
        for (Uint64 fill = first; fill <= target; ++fill) history.write(fill, frame);
    }

    // The delay has to cover the trip to whoever simulates this peer's body, and the longest of
    // those is the round trip to the host. Fitted to measured crossovers of six ticks at 50ms and
    // twelve at 150ms.
    void ReplicationManager::steerInputDelay() {
        if (!adaptiveInputDelay || peer->getConnections().empty()) return;
        if (inputDelayCooldown > 0) {
            --inputDelayCooldown;
            return;
        }

        Uint64 worstOneWay = 0;
        for (const uint32_t connection : peer->getConnections()) {
            const int pingMs = peer->getPingMs(connection);
            if (pingMs < 0) continue;
            worstOneWay = std::max(worstOneWay, static_cast<Uint64>(
                std::ceil((static_cast<float>(pingMs) * 0.5f) / (Engine::FIXED_DT * 1000.0f))));
        }
        if (worstOneWay == 0) return;

        const Uint64 wanted = std::clamp<Uint64>(worstOneWay * 2 + InputLeadTarget,
                                                 MinInputDelayTicks, maxInputDelayTicks);
        if (wanted == inputDelayTicks) return;
        // A tick at a time, since the clock steers against this and a jump would drag the clock
        // after it, leaving a seam in the history everyone predicting us reads from.
        inputDelayTicks += wanted > inputDelayTicks ? 1 : -1;
        inputDelayCooldown = InputDelayAdjustCooldown;
    }

    bool ReplicationManager::hasLocalInputFor(const Uint64 tick) const {
        if (localPeerId == NoOwner) return false;
        const auto history = inputByPeer.find(localPeerId);
        return history != inputByPeer.end() && history->second.newestTick >= tick + inputDelayTicks;
    }

    NetInputFrame ReplicationManager::getInput(const uint32_t peerId, const Uint64 tick) {
        if (peerId == NoOwner) return {};
        const auto history = inputByPeer.find(peerId);
        return history == inputByPeer.end() ? NetInputFrame{} : history->second.read(tick);
    }

    // The newest frames, oldest first, each either a repeat of the one before it or eight fresh
    // bits. The shared tick stamp is what lets a redundant copy repair the exact hole it was sent
    // to repair.
    template<typename Stream>
    void ReplicationManager::serializeInputWindow(Stream& stream, const uint32_t peerId, const Uint64 newestTick) {
        InputHistory& history = inputByPeer[peerId];
        NetInputFrame previous;
        for (int i = NetInputRedundancy - 1; i >= 0; --i) {
            const auto age = static_cast<Uint64>(i);
            const Uint64 tick = newestTick >= age ? newestTick - age : 0;
            NetInputFrame frame;
            // at, not read: a hole in what we are sending is not input starvation in the simulation.
            if constexpr (Stream::IsWriting) frame = history.at(tick);

            bool sameAsPrevious = frame == previous;
            stream.serializeBool(sameAsPrevious);
            if (sameAsPrevious) frame = previous;
            else stream.serializeBits(frame.buttons, NetInputButtonBits);

            if constexpr (!Stream::IsWriting) history.write(tick, frame);
            previous = frame;
        }
    }

    // One way trip to the host, in ticks, or 0 when the backend has no estimate yet. The host's
    // connection specifically, since the ping to a peer link says nothing about the clock.
    Uint64 ReplicationManager::oneWayTicks() const {
        const uint32_t connection = peer->getHostConnection();
        if (connection == 0) return 0;
        const int pingMs = peer->getPingMs(connection);
        if (pingMs < 0) return 0;
        return static_cast<Uint64>(
            std::ceil((static_cast<float>(pingMs) * 0.5f) / (Engine::FIXED_DT * 1000.0f)));
    }

    // How far ahead of the host the clock should start. Falls back to a fixed guess for the first
    // moments of a connection, while the backend has no estimate.
    Uint64 ReplicationManager::leadFromPing() {
        if (peer->getConnections().empty()) return InitialInputLead;
        const Uint64 oneWay = oneWayTicks();
        if (oneWay == 0) return InitialInputLead;

        // The delay already gives our input this much of a head start, so the clock only has to make
        // up whatever is left of the trip.
        const Uint64 fromClock = oneWay > inputDelayTicks ? oneWay - inputDelayTicks : 0;
        // Past this the clock cannot get far enough ahead for our input to reach the host in time,
        // and nothing else in the system can compensate.
        if (fromClock > MaxInputLeadTicks && connectionCapWarning.due()) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "This connection needs a %llu tick clock lead, past the %llu cap "
                        "(%d suppressed). Round trip is beyond what this replication is built for; "
                        "expect the host's bodies to be corrected constantly.",
                        static_cast<unsigned long long>(fromClock),
                        static_cast<unsigned long long>(MaxInputLeadTicks),
                        std::exchange(connectionCapWarning.suppressed, 0));
        }
        return std::clamp<Uint64>(fromClock, 0, MaxInputLeadTicks);
    }

    // Ticks of our input the host should be holding beyond the tick it is simulating. Asking only
    // for what the delay does not already cover lets the clock settle at zero once the delay covers
    // the trip. See docs/netcode.md, "The three delays", for why the margin belongs here.
    int32_t ReplicationManager::inputLeadTarget() const {
        const auto uncovered = static_cast<int32_t>(inputDelayTicks) - static_cast<int32_t>(oneWayTicks());
        return std::max(InputLeadTarget, uncovered);
    }

    // One tick at a time, with a dead band and a cooldown. The reported lead is a round trip old, so
    // reacting to every packet would keep correcting for a change already made and oscillate.
    void ReplicationManager::steerClock(const int32_t reportedLead) {
        measuredInputLead = reportedLead;
        if (engine == nullptr) return;
        if (clockCooldown > 0) {
            --clockCooldown;
            return;
        }

        const int32_t target = inputLeadTarget();
        const int32_t error = reportedLead < target ? target - reportedLead
            : (reportedLead > target + InputLeadSlack
               ? target + InputLeadSlack - reportedLead : 0);
        if (error == 0) return;

        // A large error is a step change rather than drift (a hitch, a route change, a bad initial
        // estimate), and crawling out of one would take seconds.
        const int adjustment = std::abs(error) > InputLeadJumpThreshold ? error : (error > 0 ? 1 : -1);
        engine->adjustClock(adjustment);
        ++clockAdjustments;
        clockCooldown = ClockAdjustCooldownPackets;
    }

    static void tickSlider(const char* label, Uint64& value, const int max, const char* tooltip) {
        int ticks = static_cast<int>(value);
        if (ImGui::SliderInt(label, &ticks, 0, max, "%d ticks")) value = static_cast<Uint64>(std::max(0, ticks));
        ImGui::SetItemTooltip("%s", tooltip);
    }

    void ReplicationManager::drawTuning() {
        ImGui::SeparatorText("Tuning");
        tickSlider("State interval", snapshotSendInterval, 12,
                   "Ticks between snapshots. Lower corrects sooner, for more bandwidth.");
        tickSlider("Jitter floor", minJitterTicks, 12,
                   "Only a floor. The hold in use is measured, not set here.");
        tickSlider("Jitter margin", jitterMargin, 8,
                   "Slack above the worst transit seen. Raise it if stacks still pop.");
        ImGui::SliderInt("Jitter window", &jitterWindowPackets, 4, 120, "%d packets");
        ImGui::SetItemTooltip("Packets between shrink steps. Longer holds onto slack after a spike.");
        ImGui::SliderFloat("Prediction tolerance", &predictionTolerance, 0.001f, 0.5f, "%.3f m");
        ImGui::SetItemTooltip("How far a body may drift before the world is rewound and replayed.");
        ImGui::SliderAngle("Prediction angle", &predictionAngleTolerance, 0.0f, 45.0f);
        ImGui::SetItemTooltip("The same, for spin. Checked separately: a ball can hold its position "
                              "while its rotation drifts.");
        ImGui::Checkbox("Late input rollback", &lateInputRollback);
        ImGui::SetItemTooltip("Replay a tick when a peer's input for it lands after we guessed. Off "
                              "to see what it is costing; a press that arrives late is then lost.");
        ImGui::Checkbox("Adaptive input delay", &adaptiveInputDelay);
        ImGui::SetItemTooltip("Follows the round trip. Off to pin it with the slider below.");
        tickSlider("Max input delay", maxInputDelayTicks, 20,
                   "Ceiling on the adaptive value. Past it the host's ball degrades first: that one "
                   "costs a round trip, while a body predicted over a direct peer link costs one hop.");
        tickSlider("Input delay", inputDelayTicks, 20,
                   "Ticks before your own ball acts, in exchange for everyone else simulating it "
                   "exactly instead of guessing. Two covers a local connection.");

        VisualSmoothing& smoothing = RigidbodyComponent::smoothing;
        ImGui::SliderFloat("Small decay", &smoothing.smallDecay, 0.5f, 0.999f, "%.3f /frame");
        ImGui::SliderFloat("Large decay", &smoothing.largeDecay, 0.5f, 0.999f, "%.3f /frame");
        ImGui::SetItemTooltip("Lower is faster. Raise it if remote players slide rather than jitter.");
        ImGui::SliderFloat("Small error", &smoothing.smallError, 0.01f, 2.0f, "%.2f m");
        ImGui::SliderFloat("Large error", &smoothing.largeError, 0.01f, 5.0f, "%.2f m");
        ImGui::SetItemTooltip("The range the fade rate ramps across, from gentle to tight.");
        ImGui::SliderAngle("Small rotation", &smoothing.smallRotation, 0.0f, 90.0f);
        ImGui::SliderAngle("Large rotation", &smoothing.largeRotation, 0.0f, 180.0f);
        ImGui::SetItemTooltip("The same range for spin, judged on its own angle.");
    }

    void ReplicationManager::sampleCorrection() {
        const Uint64 now = SDL_GetTicks();
        if (correctionWindowStartMs == 0) {
            correctionWindowStartMs = now;
            return;
        }
        if (now - correctionWindowStartMs < 250) return;

        const float samples = static_cast<float>(std::max(correction.count, 1));
        correction.mean = correction.count > 0 ? correction.total / samples : 0.0f;
        correction.meanAngle = correction.count > 0 ? correction.totalAngle / samples : 0.0f;
        correction.peak = std::max(correction.peak * 0.9f, correction.mean);
        correctionHistory[correctionHistoryIndex] = correction.mean;
        correctionHistoryIndex = (correctionHistoryIndex + 1) % static_cast<int>(correctionHistory.size());
        correction.total = 0.0f;
        correction.totalAngle = 0.0f;
        correction.count = 0;

        resimStepsPerSecond = static_cast<float>(resimStepsThisWindow)
                            * 1000.0f / static_cast<float>(now - correctionWindowStartMs);
        resimStepsThisWindow = 0;
        deepestResim = static_cast<int>(static_cast<float>(deepestResim) * 0.75f);
        correctionWindowStartMs = now;
    }

    void ReplicationManager::capture(const Uint64 tick) {
        if (!isBound() || !peer->isActive()) return;
        sampleBandwidth();
        steerInputDelay();
        writeTrace(tick);
        if (peer->isHosting()) {
            // Every tick, separately from state. Forwarding at the state rate left clients acting
            // on input a whole send interval stale, which showed up as the other players
            // constantly being corrected.
            sendPeerInput();
            captureHost(tick);
        } else {
            recordPredictions(tick);
            saveContacts(tick);
            if (localPeerId != NoOwner && tick % InputSendInterval == 0) sendClientInput();
        }
        peer->flush();
        // Last, so the column carries this tick's sends as well as what arrived during it.
        sampleNetGraph();
    }

    void ReplicationManager::sampleNetGraph() {
        NetGraphSample sample;
        sample.correction = tickCorrection;
        sample.resimDepth = static_cast<uint8_t>(std::min(tickResimDepth, 255));
        sample.bytesSent = static_cast<uint16_t>(std::min(sentBytes.takeTickBytes(), 65535));
        sample.bytesReceived = static_cast<uint16_t>(std::min(receivedBytes.takeTickBytes(), 65535));

        int starved = 0;
        for (const auto& [peerId, history] : inputByPeer) starved += history.starved;
        if (starved > starvedTotal) tickFlags |= NetGraphStarved;
        starvedTotal = starved;

        sample.flags = tickFlags;
        netGraph.push(sample);
        tickCorrection = 0.0f;
        tickResimDepth = 0;
        tickFlags = 0;
    }

    // A networked entity without a rigidbody has no velocity to report and is always at rest.
    QuantizedState ReplicationManager::captureState(const EntityId id, const TransformComponent& transform) {
        const RigidbodyComponent* rigidbody = engine->getWorld().get<RigidbodyComponent>(id);
        const glm::vec3 position = transform.getPosition();

        // Everything past the bound quantizes to the bound, so a body that leaves the world freezes
        // on every other machine while carrying on here.
        const float furthest = std::max({ std::abs(position.x), std::abs(position.y), std::abs(position.z) });
        if (furthest > WorldExtentMeters && worldBoundWarning.due()) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Entity %u is %.0fm from the origin, past the %.0fm the wire can express "
                        "(%d warnings suppressed). It will freeze at the boundary on every other "
                        "peer. Add a kill volume, or raise WorldExtentMeters.",
                        id, furthest, WorldExtentMeters,
                        std::exchange(worldBoundWarning.suppressed, 0));
        }

        if (rigidbody == nullptr) {
            return quantizeState(position, transform.getRotation(),
                                 glm::vec3(0.0f), glm::vec3(0.0f), true);
        }

        const glm::vec3 linear = rigidbody->getLinearVelocity();
        const glm::vec3 angular = rigidbody->getAngularVelocity();
        // The same failure one derivative up. A clamped velocity means every remote peer simulates
        // this body slower than it is really going.
        const float fastest = std::max({ std::abs(linear.x), std::abs(linear.y), std::abs(linear.z) });
        const float spin = std::max({ std::abs(angular.x), std::abs(angular.y), std::abs(angular.z) });
        if ((fastest > MaxLinearSpeed || spin > MaxAngularSpeed) && velocityRangeWarning.due()) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Entity %u exceeds the replicated velocity range: %.0f m/s (max %.0f), "
                        "%.0f rad/s (max %.0f) (%d suppressed). It will be clamped on the wire and "
                        "corrected constantly. Lower the body's maxLinearVelocity or raise "
                        "MaxLinearSpeed / MaxAngularSpeed.",
                        id, fastest, MaxLinearSpeed, spin, MaxAngularSpeed,
                        std::exchange(velocityRangeWarning.suppressed, 0));
        }
        return quantizeState(position, transform.getRotation(), linear, angular, rigidbody->isAtRest());
    }

    void ReplicationManager::captureHost(const Uint64 tick) {
        if (tick == 0 || tick % snapshotSendInterval != 0) return;

        World& world = engine->getWorld();

        // One quantize pass shared by every connection, indexed by netId so selection needs no
        // lookups. Only the choice of what to send is per client.
        latestStates.clear();
        world.each<NetworkComponent, TransformComponent>(
            [&](const EntityId id, NetworkComponent& network, const TransformComponent& transform) {
                if (network.netId == 0) network.netId = nextNetId++;
                entityByNetId[network.netId] = id;
                EntitySnapshot entity;
                entity.netId = network.netId;
                entity.hostEntityId = id;
                entity.ownerPeerId = network.ownerPeerId;
                entity.state = captureState(id, transform);
                latestStates.push_back(entity);
            });
        lastSnapshotEntities = static_cast<int>(latestStates.size());

        const std::vector<uint32_t>& connections = peer->getConnections();
        std::erase_if(clients, [&connections](const auto& entry) {
            return std::find(connections.begin(), connections.end(), entry.first) == connections.end();
        });

        for (const uint32_t connection : connections) {
            const auto client = clients.find(connection);
            if (client == clients.end()) continue;
            sendStateTo(connection, client->second, tick);
        }
    }

    // Deliberately small. All it decides is what a starved connection gives up first.
    float ReplicationManager::basePriority(const EntitySnapshot& entity, const ClientSyncState& client,
                                           const glm::vec3& clientFocus) const {
        float priority = 1.0f;
        if (entity.ownerPeerId != NoOwner) priority *= 4.0f;
        if (entity.ownerPeerId == client.peerId) priority *= 2.0f;

        constexpr float FarDistance = 40.0f;
        glm::vec3 position;
        glm::quat rotation;
        glm::vec3 linear;
        glm::vec3 angular;
        dequantizeState(entity.state, position, rotation, linear, angular);
        const glm::vec3 offset = position - clientFocus;
        if (glm::dot(offset, offset) > FarDistance * FarDistance) priority *= 0.25f;
        return priority;
    }

    void ReplicationManager::sendStateTo(const uint32_t connection, ClientSyncState& client, const Uint64 tick) {
        client.grow(nextNetId);

        // Distance is measured from whatever this client owns, since that is where it is looking.
        glm::vec3 clientFocus(0.0f);
        for (const EntitySnapshot& entity : latestStates) {
            if (entity.ownerPeerId != client.peerId) continue;
            glm::quat rotation;
            glm::vec3 linear;
            glm::vec3 angular;
            dequantizeState(entity.state, clientFocus, rotation, linear, angular);
            break;
        }

        // Anything whose state changed owes a fresh round of sends. Anything settled counts down and
        // goes silent, which is what makes a world full of resting objects free.
        std::vector<const EntitySnapshot*> candidates;
        for (const EntitySnapshot& entity : latestStates) {
            const size_t index = entity.netId;
            if (entity.state != client.lastSentState[index] || entity.ownerPeerId != client.lastSentOwner[index])
                client.pendingSends[index] = RestRedundancy;
            if (client.pendingSends[index] == 0) continue;
            client.priority[index] += basePriority(entity, client, clientFocus);
            candidates.push_back(&entity);
        }
        std::sort(candidates.begin(), candidates.end(),
            [&client](const EntitySnapshot* left, const EntitySnapshot* right) {
                return client.priority[left->netId] > client.priority[right->netId];
            });

        sendWords.resize(SendBufferWords);
        BitWriter writer(sendWords.data(), SendBufferWords);
        writer.writeBits(static_cast<uint32_t>(NetMessageType::Snapshot), 8);
        writer.writeVarUInt(static_cast<uint32_t>(tick));

        // The writer cannot rewind, so the budget has to decide the whole set before anything is
        // written. complete and the count are reserved here at the largest they can be, since
        // selected never exceeds candidates.
        constexpr int MaxStatePayloadBits = MaxStatePayloadBytes * 8;
        std::vector<const EntitySnapshot*> selected;
        int usedBits = writer.getBitsWritten() + 1 + varUIntBits(static_cast<uint32_t>(candidates.size()));
        for (const EntitySnapshot* entity : candidates) {
            // Charged what it actually costs. A single worst case price spent the packet on bits
            // nothing was going to write, since a resting body is half the size of a moving one.
            const int cost = entityBits(*entity);
            if (usedBits + cost > MaxStatePayloadBits) break;
            usedBits += cost;
            selected.push_back(entity);
        }
        client.lastSentEntities = static_cast<int>(selected.size());
        client.lastSkippedEntities = static_cast<int>(candidates.size() - selected.size());

        // Tells the client whether this packet is a restore point or only a correction. Resting
        // bodies left out do not count against it, since they have not moved.
        bool complete = selected.size() == candidates.size();
        if (!complete && budgetWarning.due()) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "State budget reached: %d of %d moving entities sent to peer %u, %d held "
                        "back (%d warnings suppressed). Packets are no longer complete, so clients "
                        "cannot replay from them and will snap instead. Raise MaxStatePayloadBytes "
                        "or replicate fewer moving bodies.",
                        client.lastSentEntities, static_cast<int>(candidates.size()),
                        client.peerId, client.lastSkippedEntities,
                        std::exchange(budgetWarning.suppressed, 0));
        }
        writer.serializeBool(complete);
        writer.writeVarUInt(static_cast<uint32_t>(selected.size()));
        for (const EntitySnapshot* entity : selected) {
            EntitySnapshot copy = *entity;
            serializeEntity(writer, copy);
        }

        // entityBits mirrors serializeEntity by hand, so a field added to one and missed by the
        // other surfaces here rather than as an oversized packet much later.
        SDL_assert(writer.getBitsWritten() <= usedBits);

        if (writer.hasOverflowed()) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "State packet for tick %llu overflowed the send buffer",
                         static_cast<unsigned long long>(tick));
            return;
        }
        writer.flush();

        // Only counted as sent once it is on the wire, so a failed send does not silence an entity
        // that never made it out.
        client.worstTicksSinceSent = 0;
        for (const EntitySnapshot* entity : selected) {
            const size_t index = entity->netId;
            client.lastSentState[index] = entity->state;
            client.lastSentOwner[index] = entity->ownerPeerId;
            if (client.pendingSends[index] > 0) --client.pendingSends[index];
            client.priority[index] = 0.0f;
            client.lastSentTick[index] = tick;
        }
        for (const EntitySnapshot* entity : candidates) {
            const Uint64 since = tick - client.lastSentTick[entity->netId];
            client.worstTicksSinceSent = std::max(client.worstTicksSinceSent, since);
        }

        const int size = writer.getBytesWritten();
        client.lastSentBytes = size;
        // Unreliable. A lost packet is replaced by the next one, and priority guarantees whatever it
        // was carrying comes back around.
        peer->sendUnreliable(connection, sendWords.data(), static_cast<uint32_t>(size));
        sentBytes.add(size);
    }

    // Everyone else's input, so a client can simulate the other players rather than watch them coast
    // between updates. Each recipient's own input is skipped, which also keeps the packet from
    // growing with the square of the player count.
    void ReplicationManager::sendPeerInput() {
        for (const uint32_t connection : peer->getConnections()) {
            const auto client = clients.find(connection);
            if (client == clients.end()) continue;

            // The whole clock feedback loop. Positive means this client's input is waiting for us.
            // A client that has not produced input yet is reported as 0, since taking tick 0 as its
            // position would report a lead of minus our whole uptime and land on its clock as one
            // enormous jump forward the moment it joins.
            const auto history = inputByPeer.find(client->second.peerId);
            client->second.inputLead = history == inputByPeer.end() || history->second.newestTick == 0 ? 0
                : static_cast<int32_t>(static_cast<int64_t>(history->second.newestTick) - static_cast<int64_t>(localTick));

            std::vector<uint32_t> forwarded;
            for (const auto& [peerId, peerHistory] : inputByPeer) {
                if (peerId == client->second.peerId || peerId == NoOwner) continue;
                forwarded.push_back(peerId);
            }

            sendWords.resize(SendBufferWords);
            BitWriter writer(sendWords.data(), SendBufferWords);
            writer.writeBits(static_cast<uint32_t>(NetMessageType::PeerInput), 8);
            writer.writeVarUInt(static_cast<uint32_t>(client->second.inputLead + InputLeadBias));
            writer.serializeCheck(CheckInput);
            writer.writeVarUInt(static_cast<uint32_t>(forwarded.size()));
            for (const uint32_t peerId : forwarded) {
                const Uint64 newestTick = inputByPeer[peerId].newestTick;
                writer.writeVarUInt(peerId);
                writer.writeVarUInt(static_cast<uint32_t>(newestTick));
                serializeInputWindow(writer, peerId, newestTick);
            }
            if (writer.hasOverflowed()) continue;
            writer.flush();

            const int bytes = writer.getBytesWritten();
            peer->sendUnreliable(connection, sendWords.data(), static_cast<uint32_t>(bytes));
            sentBytes.add(bytes);
        }
    }

    void ReplicationManager::onPeerInputMessage(const int size) {
        // Clients only. The host's clock is the one everyone else steers against.
        if (peer->isHosting()) return;

        BitReader reader(receiveWords.data(), size);
        reader.readBits(8);
        const auto reportedLead = static_cast<int32_t>(reader.readVarUInt()) - InputLeadBias;
        reader.serializeCheck(CheckInput);
        if (reader.hasOverflowed()) return;
        steerClock(reportedLead);
        const uint32_t forwardedPeers = reader.readVarUInt();
        for (uint32_t i = 0; i < forwardedPeers; ++i) {
            const uint32_t peerId = reader.readVarUInt();
            const Uint64 newestTick = reader.readVarUInt();
            if (reader.hasOverflowed() || newestTick > localTick + MaxInputLeadTicks) return;
            // Always consumed, even for a peer id we have no use for. Skipping the bits rather than
            // reading them would misalign the rest of the packet.
            serializeInputWindow(reader, peerId, newestTick);
        }
    }

    void ReplicationManager::onMessage(const uint32_t connection, const void* data, const uint32_t size) {
        // Bounded before the arithmetic below, since (size + 3) wraps on a huge size and under
        // allocates.
        constexpr uint32_t MaxMessageBytes = 1024 * 1024;
        if (!isBound() || size < 1 || size > MaxMessageBytes) return;
        const auto* bytes = static_cast<const uint8_t*>(data);
        receivedBytes.add(static_cast<int>(size));

        // BitReader loads the trailing partial word in full, so the payload is copied into a word
        // sized buffer rather than read in place off Steam's arbitrary length allocation. Only the
        // padding in the final word needs clearing, since the memcpy covers the rest.
        const uint32_t wordCount = (size + 3) / 4;
        if (receiveWords.size() < wordCount) receiveWords.resize(wordCount);
        receiveWords[wordCount - 1] = 0;
        std::memcpy(receiveWords.data(), bytes, size);

        switch (static_cast<NetMessageType>(bytes[0])) {
            case NetMessageType::Snapshot: 
                onSnapshotMessage(static_cast<int>(size)); 
                break;
            case NetMessageType::PeerInput:
                onPeerInputMessage(static_cast<int>(size));
                break;
            case NetMessageType::ClientInput:
                onClientInputMessage(connection, static_cast<int>(size));
                break;
            case NetMessageType::Welcome:
                onWelcomeMessage(static_cast<int>(size));
                break;
            case NetMessageType::PeerHello:
                onPeerHelloMessage(connection, static_cast<int>(size));
                break;
            case NetMessageType::PeerRoster:
                onPeerRosterMessage(static_cast<int>(size));
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

        // Unreliable delivery can duplicate or reorder, and a second copy would just take a slot.
        if (findReceived(snapshot.tick) != nullptr) return;

        // Every packet stands on its own. There is no baseline to rebuild against and nothing to
        // merge, so anything left out is simply not being updated yet.
        reader.serializeBool(snapshot.complete);
        const uint32_t entityCount = reader.readVarUInt();
        for (uint32_t i = 0; i < entityCount; ++i) {
            EntitySnapshot entity;
            serializeEntity(reader, entity);
            if (reader.hasOverflowed()) return;
            snapshot.entities.push_back(entity);
        }

        if (reader.hasOverflowed()) return;

        lastReceivedChanged = static_cast<int>(snapshot.entities.size());
        // The host sends on a fixed interval, so a jump of more than one interval is exactly the
        // snapshots that went missing. Duplicates and reorders are already gone.
        if (lastArrivedTick != 0 && snapshotSendInterval > 0
            && snapshot.tick > lastArrivedTick + snapshotSendInterval) {
            lostSnapshots += static_cast<int>((snapshot.tick - lastArrivedTick) / snapshotSendInterval) - 1;
            tickFlags |= NetGraphLost;
        }
        lastArrivedTick = std::max(lastArrivedTick, snapshot.tick);
        newestReceivedTick = std::max(newestReceivedTick, snapshot.tick);
        // A packet describing a tick we have not reached means the clock just moved and this
        // measurement says nothing about the connection.
        if (localTick > snapshot.tick) jitter.observe(localTick - snapshot.tick);
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
            sendPeerRoster();
        } else if (connection == peer->getHostConnection()) {
            resetReceivedState();
        }
    }

    void ReplicationManager::sendWelcome(const uint32_t connection, const uint32_t peerId) {
        sendWords.resize(SendBufferWords);
        BitWriter writer(sendWords.data(), SendBufferWords);
        writer.writeBits(static_cast<uint32_t>(NetMessageType::Welcome), 8);
        writer.writeVarUInt(peerId);
        writer.writeVarUInt(static_cast<uint32_t>(engine->getTickNumber()));
        if (writer.hasOverflowed()) return;
        writer.flush();
        peer->sendReliable(connection, sendWords.data(), static_cast<uint32_t>(writer.getBytesWritten()));
    }

    void ReplicationManager::onWelcomeMessage(const int size) {
        BitReader reader(receiveWords.data(), size);
        reader.readBits(8);
        const uint32_t peerId = reader.readVarUInt();
        const Uint64 hostTick = reader.readVarUInt();
        if (reader.hasOverflowed() || peer->isHosting()) return;
        localPeerId = peerId;

        // The host's numbering becomes ours, offset so we simulate a tick before it does. Seeded
        // from the round trip and then held there by the lead the host reports back. Neither half is
        // redundant, see docs/netcode.md, "The three delays".
        const Uint64 lead = leadFromPing();
        engine->setTickNumber(hostTick + lead);
        // We have an id now, so the host can put us in the roster and the others can dial us.
        sendPeerHello();
        // The slots are indexed by tick, and the numbering just changed under them.
        contactsValid = false;
        SDL_Log("Assigned peer id %u by the host, clock set to tick %llu (%llu ticks ahead).", peerId,
                static_cast<unsigned long long>(hostTick + lead), static_cast<unsigned long long>(lead));
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
                network.ownerPeerId = NoOwner;
            });
            inputByPeer.erase(departed);
            clients.erase(entry);
            sendPeerRoster();
        } else if (connection == peer->getHostConnection()) {
            resetReceivedState();
        }
    }

    void ReplicationManager::setOwner(const EntityId id, const uint32_t peerId) {
        if (!isBound() || !peer->isHosting()) return;
        if (const auto network = engine->getWorld().get<NetworkComponent>(id)) {
            network->ownerPeerId = peerId;
        }
    }

    uint32_t ReplicationManager::getPeerId(const uint32_t connection) const {
        const auto entry = clients.find(connection);
        return entry == clients.end() ? NoOwner : entry->second.peerId;
    }

    void ReplicationManager::resetReceivedState() {
        receivedHistory.clear();
        entityByNetId.clear();
        newestReceivedTick = 0;
        appliedTick = 0;
        localPeerId = NoOwner;
        inputByPeer.clear();
        dialledPeers.clear();
        predictedByNetId.clear();
        hasLastApplied = false;
        contactsValid = false;
        lastResimDepth = 0;
        clockCooldown = 0;
        jitter.reset();

        // The graph describes a connection, and this is a different one. Its loss count and its
        // tick numbering both mean nothing now.
        netGraph.clear();
        lastArrivedTick = 0;
        lostSnapshots = 0;
        starvedTotal = 0;
        tickCorrection = 0.0f;
        tickResimDepth = 0;
        tickFlags = 0;
    }

    void ReplicationManager::onClientInputMessage(const uint32_t connection, const int size) {
        BitReader reader(receiveWords.data(), size);
        reader.readBits(8);
        const uint32_t claimedPeerId = reader.readVarUInt();
        const Uint64 ackedTick = reader.readVarUInt();
        const Uint64 newestInputTick = reader.readVarUInt();
        if (reader.hasOverflowed()) return;
        // A tick out in the future would park this peer's history there, so every later read would
        // hand back neutral input forever, and the lead it implies is what the client steers its
        // clock by.
        if (newestInputTick > localTick + MaxInputLeadTicks) return;

        if (!peer->isHosting()) {
            // Straight from another client, one hop instead of two. Taking its word for who it is
            // costs nothing, since this only feeds our prediction of its ball and the host's state
            // still decides where that ball actually went.
            if (claimedPeerId != NoOwner && claimedPeerId != localPeerId) {
                serializeInputWindow(reader, claimedPeerId, newestInputTick);
            }
            return;
        }

        const auto entry = clients.find(connection);
        if (entry == clients.end()) return;
        ClientSyncState& client = entry->second;
        // A liveness readout only. Every packet is self-contained, so a client that never acks still
        // receives state.
        client.ackedTick = std::max(client.ackedTick, ackedTick);

        // Keyed by the peer id we assigned, never by what the client claims, so a client cannot
        // drive another player's body by lying about who it is.
        serializeInputWindow(reader, client.peerId, newestInputTick);
    }

    // A peer address is 64 bits over the relay and 16 on the local transport, and there is no
    // 64-bit varint, so it travels as two 32-bit halves.
    void ReplicationManager::sendPeerHello() {
        if (!peerMesh) return;
        sendWords.resize(SendBufferWords);
        BitWriter writer(sendWords.data(), SendBufferWords);
        writer.writeBits(static_cast<uint32_t>(NetMessageType::PeerHello), 8);
        const uint64_t address = peer->getPeerAddress();
        writer.writeVarUInt(static_cast<uint32_t>(address & 0xFFFFFFFFull));
        writer.writeVarUInt(static_cast<uint32_t>(address >> 32));
        if (writer.hasOverflowed()) return;
        writer.flush();
        peer->broadcastReliable(sendWords.data(), static_cast<uint32_t>(writer.getBytesWritten()));
    }

    void ReplicationManager::onPeerHelloMessage(const uint32_t connection, const int size) {
        if (!peer->isHosting()) return;
        BitReader reader(receiveWords.data(), size);
        reader.readBits(8);
        const uint64_t low = reader.readVarUInt();
        const uint64_t high = reader.readVarUInt();
        if (reader.hasOverflowed()) return;

        const auto entry = clients.find(connection);
        if (entry == clients.end()) return;
        entry->second.peerAddress = low | (high << 32);
        sendPeerRoster();
    }

    // Resent whole whenever membership changes. Reliable and tiny, and a client that already holds
    // an address ignores it.
    void ReplicationManager::sendPeerRoster() {
        if (!peer->isHosting()) return;

        sendWords.resize(SendBufferWords);
        BitWriter writer(sendWords.data(), SendBufferWords);
        writer.writeBits(static_cast<uint32_t>(NetMessageType::PeerRoster), 8);
        writer.writeVarUInt(static_cast<uint32_t>(clients.size()));
        for (const auto& [connection, client] : clients) {
            writer.writeVarUInt(client.peerId);
            writer.writeVarUInt(static_cast<uint32_t>(client.peerAddress & 0xFFFFFFFFull));
            writer.writeVarUInt(static_cast<uint32_t>(client.peerAddress >> 32));
        }
        if (writer.hasOverflowed()) return;
        writer.flush();
        peer->broadcastReliable(sendWords.data(), static_cast<uint32_t>(writer.getBytesWritten()));
    }

    void ReplicationManager::onPeerRosterMessage(const int size) {
        if (peer->isHosting() || !peerMesh) return;
        BitReader reader(receiveWords.data(), size);
        reader.readBits(8);
        const uint32_t count = reader.readVarUInt();
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t peerId = reader.readVarUInt();
            const uint64_t low = reader.readVarUInt();
            const uint64_t high = reader.readVarUInt();
            if (reader.hasOverflowed()) return;

            const uint64_t address = low | (high << 32);
            if (address == 0 || address == peer->getPeerAddress()) continue;
            // Only upwards, so exactly one side of each pair dials and links are not doubled.
            if (peerId <= localPeerId) continue;
            if (std::find(dialledPeers.begin(), dialledPeers.end(), address) != dialledPeers.end()) continue;
            dialledPeers.push_back(address);
            peer->connectToPeer(address);
        }
    }

    void ReplicationManager::sendClientInput() {
        const InputHistory& history = inputByPeer[localPeerId];
        // Nothing produced yet, which is every tick between the welcome and the ball we own being
        // named. Announcing tick 0 as our position is worse than saying nothing.
        if (history.newestTick == 0) return;

        sendWords.resize(SendBufferWords);
        BitWriter writer(sendWords.data(), SendBufferWords);
        writer.writeBits(static_cast<uint32_t>(NetMessageType::ClientInput), 8);
        // Only another client reads this. The host keys off the connection it arrived on instead.
        writer.writeVarUInt(localPeerId);
        writer.writeVarUInt(static_cast<uint32_t>(newestReceivedTick));
        writer.writeVarUInt(static_cast<uint32_t>(history.newestTick));
        serializeInputWindow(writer, localPeerId, history.newestTick);
        if (writer.hasOverflowed()) return;
        writer.flush();

        const int bytes = writer.getBytesWritten();
        peer->broadcastUnreliable(sendWords.data(), static_cast<uint32_t>(bytes));
        sentBytes.add(bytes);
    }

    ReplicationManager::ReplicationManager(Engine *inEngine, NetPeer *inPeer) {
        engine = inEngine;
        peer = inPeer;
    }

    void ReplicationManager::applyReceived(const Uint64 tick) {
        if (!isBound() || !peer->isActive()) return;
        localTick = tick;

        // The host is peer 1 rather than a special case, so its input takes the same path as
        // everyone else's.
        if (peer->isHosting()) localPeerId = HostPeerId;

        std::erase_if(correctionMarkers, [this](const CorrectionMarker& marker) {
            return localTick - marker.tick > CorrectionMarkerTicks;
        });

        sampleCorrection();
        // The host answers to nobody, so there is nothing to apply on it.
        if (peer->isHosting()) return;
        if (newestReceivedTick == 0) return;
        applyBufferedState();
        replayLateInput();
    }

    // Kept per tick alongside the predictions, since a replay has to start from the solver state
    // that belonged to the tick it is rewinding to.
    void ReplicationManager::saveContacts(const Uint64 tick) {
        physics::PhysicsManager::get().saveContacts(
            static_cast<int>(tick % physics::PhysicsManager::MaxSavedContacts));
        if (!contactsValid) {
            oldestContactTick = tick;
            contactsValid = true;
        }
        newestContactTick = tick;
        const Uint64 ringOldest = tick >= physics::PhysicsManager::MaxSavedContacts - 1
            ? tick - (physics::PhysicsManager::MaxSavedContacts - 1) : 0;
        oldestContactTick = std::max(oldestContactTick, ringOldest);
    }

    void ReplicationManager::recordPredictions(const Uint64 tick) {
        engine->getWorld().each<NetworkComponent, TransformComponent>(
            [&](const EntityId, const NetworkComponent& network, const TransformComponent& transform) {
                if (network.netId == 0) return;
                predictedByNetId[network.netId].write(tick, transform.getPosition(), transform.getRotation());
            });
    }

    // Both sides describe the same tick, so a body we extrapolated correctly does agree.
    bool ReplicationManager::predictionAgrees(const WorldSnapshot& snapshot) const {
        for (const EntitySnapshot& entity : snapshot.entities) {
            const auto predicted = predictedByNetId.find(entity.netId);
            if (predicted == predictedByNetId.end()) return false;

            glm::vec3 position;
            glm::quat rotation;
            glm::vec3 linear;
            glm::vec3 angular;
            dequantizeState(entity.state, position, rotation, linear, angular);
            if (!predicted->second.matches(snapshot.tick, position, rotation)) return false;
        }
        return true;
    }

    // The only authoritative state we keep is the last snapshot applied, so the replay starts there
    // rather than at the offending tick. That costs a few extra steps and avoids needing a saved
    // body state for every tick.
    void ReplicationManager::replayLateInput() {
        if (!lateInputRollback) return;
        Uint64 earliest = 0;
        for (auto& [peerId, history] : inputByPeer) {
            if (history.mispredictedTick != 0 && (earliest == 0 || history.mispredictedTick < earliest)) {
                earliest = history.mispredictedTick;
            }
            history.mispredictedTick = 0;
        }
        if (earliest == 0 || !hasLastApplied) return;
        // Already covered, since the host has since told us what actually happened at that tick.
        if (earliest <= lastApplied.tick) return;
        if (localTick <= lastApplied.tick || localTick - lastApplied.tick > MaxRollbackTicks) return;

        ++lateInputReplays;
        rollbackAndReplay(lastApplied, true);
    }

    void ReplicationManager::applyStates(const WorldSnapshot& snapshot) {
        for (const EntitySnapshot& entity : snapshot.entities) {
            const EntityId id = resolveEntity(entity.netId, entity.hostEntityId);
            if (id != NULL_ENTITY) applyState(id, entity, snapshot.tick);
        }
    }

    // The host's answer describes a tick we have already simulated past, so applying it on its own
    // would leave the whole world standing that far in the past. Replaying is the only thing that
    // keeps a remote player at the present rather than a round trip behind.
    // See docs/netcode.md, "Rollback and replay".
    void ReplicationManager::rollbackAndReplay(const WorldSnapshot& snapshot, const bool force) {
        World& world = engine->getWorld();
        const Uint64 target = snapshot.tick;

        ++predictionsChecked;
        if (!force && predictionAgrees(snapshot)) {
            ++predictionsKept;
            lastResimDepth = 0;
            return;
        }

        // Nothing between the packet and us to replay means applying it is the whole job.
        const bool willReplay = snapshot.complete
                             && localTick > target && localTick - target <= MaxRollbackTicks;

        // Where everything is drawn right now, so the rewind and the catch-up are hidden together as
        // one movement rather than a jump backwards followed by a scramble forwards. Taken on the
        // snap-only path too, which moves bodies furthest and so needs the smoothing most.
        const float alpha = engine->getRenderAlpha();
        world.each<RigidbodyComponent>([alpha](const EntityId, RigidbodyComponent& rigidbody) {
            rigidbody.captureVisualReference(alpha);
        });

        // Before the bodies move onto the snapshot, so the first replayed step solves from the
        // contacts that belonged to this tick rather than the ones the present left behind.
        if (willReplay && contactsValid && target >= oldestContactTick && target <= newestContactTick) {
            // Refused means a body was created or destroyed since that slot was written, which
            // empties every slot rather than just this one, so the window restarts from next tick.
            if (!physics::PhysicsManager::get().restoreContacts(
                    static_cast<int>(target % physics::PhysicsManager::MaxSavedContacts))) {
                contactsValid = false;
            }
        }

        applyStates(snapshot);

        if (!willReplay) {
            if (!snapshot.complete) ++snappedIncomplete;
            else if (localTick > target) ++snappedTooDeep;
            // The world is left however far back this packet reaches, and corrections land as
            // visible snaps from here on.
            if (replaySkippedWarning.due()) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Correction applied without replaying: %s (%d suppressed). The world is "
                            "left %llu ticks in the past and every correction is felt.",
                            snapshot.complete ? "the packet is further back than MaxRollbackTicks"
                                              : "the host's budget left the packet incomplete",
                            std::exchange(replaySkippedWarning.suppressed, 0),
                            static_cast<unsigned long long>(localTick > target ? localTick - target : 0));
            }
            lastResimDepth = 0;
            tickFlags |= NetGraphSnapped;
        } else {
            // Up to but not including localTick: applyReceived runs before this tick is stepped, and
            // the caller's own simulateStep is what finishes the catch-up.
            for (Uint64 tick = target + 1; tick < localTick; ++tick) {
                engine->simulateStep(tick, Engine::FIXED_DT);
                // The replay rewrites these ticks, so what was recorded the first time through is
                // now wrong. Left stale, one divergence becomes a resimulation on every packet from
                // then on, since the next packet can never agree with a prediction this correction
                // already invalidated.
                recordPredictions(tick);
                saveContacts(tick);
            }
            lastResimDepth = static_cast<int>(localTick - target - 1);
            deepestResim = std::max(deepestResim, lastResimDepth);
            resimStepsThisWindow += lastResimDepth;
            tickFlags |= NetGraphReplayed;
            // Summed rather than assigned: a tick can release more than one held packet, and the
            // column is meant to show what the tick cost in total.
            tickResimDepth += lastResimDepth;
        }

        world.each<RigidbodyComponent>([alpha](const EntityId, RigidbodyComponent& rigidbody) {
            rigidbody.applyVisualReference(alpha);
        });
    }

    void ReplicationManager::applyState(const EntityId id, const EntitySnapshot& entity, const Uint64 tick) {
        World& world = engine->getWorld();
        glm::vec3 position;
        glm::quat rotation;
        glm::vec3 linear;
        glm::vec3 angular;
        dequantizeState(entity.state, position, rotation, linear, angular);

        // Judged against what we predicted for this same tick, never against where the body stands
        // now. The distance to the current pose is mostly the clock lead, so measuring that would
        // report a large error on a body predicted perfectly.
        ReceivedState& received = lastReceivedByNetId[entity.netId];
        received.state = entity.state;
        glm::quat predictedRotation;
        const auto predicted = predictedByNetId.find(entity.netId);
        received.hadPrediction = predicted != predictedByNetId.end()
                              && predicted->second.poseAt(tick, received.predicted, predictedRotation);
        if (received.hadPrediction) {
            const float error = glm::distance(received.predicted, position);
            correction.total += error;
            correction.totalAngle += angleBetween(predictedRotation, rotation);
            ++correction.count;
            // The worst body decides the column, since an average over a mostly settled world would
            // hide the one that was wrong.
            tickCorrection = std::max(tickCorrection, error);
            if (error > CorrectionMarkerMinimum) {
                correctionMarkers.push_back({ received.predicted, position, localTick });
            }
        }

        if (RigidbodyComponent* rigidbody = world.get<RigidbodyComponent>(id)) {
            rigidbody->setState(position, rotation, linear, angular, entity.state.atRest);
        } else if (TransformComponent* transform = world.get<TransformComponent>(id)) {
            transform->setPosition(position);
            transform->setRotation(rotation);
        }
    }

    EntityId ReplicationManager::resolveEntity(const uint32_t netId, const uint32_t hostEntityId) {
        const auto cached = entityByNetId.find(netId);
        if (cached != entityByNetId.end()) return cached->second;

        // Scene loading preserves entity ids, so the host's id names our copy too. A runtime spawned
        // entity has no local copy and needs spawn data.
        World& world = engine->getWorld();
        if (world.getEntity(hostEntityId) == nullptr || world.get<NetworkComponent>(hostEntityId) == nullptr) {
            // The two scenes differ, whether from a stale copy, an unsaved edit, or a runtime spawn.
            // This body will never be replicated here, and it fails silently otherwise.
            if (sceneMismatchWarning.due()) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "The host is replicating netId %u as entity %u, which does not exist "
                            "here or is not networked (%d suppressed). The two scenes differ.",
                            netId, hostEntityId, std::exchange(sceneMismatchWarning.suppressed, 0));
            }
            return NULL_ENTITY;
        }

        NetworkComponent* network = world.get<NetworkComponent>(hostEntityId);
        network->netId = netId;
        entityByNetId[netId] = hostEntityId;
        return hostEntityId;
    }

    // Held briefly rather than applied on arrival, because packets clump and a clump puts objects
    // from different packets out of phase in time, which pops anything stacked on something else.
    void ReplicationManager::applyBufferedState() {
        World& world = engine->getWorld();
        // Applied in place rather than consumed, so a duplicate arriving later still finds its tick
        // and is recognised. appliedTick stops one being applied twice, and stops a reordered older
        // packet dragging the world backwards.
        for (const WorldSnapshot& snapshot : receivedHistory) {
            if (snapshot.tick <= appliedTick) continue;
            // Released at a fixed transit rather than after a fixed wait, so an early packet waits
            // and a late one goes straight through.
            if (localTick < snapshot.tick + jitter.target) break;

            for (const EntitySnapshot& entity : snapshot.entities) {
                const EntityId id = resolveEntity(entity.netId, entity.hostEntityId);
                if (id == NULL_ENTITY) continue;
                if (NetworkComponent* network = world.get<NetworkComponent>(id)) {
                    network->ownerPeerId = entity.ownerPeerId;
                }
            }

            rollbackAndReplay(snapshot);
            appliedTick = snapshot.tick;
            lastApplied = snapshot;
            hasLastApplied = true;
            tickFlags |= NetGraphApplied;
        }
    }

    const WorldSnapshot* ReplicationManager::findReceived(const Uint64 tick) const {
        for (const WorldSnapshot& snapshot : receivedHistory) {
            if (snapshot.tick == tick) return &snapshot;
        }
        return nullptr;
    }

    void ReplicationManager::drawNetDebug(DebugDraw& debug) const {
        if (!isBound()) return;
        const World& world = engine->getWorld();

        if (showGhosts) {
            for (const auto& [netId, received] : lastReceivedByNetId) {
                const auto mapped = entityByNetId.find(netId);
                if (mapped == entityByNetId.end()) continue;
                const RigidbodyComponent* rigidbody = world.get<RigidbodyComponent>(mapped->second);
                const TransformComponent* transform = world.get<TransformComponent>(mapped->second);
                if (transform == nullptr) continue;

                glm::vec3 hostPosition;
                glm::quat hostRotation;
                glm::vec3 hostLinear;
                glm::vec3 hostAngular;
                dequantizeState(received.state, hostPosition, hostRotation, hostLinear, hostAngular);

                // Measured against where we had this body at the tick the ghost describes, not where
                // it stands now. A ghost trails the body by the clock lead plus the trip plus the
                // jitter hold whatever happens, and none of that is error.
                const float divergence = received.hadPrediction
                    ? glm::distance(hostPosition, received.predicted) : 0.0f;
                const float scaled = glm::clamp(divergence / DivergenceFullScale, 0.0f, 1.0f);
                const glm::vec4 color(scaled, 1.0f - scaled, 0.2f, 1.0f);

                debug.wireSphere(hostPosition, 0.25f, color, 12);
                if (received.hadPrediction) debug.line(hostPosition, received.predicted, color);
                // Where the host thinks it is heading. A ghost sitting still while our copy moves
                // means the host is not applying the input driving it here.
                if (!received.state.atRest) {
                    debug.arrow(hostPosition, hostPosition + hostLinear * 0.25f,
                                glm::vec4(0.4f, 0.7f, 1.0f, 1.0f));
                }

                // Simulated pose to drawn pose. Long means a big correction is mid-fade.
                if (showVisualOffset && rigidbody != nullptr) {
                    const glm::vec3 simulated = transform->getPosition();
                    const glm::vec3 drawn = simulated + rigidbody->getVisualPositionError();
                    debug.line(simulated, drawn, glm::vec4(1.0f, 0.9f, 0.2f, 1.0f));
                    debug.wireSphere(drawn, 0.12f, glm::vec4(1.0f, 0.9f, 0.2f, 1.0f), 8);
                }
            }
        }

        if (showCorrections) {
            for (const CorrectionMarker& marker : correctionMarkers) {
                const Uint64 age = localTick - marker.tick;
                const float fade = 1.0f - static_cast<float>(age) / static_cast<float>(CorrectionMarkerTicks);
                debug.arrow(marker.from, marker.to, glm::vec4(1.0f, 0.2f, 0.8f, fade));
            }
        }
    }

    static void ownerLabel(const uint32_t peerId, char* buffer, const size_t size) {
        if (peerId == ReplicationManager::NoOwner) {
            SDL_strlcpy(buffer, "nobody", size);
        } else if (peerId == ReplicationManager::HostPeerId) {
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
            peerIds.reserve(clients.size() + 2);
            peerIds.push_back(NoOwner);
            peerIds.push_back(HostPeerId);
            for (const auto& [connection, client] : clients) peerIds.push_back(client.peerId);
            std::sort(peerIds.begin(), peerIds.end());
        }
        // Applied after the pass: setOwner touches the component the iteration is walking.
        EntityId reassignEntity = NULL_ENTITY;
        uint32_t reassignPeerId = NoOwner;

        if (ImGui::BeginTable("netentities", 3,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Net Id");
            ImGui::TableSetupColumn("Owner");
            ImGui::TableSetupColumn("Motion");
            ImGui::TableHeadersRow();

            World& world = engine->getWorld();
            world.each<NetworkComponent>([&](const EntityId id, const NetworkComponent& network) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%u", network.netId);
                ImGui::TableNextColumn();
                if (!hosting) {
                    char label[16];
                    ownerLabel(network.ownerPeerId, label, sizeof(label));
                    ImGui::TextUnformatted(label);
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
                const RigidbodyComponent* rigidbody = world.get<RigidbodyComponent>(id);
                if (rigidbody == nullptr) {
                    ImGui::TextUnformatted("-");
                } else {
                    ImGui::TextUnformatted(rigidbody->isAtRest() ? "at rest" : "moving");
                }
            });
            ImGui::EndTable();
        }
        if (reassignEntity != NULL_ENTITY) setOwner(reassignEntity, reassignPeerId);
        ImGui::TreePop();
    }

    void ReplicationManager::drawNetGraphControls() {
        if (!isBound()) return;
        ImGui::Checkbox("Net Graph", &netGraph.visible);
        ImGui::SetItemTooltip("Per-tick connection health, drawn over the game. It takes no input, so "
                              "nothing behind it becomes unclickable.");
        if (!netGraph.visible) return;

        ImGui::SameLine();
        ImGui::SetNextItemWidth(130.0f);
        const char* corners[] = { "Top left", "Top right", "Bottom left", "Bottom right" };
        ImGui::Combo("##netgraphcorner", &netGraph.corner, corners, IM_ARRAYSIZE(corners));
        ImGui::SameLine();
        ImGui::Checkbox("Numbers", &netGraph.showStats);
    }

    void ReplicationManager::drawNetGraph() {
        if (!netGraph.visible || !isBound()) return;

        NetGraphStats stats;
        stats.hosting = peer->isHosting();
        const std::vector<uint32_t>& connections = peer->getConnections();
        stats.connections = static_cast<int>(connections.size());
        stats.simulated = peer->getNetSim().isActive();
        stats.inKbPerSec = receivedBytes.bytesPerSecond / 1024.0f;
        stats.outKbPerSec = sentBytes.bytesPerSecond / 1024.0f;

        // A client reports the host's link specifically, since the ping to a peer link says nothing
        // about the clock. A host has no single link, so it reports the worst of them.
        NetConnectionStats link;
        if (stats.hosting) {
            for (const uint32_t connection : connections) {
                NetConnectionStats candidate;
                if (peer->getStats(connection, candidate) && candidate.pingMs > link.pingMs) link = candidate;
            }
        } else {
            peer->getStats(peer->getHostConnection(), link);
        }
        stats.pingMs = link.pingMs;
        stats.lossPct = link.qualityLocal < 0.0f ? -1.0f : (1.0f - link.qualityLocal) * 100.0f;

        constexpr float msPerTick = Engine::FIXED_DT * 1000.0f;
        if (stats.hosting) {
            stats.replicatedEntities = lastSnapshotEntities;
        } else {
            // What the local player actually feels, since their own ball does not act until then.
            stats.inputDelayTicks = static_cast<int>(inputDelayTicks);
            stats.inputLagMs = static_cast<float>(inputDelayTicks) * msPerTick;
            stats.inputLeadTicks = measuredInputLead;
            stats.inputLeadTarget = inputLeadTarget();
            stats.jitterHoldTicks = static_cast<int>(jitter.target);
            stats.jitterHoldMs = static_cast<float>(jitter.target) * msPerTick;
            // Nothing applied yet is no world at all rather than an infinitely old one.
            stats.stateAgeMs = appliedTick != 0 && localTick > appliedTick
                             ? static_cast<float>(localTick - appliedTick) * msPerTick : 0.0f;
            stats.correctionMean = correction.mean;
            stats.correctionPeak = correction.peak;
            stats.toleranceMetres = predictionTolerance;
            stats.resimStepsPerSecond = resimStepsPerSecond;
            stats.deepestResim = deepestResim;
            stats.keptPct = predictionsChecked > 0
                          ? 100.0f * static_cast<float>(predictionsKept) / static_cast<float>(predictionsChecked)
                          : -1.0f;
            stats.lostSnapshots = lostSnapshots;
            stats.starvedInputs = starvedTotal;
        }

        netGraph.draw(stats);
    }

    void ReplicationManager::drawDebugUI() {
        if (!isBound() || !ImGui::CollapsingHeader("Replication")) return;

        ImGui::Text("Bandwidth: %.1f KB/s out, %.1f KB/s in",
                    sentBytes.bytesPerSecond / 1024.0f, receivedBytes.bytesPerSecond / 1024.0f);
        ImGui::Checkbox("Ghosts (host state)", &showGhosts);
        ImGui::SameLine();
        ImGui::Checkbox("Corrections", &showCorrections);
        ImGui::SameLine();
        ImGui::Checkbox("Visual offset", &showVisualOffset);
        if (showCorrections) {
            ImGui::Text("%d markers live", static_cast<int>(correctionMarkers.size()));
        }
        ImGui::Separator();

        if (peer->isHosting()) {
            ImGui::Text("Peer id: host (%u)", localPeerId);
            ImGui::Text("Replicated: %d entities", lastSnapshotEntities);
            for (const auto& [connection, client] : clients) {
                if (!ImGui::TreeNode(reinterpret_cast<void*>(static_cast<uintptr_t>(connection)),
                                     "conn %u  peer %u  %d B  %d sent", connection, client.peerId,
                                     client.lastSentBytes, client.lastSentEntities)) {
                    continue;
                }
                ImGui::Text("Acked tick: %llu", static_cast<unsigned long long>(client.ackedTick));
                // Skipped is the budget doing its job. Ticks since sent is whether it is fair, so a
                // number that keeps climbing means something is starving.
                ImGui::Text("Skipped by budget: %d", client.lastSkippedEntities);
                ImGui::Text("Worst ticks since sent: %llu",
                            static_cast<unsigned long long>(client.worstTicksSinceSent));
                ImGui::Text("Input lead: %d ticks", client.inputLead);
                if (client.lastSentBytes > SoftPacketBytes) {
                    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                                       "Packet exceeds %d B and will fragment", SoftPacketBytes);
                }
                ImGui::TreePop();
            }
            // Age should sit near the clock lead plus the trip from the host. Growing without bound
            // means that peer stopped sending.
            for (const auto& [peerId, history] : inputByPeer) {
                ImGui::Text("peer %u input: %lld ticks old, %d repeated, %d starved", peerId,
                            static_cast<long long>(static_cast<int64_t>(localTick) - static_cast<int64_t>(history.newestTick)),
                            history.repeated, history.starved);
            }
        } else {
            ImGui::Text("Peer id: %u", localPeerId);
            ImGui::Text("Newest tick: %llu", static_cast<unsigned long long>(newestReceivedTick));
            ImGui::Text("Applied tick: %llu (%llu behind)",
                        static_cast<unsigned long long>(appliedTick),
                        static_cast<unsigned long long>(newestReceivedTick - appliedTick));
            // How far each snap moves a body, and the number every prediction change is judged
            // against. Near zero means the local simulation agreed with the host.
            ImGui::Text("Correction: %.3f m mean, %.3f m peak, %.1f deg mean",
                        correction.mean, correction.peak, glm::degrees(correction.meanAngle));
            ImGui::PlotLines("##correction", correctionHistory.data(),
                             static_cast<int>(correctionHistory.size()), correctionHistoryIndex,
                             nullptr, 0.0f, 0.5f, ImVec2(0.0f, 40.0f));
            // The hold is what the connection asked for, not what it was configured with. Worst
            // transit sitting far above it means the estimator has shrunk since a spike.
            ImGui::Text("Buffered: %d packets, holding %llu ticks (worst transit %llu)",
                        static_cast<int>(receivedHistory.size()),
                        static_cast<unsigned long long>(jitter.target),
                        static_cast<unsigned long long>(jitter.worstTransit));
            // Should sit at the target. Drifting below means our input is reaching the host late
            // and it is having to guess.
            ImGui::Text("Input lead: %d ticks (target %d, %llu of it delay), %d clock adjustments",
                        measuredInputLead, inputLeadTarget(),
                        static_cast<unsigned long long>(inputDelayTicks), clockAdjustments);
            ImGui::Text("Seeded lead from ping: %llu ticks",
                        static_cast<unsigned long long>(leadFromPing()));
            ImGui::SeparatorText("Resimulation");
            // Extra physics on top of the one step a tick normally runs, so 60 here is twice the
            // work.
            ImGui::Text("Replayed steps: %.0f/s, last %d deep, worst %d",
                        resimStepsPerSecond, lastResimDepth, deepestResim);
            // High is good: the prediction was already right and nothing had to be replayed.
            ImGui::Text("Prediction kept: %d packets", predictionsKept);
            // A press cannot be recovered any other way, since the tick it belongs to is behind us.
            ImGui::Text("Late input replays: %d", lateInputReplays);
            if (snappedTooDeep > 0 || snappedIncomplete > 0) {
                // Each of these left the world a rollback's worth of time in the past.
                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f),
                                   "Snapped without replay: %d too deep, %d incomplete",
                                   snappedTooDeep, snappedIncomplete);
            }

            drawTuning();
            ImGui::Text("Mapped entities: %d", static_cast<int>(entityByNetId.size()));
            // Zero while nothing moves is at-rest detection working.
            ImGui::Text("Entities in last packet: %d", lastReceivedChanged);
            // Age should sit near the clock lead plus the trip from the host. Growing without bound
            // means that peer stopped sending.
            for (const auto& [peerId, history] : inputByPeer) {
                ImGui::Text("peer %u input: %lld ticks old, %d starved", peerId,
                            static_cast<long long>(static_cast<int64_t>(localTick) - static_cast<int64_t>(history.newestTick)),
                            history.starved);
            }
        }

        drawEntityTable();
    }
} // ytail::net
