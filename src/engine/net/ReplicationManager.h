//
// Created by Peter Gilbert on 8/8/26.
//

#ifndef YELLOWTAIL_REPLICATIONMANAGER_H
#define YELLOWTAIL_REPLICATIONMANAGER_H

#include <array>
#include <cstdint>
#include <deque>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <SDL3/SDL_stdinc.h>

#include <glm/vec3.hpp>
#include <glm/gtc/quaternion.hpp>

#include "NetArchive.h"
#include "NetGraph.h"
#include "engine/Entity.h"
#include "engine/managers/PhysicsManager.h"

namespace ytail {
    class Engine;
    class DebugDraw;
    class NetworkComponent;
    class TransformComponent;
}

namespace ytail::net {
    class NetPeer;

    // Bytes over a sampling window, turned into a rate for the debug panel.
    struct BandwidthMeter {
        int bytesThisWindow = 0;
        float bytesPerSecond = 0.0f;
        Uint64 windowStartMs = 0;
        // The same bytes over a single tick, drained into one netgraph column. Separate from the
        // window because the graph wants the shape of the traffic, not its average.
        int bytesThisTick = 0;

        void add(const int bytes) {
            bytesThisWindow += bytes;
            bytesThisTick += bytes;
        }
        int takeTickBytes() {
            const int bytes = bytesThisTick;
            bytesThisTick = 0;
            return bytes;
        }
    };

    struct EntitySnapshot {
        uint32_t netId = 0;
        // The host's EntityId. Scene loading preserves ids, so a client can match this to its own
        // copy. TODO only needed the first time a client sees a netId; move it into spawn records
        // along with the component data a runtime-spawned entity needs.
        uint32_t hostEntityId = 0;
        uint32_t ownerPeerId = 0;
        QuantizedState state;
    };

    // One peer's recent input, kept by shared tick so a frame lands in the same place on every
    // machine. Reads ask for a tick and get the newest frame at or before it, so a gap carries the
    // last input forward rather than dropping to neutral: a rolling ball whose owner is still
    // holding forward must keep being pushed while the next packet is late.
    struct InputHistory {
        static constexpr size_t Length = 64;
        std::array<NetInputFrame, Length> frames{};
        std::array<Uint64, Length> ticks{};
        Uint64 newestTick = 0;
        // Ticks driven with nothing at all, because the peer went quiet for longer than
        // MaxInputStaleTicks. Repeating the newest frame is not counted: that is the normal case
        // for anyone but ourselves, since we simulate ahead of what we hold for them.
        int starved = 0;
        // Ticks driven with a repeat of an older frame because the exact one had not arrived. Held
        // buttons survive that; a press does not, so this is what a missing jump looks like.
        int repeated = 0;

        // What read() actually handed back for a tick, which is not always what we hold: a tick we
        // had no frame for was driven by carrying an older one forward. Keeping it is what lets a
        // late arrival be recognised as a guess we got wrong rather than quietly filed.
        std::array<NetInputFrame, Length> used{};
        std::array<Uint64, Length> usedTicks{};
        // Earliest tick whose real frame turned out to differ from the guess we simulated it with.
        // 0 when there is nothing to redo.
        Uint64 mispredictedTick = 0;

        void write(Uint64 tick, const NetInputFrame& frame);
        // Exactly what was stored for this tick, or neutral. Does not count as starvation.
        [[nodiscard]] NetInputFrame at(Uint64 tick) const;
        [[nodiscard]] NetInputFrame read(Uint64 tick);
    };

    // Where we predicted every replicated body would be, by tick, so an arriving state can be
    // checked against what we already had rather than replacing it blindly. Pose, not just position:
    // a ball can sit within centimetres of where the host has it while its spin has drifted, and on
    // anything with a visible surface that is the more obvious error of the two.
    struct PredictedPoses {
        static constexpr size_t Length = 64;
        std::array<glm::vec3, Length> positions{};
        std::array<glm::quat, Length> rotations{};
        std::array<Uint64, Length> ticks{};

        void write(Uint64 tick, const glm::vec3& position, const glm::quat& rotation);
        [[nodiscard]] bool poseAt(Uint64 tick, glm::vec3& outPosition, glm::quat& outRotation) const;
        // False when nothing is recorded for that tick, which counts as disagreement.
        [[nodiscard]] bool matches(Uint64 tick, const glm::vec3& position, const glm::quat& rotation) const;
    };

