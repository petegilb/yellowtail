//
// Created by Peter Gilbert on 8/11/26.
//

#ifndef YELLOWTAIL_NETSIMSETTINGS_H
#define YELLOWTAIL_NETSIMSETTINGS_H

#include <string>
#include <vector>

namespace ytail {
    // Simulated network conditions for local testing. Lives outside engine/net/ so the editor's test
    // launcher can build these without networking headers; only the spawned game applies them.
    struct NetSimSettings {
        // Added to outbound packets in every instance, so a round trip picks it up twice.
        int lagMs = 0;
        float jitterMs = 0.0f;
        // Odds a given packet gets a random jitter value at all.
        float jitterPct = 100.0f;
        float lossPct = 0.0f;
        // Packets held back by reorderTimeMs, so later ones overtake them.
        float reorderPct = 0.0f;
        int reorderTimeMs = 15;

        [[nodiscard]] bool isActive() const {
            return lagMs > 0 || jitterMs > 0.0f || lossPct > 0.0f || reorderPct > 0.0f;
        }
    };

    [[nodiscard]] std::vector<std::string> netSimToArgs(const NetSimSettings& settings);
    [[nodiscard]] NetSimSettings netSimFromArgs(int argc, char* argv[]);
} // ytail

#endif //YELLOWTAIL_NETSIMSETTINGS_H
