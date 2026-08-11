//
// Created by Peter Gilbert on 8/6/26.
//

#ifndef YELLOWTAIL_NETPEER_H
#define YELLOWTAIL_NETPEER_H

#include <cstdint>
#include <vector>

#include "engine/NetSimSettings.h"

// The networking interfaces are provided by the app's backend (Steam or GameNetworkingSockets)
class ISteamNetworkingSockets;
class ISteamNetworkingUtils;

namespace ytail::net {
    class INetworkEventHandler;

    enum class NetConnState { Connecting, Connected, Closed };

    // First byte of every payload, so a receiver can route without guessing. Never renumber these.
    // Values from GameFirst up are the game's to define; the engine forwards them uninterpreted.
    enum class NetMessageType : uint8_t {
        Snapshot = 1,
        ClientState = 2,
        Welcome = 3,
        RequestOwnership = 4,
        GameFirst = 128,
    };

    // Connection layer over ISteamNetworkingSockets. One instance either hosts a listen server or
    // connects to one, and forwards connection and message events to an INetworkEventHandler. The
    // interface pointers come from the app via bind(), so the engine never calls the backend's global
    // accessors and stays swappable between Steam and GameNetworkingSockets.
    class NetPeer {
    public:
        // Supply the backend interfaces and the event sink. Call once after the backend is initialized.
        void bind(ISteamNetworkingSockets* inSockets, ISteamNetworkingUtils* inUtils,
                  INetworkEventHandler* inHandler);

        // Start listening for peers over the relay. False if not bound or sockets are unavailable.
        bool startHost();
        // Connect to a host by its SteamID (raw 64-bit). False on immediate failure.
        bool connectTo(uint64_t hostSteamId);

        // Direct-IP variants for local testing (loopback / LAN): no relay, no second Steam account.
        bool startHostIP(uint16_t port);
        bool connectToIP(uint16_t port);

        // Fake lag/jitter/loss for local testing. Global to the process and applied to sends, so a
        // round trip between two instances sees each one's lag once. Call any time after bind().
        void applyNetSim(const NetSimSettings& settings);
        [[nodiscard]] const NetSimSettings& getNetSim() const { return netSim; }

        // Receive and dispatch pending messages. Call once per frame after backend callbacks run.
        void poll();

        // Close all connections and release sockets.
        void shutdown();

        // Reliable for events that can't be reconstructed from state; unreliable for snapshots.
        void sendReliable(uint32_t connection, const void* data, uint32_t size);
        void sendUnreliable(uint32_t connection, const void* data, uint32_t size);
        void broadcastReliable(const void* data, uint32_t size);
        void broadcastUnreliable(const void* data, uint32_t size);
        // Push out anything Nagle is still holding. Call after a tick's sends are queued.
        void flush();

        [[nodiscard]] const std::vector<uint32_t>& getConnections() const { return connections; }

        [[nodiscard]] bool isActive() const { return active; }
        [[nodiscard]] bool isHosting() const { return hosting; }

        // Render a "Multiplayer" section (mode, connections, per-connection ping) into the current
        // ImGui window. Call from the app's debug UI.
        void drawDebugUI();

        // Invoked by the connection-status callback (see the trampoline in the .cpp). Public only so
        // that file-static callback can reach it; not part of the normal API.
        void onStatusChanged(uint32_t connection, NetConnState state, bool inboundFromListen,
                             const char* endDebug);

    private:
        void send(uint32_t connection, const void* data, uint32_t size, int sendFlags);
        void removeConnection(uint32_t connection);

        ISteamNetworkingSockets* sockets = nullptr;
        ISteamNetworkingUtils* utils = nullptr;
        INetworkEventHandler* handler = nullptr;

        // Steam handle values. HSteamNetConnection / HSteamListenSocket / HSteamNetPollGroup are all
        // uint32, and 0 is the invalid sentinel for each.
        NetSimSettings netSim;

        std::vector<uint32_t> connections;
        uint32_t listenSocket = 0;
        uint32_t pollGroup = 0;
        bool hosting = false;
        bool active = false;
    };
} // ytail::net

#endif //YELLOWTAIL_NETPEER_H