    // How long a packet is held before it is applied, learned from the connection rather than fixed
    // for the worst case it might ever see. The measurement is transit: how many ticks behind us the
    // packet already was when it landed. Releasing every packet at the same transit spaces them
    // evenly however they arrived, so an early one waits and a late one goes straight out.
    struct JitterEstimator {
        Uint64 target = 0;
        Uint64 windowMax = 0;
        int windowPackets = 0;
        // Worst transit seen since the counters were last cleared, for the debug readout.
        Uint64 worstTransit = 0;

        void observe(Uint64 transit);
        void reset() { *this = {}; }
    };

    // The host's newest word on an entity, kept alongside what we had predicted for the same tick.
    // Comparing those two is the only honest measure of prediction: comparing the host's answer to
    // where the body stands now measures the clock lead, and the lead is the system working.
    struct ReceivedState {
        QuantizedState state;
        glm::vec3 predicted{0.0f};
        bool hadPrediction = false;
    };

    // Position and angular error of one applied correction, averaged over a sampling window.
    struct CorrectionMeter {
        float total = 0.0f;
        float totalAngle = 0.0f;
        int count = 0;
        float mean = 0.0f;
        float meanAngle = 0.0f;
        float peak = 0.0f;
    };

    // A condition that persists rather than an event that happened once, so the log is throttled:
    // the first one is the useful one and a per-tick repeat would bury everything around it. The
    // count of swallowed repeats goes out with the next line, so a rare condition and a constant one
    // do not read the same.
    struct ThrottledWarning {
        Uint64 lastLoggedMs = 0;
        int suppressed = 0;

        [[nodiscard]] bool due();
    };

    // One applied correction, kept briefly so it can be seen after the fact.
    struct CorrectionMarker {
        glm::vec3 from{0.0f};
        glm::vec3 to{0.0f};
        Uint64 tick = 0;
    };

    // Kept sorted by netId, which is the order the host writes them in.
    struct WorldSnapshot {
        // The shared tick this describes, always behind us because we run ahead of the host. How far
        // behind is the transit the estimator measures, and is what the release is timed against.
        Uint64 tick = 0;
        // Every moving entity is present, so this packet is a complete restore point for the tick.
        // False when the budget forced some out, which makes it safe to apply but not to replay
        // from: the bodies it left out are still standing at the tick we have reached, and stepping
        // those forward again from there would send them somewhere they were never going.
        bool complete = true;
        std::vector<EntitySnapshot> entities;
    };

    // What each client is owed. Every entity accumulates priority until it is sent, so a packet
    // carries the most valuable subset that fits and nothing starves: one passed over keeps climbing
    // until it wins.
    struct ClientSyncState {
        uint32_t peerId = 0;
        Uint64 ackedTick = 0;
        // Ticks this client's input is arriving ahead of the tick we are about to simulate.
        int32_t inputLead = 0;

        // How other clients reach this one directly, learned from its PeerHello and handed to
        // everybody in the roster.
        uint64_t peerAddress = 0;

        // All indexed by netId, grown on demand.
        std::vector<float> priority;
        std::vector<QuantizedState> lastSentState;
        // Owner is not part of the state, so a handover on a body that is standing still changes
        // nothing this client would otherwise be told about.
        std::vector<uint32_t> lastSentOwner;
        // Sends still owed for an entity whose state stopped changing. A settled body would
        // otherwise go quiet on the very packet that told everyone it settled, and if that packet
        // is lost it rests in the wrong place on this peer forever.
        std::vector<uint8_t> pendingSends;
        std::vector<Uint64> lastSentTick;

        int lastSentBytes = 0;
        int lastSentEntities = 0;
        int lastSkippedEntities = 0;
        Uint64 worstTicksSinceSent = 0;

        void grow(size_t netIdCount);
    };

