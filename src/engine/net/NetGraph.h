//
// Created by Peter Gilbert on 8/14/26.
//

#ifndef YELLOWTAIL_NETGRAPH_H
#define YELLOWTAIL_NETGRAPH_H

#include <array>
#include <cstddef>
#include <cstdint>

namespace ytail::net {
    // What happened on one tick, drawn as one column. Bits rather than bools because a tick can be
    // several of these at once: a packet can arrive, be applied, and be too deep to replay.
    enum NetGraphFlag : uint8_t {
        NetGraphApplied = 1 << 0,  // a snapshot was applied on this tick
        NetGraphReplayed = 1 << 1, // and the world was rewound and replayed to catch back up
        NetGraphSnapped = 1 << 2,  // and it could not be, so the world stayed in the past
        NetGraphLost = 1 << 3,     // a snapshot the host sent never arrived, seen as a gap in ticks
        NetGraphStarved = 1 << 4,  // a ball was driven with no input at all
    };

    // One fixed tick of the graph. A column is a tick rather than a frame: everything it reports is
    // decided on the tick timeline, and a frame spanning two ticks would smear two answers into one.
    struct NetGraphSample {
        // Metres between where we predicted a body and where the host put it, worst of the bodies
        // corrected this tick.
        float correction = 0.0f;
        uint16_t bytesSent = 0;
        uint16_t bytesReceived = 0;
        // Ticks replayed to catch back up after a correction.
        uint8_t resimDepth = 0;
        uint8_t flags = 0;
    };

    // Everything printed under the graph, refreshed from live state each frame.
    struct NetGraphStats {
        bool hosting = false;
        int connections = 0;
        // Includes any simulated lag, which is why simulated is reported next to it.
        int pingMs = -1;
        bool simulated = false;
        // Share of packets the connection never delivered, from the backend's quality estimate.
        // Negative while it has no estimate yet.
        float lossPct = -1.0f;
        float inKbPerSec = 0.0f;
        float outKbPerSec = 0.0f;

        // Client only, all in milliseconds unless the name says ticks.
        float inputLagMs = 0.0f;
        int inputDelayTicks = 0;
        int inputLeadTicks = 0;
        int inputLeadTarget = 0;
        float jitterHoldMs = 0.0f;
        int jitterHoldTicks = 0;
        // How far in the past the newest state we snapped to is.
        float stateAgeMs = 0.0f;
        float correctionMean = 0.0f;
        float correctionPeak = 0.0f;
        float toleranceMetres = 0.0f;
        float resimStepsPerSecond = 0.0f;
        int deepestResim = 0;
        // Share of packets that needed no replay because the prediction already matched.
        float keptPct = -1.0f;
        int lostSnapshots = 0;
        int starvedInputs = 0;

        // Host only.
        int replicatedEntities = 0;
    };

    // Source's net_graph, in ImGui: a strip of per-tick columns for how the connection is behaving,
    // with the numbers under it. Drawn as an overlay that takes no input, so it can sit over the game
    // without being in the way of anything.
    class NetGraph {
    public:
        // 200 ticks is a bit over three seconds at 60Hz: long enough that a hitch is still on screen
        // when you look up from what caused it.
        static constexpr size_t Length = 200;

        void push(const NetGraphSample& sample);
        void draw(const NetGraphStats& stats);
        void clear();

        bool visible = false;
        // 0 top-left, 1 top-right, 2 bottom-left, 3 bottom-right.
        int corner = 1;
        // Graph only, for when the numbers are not what is being watched.
        bool showStats = true;

    private:
        [[nodiscard]] const NetGraphSample& at(size_t column) const;
        // Eased towards the window's peak so the bars stay readable without the scale jumping under
        // them every time one tall column falls off the end.
        void updateScales();

        std::array<NetGraphSample, Length> samples{};
        size_t next = 0;
        float correctionScale = 0.1f;
        float byteScale = 256.0f;
    };
} // ytail::net

#endif //YELLOWTAIL_NETGRAPH_H
