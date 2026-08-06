//
// Created by Peter Gilbert on 8/6/26.
//

#ifndef YELLOWTAIL_NETPEER_H
#define YELLOWTAIL_NETPEER_H

#include <cstdint>
#include <vector>

// The networking interfaces are provided by the app's backend (Steam or GameNetworkingSockets)
class ISteamNetworkingSockets;
class ISteamNetworkingUtils;

namespace ytail {
    class INetworkEventHandler;

    enum class NetConnState { Connecting, Connected, Closed };

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

        // Receive and dispatch pending messages. Call once per frame after backend callbacks run.
        void poll();

        // Close all connections and release sockets.
        void shutdown();

        void sendReliable(uint32_t connection, const void* data, uint32_t size);

        [[nodiscard]] bool isActive() const { return active; }
        [[nodiscard]] bool isHosting() const { return hosting; }

        // Invoked by the connection-status callback (see the trampoline in the .cpp). Public only so
        // that file-static callback can reach it; not part of the normal API.
        void onStatusChanged(uint32_t connection, NetConnState state, bool inboundFromListen,
                             const char* endDebug);

    private:
        void removeConnection(uint32_t connection);

        ISteamNetworkingSockets* sockets = nullptr;
        ISteamNetworkingUtils* utils = nullptr;
        INetworkEventHandler* handler = nullptr;

        // Steam handle values. HSteamNetConnection / HSteamListenSocket / HSteamNetPollGroup are all
        // uint32, and 0 is the invalid sentinel for each.
        std::vector<uint32_t> connections;
        uint32_t listenSocket = 0;
        uint32_t pollGroup = 0;
        bool hosting = false;
        bool active = false;
    };
} // ytail

#endif //YELLOWTAIL_NETPEER_H