    // State synchronization. Every peer simulates every body and shares one tick timeline, with the
    // host as the clock. Clients run a little ahead of it so their input arrives before the host
    // needs it, send input rather than positions, and snap received state onto the running
    // simulation, hiding the jump with RigidbodyComponent's visual error offset. Nothing is ever
    // rewound: the host's answer is taken as it lands and the eye is what gets smoothed.
    // reference: https://www.gafferongames.com/post/state_synchronization/
    // Owned by Engine, inert until bind() supplies a peer.
    class ReplicationManager {
    public:
        ReplicationManager() = default;
        ReplicationManager(Engine* inEngine, NetPeer* inPeer);
        void attach(Engine* inEngine) { engine = inEngine; }
        void bind(NetPeer* inPeer) { peer = inPeer; }
        [[nodiscard]] bool isBound() const { return engine != nullptr && peer != nullptr; }

        // Runs before the physics step, so a correction lands on the step that follows it.
        void applyReceived(Uint64 tick);

        // Runs after the world's fixed tick and its deferred flush.
        void capture(Uint64 tick);
        void onMessage(uint32_t connection, const void* data, uint32_t size);
        void onConnected(uint32_t connection);
        void onDisconnected(uint32_t connection);
        void drawDebugUI();
        // The overlay: a per-tick strip of how the connection is behaving, plus the numbers under it.
        // Drawn outside the debug window on purpose, so it survives closing it.
        void drawNetGraph();
        // The toggle and where it sits. Meant for the top of the debug window: the point of the
        // graph is to be reachable without scrolling past everything else first.
        void drawNetGraphControls();
        [[nodiscard]] NetGraph& getNetGraph() { return netGraph; }

        // Appends every replicated body's pose, as this peer sees it, once per tick. Joining two
        // peers' traces on (tick, netId) is the only way to ask "did A predict B correctly" without
        // watching two windows and guessing. No-op until a path is supplied.
        void openTrace(const std::string& path);
        // Ghost of the last state the host sent for each entity, the divergence from where we have
        // it, the offset being hidden from the eye, and a fading marker for each correction.
        void drawNetDebug(DebugDraw& debug) const;
        // Where the host says things are, and how far off we are.
        bool showGhosts = false;
        // A line per correction, left up briefly so a jump can be seen after it happens.
        bool showCorrections = false;
        // The gap between the simulated pose and the drawn one: what the smoothing is covering up.
        bool showVisualOffset = false;

        // Host only. Hands an entity to a peer, or back to the host with HostPeerId. The change
        // reaches everyone through the snapshot, which repeats it until acked.
        void setOwner(EntityId id, uint32_t peerId);
        [[nodiscard]] uint32_t getLocalPeerId() const { return localPeerId; }
        // Host only: the peer number assigned to a connection, or NoOwner if it has none yet.
        [[nodiscard]] uint32_t getPeerId(uint32_t connection) const;

        // What the local player pressed this tick. Stored under our own peer id and sent to the
        // host, which applies it and passes it on to everyone else.
        void setLocalInput(Uint64 tick, const NetInputFrame& frame);
        // What a peer was pressing at a tick, for whoever is simulating the body it drives.
        [[nodiscard]] NetInputFrame getInput(uint32_t peerId, Uint64 tick);
        // True once we have produced input for this tick. False means it is a tick we have not
        // lived through yet, so the controls should be read; true means a replay is passing back
        // over one, and reading again would rewrite what was actually pressed.
        [[nodiscard]] bool hasLocalInputFor(Uint64 tick) const;

        // 0 means nobody owns this entity; the host is a peer like any other so its input can be
        // looked up and forwarded the same way.
        static constexpr uint32_t NoOwner = 0;
        static constexpr uint32_t HostPeerId = 1;

        // Live rather than constexpr: these are judged by watching balls under lag, and this
        // project is far too expensive to rebuild once per guess. Sliders are in drawTuning.
        //
        // Fixed ticks between snapshots: 6 at 60Hz is 10 per second. Halving it halves how long a
        // mispredicted remote player runs before it is corrected, at double the state bandwidth.
        static Uint64 snapshotSendInterval;
        // Floor under the measured jitter hold, for a connection whose first packets happen to
        // arrive evenly.
        static Uint64 minJitterTicks;
        // Slack above the worst transit measured, so a packet a shade later than anything seen so
        // far is still covered rather than being the one that pops a stack.
        static Uint64 jitterMargin;
        // Packets per shrink step. The hold jumps up the moment a late packet needs it and comes
        // back down one tick per window, so one hiccup does not pin it high forever.
        static int jitterWindowPackets;
        // Ticks between reading local input and the tick it is applied on, which is the head start
        // it gets to reach the other peers. Cover the trip and nobody has to predict anybody; two
        // ticks does that on a local connection. It cannot scale to a real internet trip, where the
        // delay would have to be longer than the lag it was hiding.
        static Uint64 inputDelayTicks;
        // Follow the connection instead of holding a number that is wasteful on a good link and
        // short on a bad one. Peers do not have to agree on it: a peer's delay only decides how
        // early its own input reaches everyone else.
        static bool adaptiveInputDelay;
        // Replay a tick when a peer's real input for it turns up after we already guessed
        static bool lateInputRollback;
        // Direct client-to-client links for input. Off falls back to the host relaying everyone's
        // input, which costs a hop and so a worse prediction of the other players, but takes the
        // whole peer listen/dial path out of the picture.
        static bool peerMesh;
        // Ceiling on that. Past it prediction degrades rather than the controls getting heavier, and
        // what degrades first is only the host's ball: that one costs a round trip, our input out and
        // its input back, while a body predicted over a direct link costs one hop and stays covered.
        static Uint64 maxInputDelayTicks;
        static constexpr Uint64 MinInputDelayTicks = 2;
        // Ticks between adjustments, so the delay does not chase every ping sample.
        static constexpr int InputDelayAdjustCooldown = 60;
        // How far a body may sit from where the host put it before the whole world is rewound and
        // replayed. Under this the prediction was good enough to keep, and a resimulation would burn
        // steps arriving where we already are. Loose enough to absorb the 2mm the wire quantizes to
        // plus solver noise; too tight and every packet resimulates to prove nothing was wrong.
        static float predictionTolerance;
        // The same question for rotation, in radians. Independent of the position tolerance because
        // the two diverge independently: a sphere slipping on contact, or airborne with no rolling
        // contact to couple them, drifts in spin while staying put.
        static float predictionAngleTolerance;
        // Input goes out every tick. It is a handful of bits, and it is what every peer's
        // simulation of every other player depends on.
        static constexpr Uint64 InputSendInterval = 1;
        // Past this an input is too old to stand in for the present, so the body stops being driven
        // rather than rolling away on its own. This is a "that peer is gone" threshold, not a
        // latency one: at 300ms a remote player's newest input is routinely 20 ticks behind the
        // tick we are simulating, and cutting them off there would stall every remote ball.
        static constexpr Uint64 MaxInputStaleTicks = 60;
        // How long a correction marker stays on screen.
        static constexpr Uint64 CorrectionMarkerTicks = 45;
        // Corrections smaller than this are not worth drawing; they are the normal quantization noise.
        static constexpr float CorrectionMarkerMinimum = 0.01f;
        // Divergence at which the ghost line is fully red rather than green.
        static constexpr float DivergenceFullScale = 0.5f;

        // Ticks a client aims to keep its input ahead of the host, so it lands before it is needed.
        static constexpr int32_t InputLeadTarget = 2;
        // Dead band above the target, so the clock is not adjusted on every packet.
        static constexpr int32_t InputLeadSlack = 3;
        // Where the client's clock starts before steering takes over.
        static constexpr Uint64 InitialInputLead = 6;
        // Lead is signed and varints are not, so it travels shifted by this and is shifted back.
        static constexpr int32_t InputLeadBias = 128;
        // Input packets to wait between clock adjustments, long enough for the previous one to have
        // made the round trip and shown up in the reported lead.
        static constexpr int ClockAdjustCooldownPackets = 30;
        // Above this the lead is corrected in one move rather than a tick at a time.
        static constexpr int32_t InputLeadJumpThreshold = 4;
        // Received packets older than this are dropped; only the jitter hold needs them.
        static constexpr size_t MaxSnapshotHistory = 32;
        // Bytes of snapshot packet, header included, under SoftPacketBytes so it is one datagram.
        static constexpr int MaxStatePayloadBytes = 1000;
        // Times an entity repeats its final state after it stops changing.
        static constexpr uint8_t RestRedundancy = 3;
        // Ceilings on how far ahead of the host the clock may sit and how long a packet may be held.
        // Both bound how stale the input driving a body can get, so neither may approach
        // InputHistory::Length.
        static constexpr Uint64 MaxInputLeadTicks = 24;
        static constexpr Uint64 MaxJitterTicks = 24;
        // Ticks we are willing to replay in one go. Past this the client is far enough behind that
        // snapping and carrying on costs less than the catch-up. It has to exceed the worst depth a
        // supported connection produces, which is the lead plus the trip down plus the jitter hold
        // plus a send interval: about 30 ticks at 300ms. It must also stay under InputHistory::Length,
        // since a replay is only as good as the input it can still look up.
        static constexpr Uint64 MaxRollbackTicks = 48;
        // A replay needs a saved contact set and the input to feed it for every tick it covers, so
        // the rollback window can never outgrow either ring.
        static_assert(MaxRollbackTicks < physics::PhysicsManager::MaxSavedContacts,
                      "rollback window exceeds the saved contact ring");

    private:
        [[nodiscard]] QuantizedState captureState(EntityId id, const TransformComponent& transform);
        void captureHost(Uint64 tick);
        void sendStateTo(uint32_t connection, ClientSyncState& client, Uint64 tick);
        // How much this client wants this entity right now, before the accumulator adds it up.
        [[nodiscard]] float basePriority(const EntitySnapshot& entity, const ClientSyncState& client,
                                         const glm::vec3& clientFocus) const;
        // Host to clients, every tick: everyone else's recent input.
        void sendPeerInput();
        void onPeerInputMessage(int size);
        void onSnapshotMessage(int size);
        // Client to host: the state ack plus this peer's recent input frames.
        void onClientInputMessage(uint32_t connection, int size);
        void sendClientInput();
        // Client to host on joining: where other clients can reach us.
        void sendPeerHello();
        void onPeerHelloMessage(uint32_t connection, int size);
        // Host to clients whenever the membership changes, so each one can dial the others. Input
        // relayed through the host arrives a hop later than input sent straight across, and that hop
        // is the largest part of what a client gets wrong about another client.
        void sendPeerRoster();
        void onPeerRosterMessage(int size);
        // Redundant window of one peer's input, shared by the client's upload and the host's
        // rebroadcast of everyone else's.
        template<typename Stream>
        void serializeInputWindow(Stream& stream, uint32_t peerId, Uint64 newestTick);
        void steerClock(int32_t reportedLead);
        [[nodiscard]] Uint64 leadFromPing();
        [[nodiscard]] Uint64 oneWayTicks() const;
        void steerInputDelay();
        void onWelcomeMessage(int size);
        void sendWelcome(uint32_t connection, uint32_t peerId);
        // Drains the jitter buffer: snapshots held long enough are applied, oldest first.
        void applyBufferedState();
        // Snaps one entity onto an authoritative state and records how far out the prediction was.
        void applyState(EntityId id, const EntitySnapshot& entity, Uint64 tick);
        // Every replicated body at the end of this tick, for the check below.
        void recordPredictions(Uint64 tick);
        // Solver state for this tick, so a replay can rewind to it.
        void saveContacts(Uint64 tick);
        // True when every body in the packet was already within predictionTolerance of where the
        // host puts it, so there is nothing a replay could improve.
        [[nodiscard]] bool predictionAgrees(const WorldSnapshot& snapshot) const;
        void applyStates(const WorldSnapshot& snapshot);
        // Restores the world to the tick the host is describing and replays every step since from
        // buffered input, so the whole simulation ends up back at the present. force skips the
        // agreement check, for a replay driven by input rather than by state.
        void rollbackAndReplay(const WorldSnapshot& snapshot, bool force = false);
        // A peer's real input arrived for a tick we already simulated with a guess, and the two
        // differ. Replays from the last authoritative state we have so the press lands where it
        // belongs instead of being lost for having arrived a few ticks late.
        void replayLateInput();
        // The local entity for a netId, or NULL_ENTITY if this peer has no copy of it.
        EntityId resolveEntity(uint32_t netId, uint32_t hostEntityId);
        void resetReceivedState();

        [[nodiscard]] const WorldSnapshot* findReceived(Uint64 tick) const;
        void sampleBandwidth();
        void writeTrace(Uint64 tick);
        void sampleCorrection();
        // Closes off one netgraph column at the end of a tick and starts the next one.
        void sampleNetGraph();
        // Runtime sliders for the rates and the smoothing, drawn inside debugUI.
        void drawTuning();
        // Ticks of our input the host should hold beyond the tick it is simulating.
        [[nodiscard]] int32_t inputLeadTarget() const;
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
        uint32_t nextPeerId = HostPeerId + 1;
        std::unordered_map<uint32_t, InputHistory> inputByPeer;
        // The host's current view of every replicated entity, rebuilt each capture and shared by
        // every connection. Only the selection of what to send is per client.
        std::vector<EntitySnapshot> latestStates;
        std::unordered_map<uint32_t, ClientSyncState> clients;

        uint32_t localPeerId = 0;
        // Which connection is the host's, so a client can tell it apart from the other clients that
        // dial in for input. 0 until we have one.
        uint32_t hostConnection = 0;
        // Peers we have already dialled, so a repeated roster does not open a second connection.
        std::vector<uint64_t> dialledPeers;

        // The jitter buffer. Snapshots wait here until the local tick reaches their tick plus the
        // measured hold, sorted so they are released in the order the host produced them.
        std::deque<WorldSnapshot> receivedHistory;
        JitterEstimator jitter;
        std::unordered_map<uint32_t, EntityId> entityByNetId;
        Uint64 newestReceivedTick = 0;
        Uint64 appliedTick = 0;
        int32_t measuredInputLead = 0;
        int clockAdjustments = 0;
        int clockCooldown = 0;
        int inputDelayCooldown = 0;

        // Newest state the host sent for each entity, kept so a ghost stays put instead of blinking
        // out whenever that entity misses a packet's budget.
        std::unordered_map<uint32_t, ReceivedState> lastReceivedByNetId;
        std::vector<CorrectionMarker> correctionMarkers;
        std::unordered_map<uint32_t, PredictedPoses> predictedByNetId;
        // Ticks whose contact state is saved and still valid to replay from. Invalidated whenever
        // the clock is renumbered, since the slots are indexed by a tick that no longer means
        // anything.
        Uint64 oldestContactTick = 0;
        Uint64 newestContactTick = 0;
        bool contactsValid = false;

        // The newest snapshot we applied, kept whole so a late input can be replayed from an
        // authoritative state rather than from wherever the simulation happens to be.
        WorldSnapshot lastApplied;
        bool hasLastApplied = false;
        int lateInputReplays = 0;

        ThrottledWarning budgetWarning;
        ThrottledWarning worldBoundWarning;
        ThrottledWarning velocityRangeWarning;
        ThrottledWarning sceneMismatchWarning;
        ThrottledWarning replaySkippedWarning;
        ThrottledWarning connectionCapWarning;

        // Packets that needed no resimulation because the prediction already matched. Climbing
        // steadily is the whole system working: the client guessed right and paid nothing.
        int predictionsKept = 0;
        // Packets the question was asked of, so the above can be read as a share rather than a count.
        int predictionsChecked = 0;
        // What a resimulation costs. Depth times rate is the extra physics being run, so replayed
        // steps per second against the 60 a tick normally costs is the number to watch.
        int lastResimDepth = 0;
        int deepestResim = 0;
        int resimStepsThisWindow = 0;
        float resimStepsPerSecond = 0.0f;
        // Corrections applied without a replay: too far behind to catch up, or a packet the budget
        // left incomplete. Either way the world is left a rollback's worth of time in the past.
        int snappedTooDeep = 0;
        int snappedIncomplete = 0;

        int lastSnapshotEntities = 0;
        int lastReceivedChanged = 0;
        // How far out each prediction turned out to be: the number that says whether this is working.
        CorrectionMeter correction;
        std::array<float, 120> correctionHistory{};
        int correctionHistoryIndex = 0;
        Uint64 correctionWindowStartMs = 0;
        BandwidthMeter sentBytes;
        BandwidthMeter receivedBytes;

        NetGraph netGraph;
        // Filled as the tick runs, drained into one column at the end of it.
        float tickCorrection = 0.0f;
        int tickResimDepth = 0;
        uint8_t tickFlags = 0;
        // Starvation is a running total per peer, so a column shows the change rather than the sum.
        int starvedTotal = 0;
        // Snapshots the host sent that never arrived, counted from the gaps in the tick numbers
        // rather than from the backend, so it measures what the simulation actually went without.
        int lostSnapshots = 0;
        Uint64 lastArrivedTick = 0;

        std::ofstream traceFile;
    };
} // ytail::net

#endif //YELLOWTAIL_REPLICATIONMANAGER_H
