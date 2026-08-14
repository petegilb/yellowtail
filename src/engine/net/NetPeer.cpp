//
// Created by Peter Gilbert on 8/6/26.
//

#include "NetPeer.h"

#include <algorithm>

#include <SDL3/SDL.h>

#include "steam/isteamnetworkingsockets.h"
#include "steam/isteamnetworkingutils.h"

#include "imgui.h"

#include "engine/net/INetworkEventHandler.h"

namespace ytail::net {
    // Peer links live on their own virtual port. Sharing the host's would make a client's outbound
    // connection to the host and an inbound link from another client indistinguishable, and Steam
    // does not merge those without SymmetricConnect: the client ends up connected twice, gets two
    // peer ids and two balls, and drives neither. The local transport is immune because every
    // instance already listens on its own IP port.
    constexpr int PeerVirtualPort = 1;

    // The backend's status callback is a plain function pointer, so it reaches the active peer through
    // this file-static pointer. There is one networking peer per process.
    static NetPeer* activePeer = nullptr;

    void NetPeer::bind(ISteamNetworkingSockets* inSockets, ISteamNetworkingUtils* inUtils,
                       INetworkEventHandler* inHandler) {
        sockets = inSockets;
        utils = inUtils;
        handler = inHandler;
        activePeer = this;
        utils->SetGlobalCallback_SteamNetConnectionStatusChanged(
            [](SteamNetConnectionStatusChangedCallback_t* info) {
                if (activePeer == nullptr) return;

                const bool inbound = info->m_info.m_hListenSocket != k_HSteamListenSocket_Invalid;
                switch (info->m_info.m_eState) {
                    case k_ESteamNetworkingConnectionState_Connecting:
                        activePeer->onStatusChanged(info->m_hConn, NetConnState::Connecting, inbound, nullptr);
                        break;
                    case k_ESteamNetworkingConnectionState_Connected:
                        activePeer->onStatusChanged(info->m_hConn, NetConnState::Connected, inbound, nullptr);
                        break;
                    case k_ESteamNetworkingConnectionState_ClosedByPeer:
                    case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
                        activePeer->onStatusChanged(info->m_hConn, NetConnState::Closed, inbound,
                                                    info->m_info.m_szEndDebug);
                        break;
                    default:
                        break;
                }
            });
    }

    void NetPeer::applyNetSim(const NetSimSettings& settings) {
        if (utils == nullptr) return;
        netSim = settings;

        utils->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_FakePacketLag_Send, settings.lagMs);
        utils->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketJitter_Send_Avg, settings.jitterMs);
        // Cap the tail of the exponential distribution so a stray sample can't stall for seconds.
        utils->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketJitter_Send_Max, settings.jitterMs * 3.0f);
        utils->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketJitter_Send_Pct, settings.jitterPct);
        utils->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketLoss_Send, settings.lossPct);
        utils->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketReorder_Send, settings.reorderPct);
        utils->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_FakePacketReorder_Time, settings.reorderTimeMs);

        if (settings.isActive()) {
            SDL_Log("Simulating network conditions: %dms lag, %.0fms jitter (%.0f%%), %.1f%% loss, "
                    "%.1f%% reorder (+%dms).",
                    settings.lagMs, settings.jitterMs, settings.jitterPct, settings.lossPct,
                    settings.reorderPct, settings.reorderTimeMs);
        }
    }

    // Loopback and LAN peers have no Steam cert, so an IP connection has to be let through without
    // one. A relay connection never gets it: over Steam both ends have a real identity, and waiving
    // that is a deliberate act rather than something a peer helper should do on the way past.
    int NetPeer::transportOptions(SteamNetworkingConfigValue_t& storage) const {
        if (!localTransport) return 0;
        storage.SetInt32(k_ESteamNetworkingConfig_IP_AllowWithoutAuth, 1);
        return 1;
    }

    uint64_t NetPeer::getRemoteId(const uint32_t connection) const {
        if (sockets == nullptr) return 0;
        SteamNetConnectionInfo_t info{};
        if (!sockets->GetConnectionInfo(connection, &info)) return 0;
        return info.m_identityRemote.GetSteamID64();
    }

    void NetPeer::ensurePollGroup() {
        if (pollGroup == k_HSteamNetPollGroup_Invalid) pollGroup = sockets->CreatePollGroup();
    }

    bool NetPeer::startHost() {
        if (sockets == nullptr) return false;
        if (active) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Already in a session; ignoring %s.", "a second host");
            return false;
        }

        ensurePollGroup();
        listenSocket = sockets->CreateListenSocketP2P(0, 0, nullptr);
        if (listenSocket == k_HSteamListenSocket_Invalid) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create a P2P listen socket.");
            return false;
        }

        hosting = true;
        active = true;
        SDL_Log("Hosting a listen server over the relay.");
        return true;
    }

    bool NetPeer::connectTo(const uint64_t hostSteamId) {
        if (sockets == nullptr) return false;
        if (active) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Already in a session; ignoring %s.", "a second connect");
            return false;
        }

        ensurePollGroup();

        SteamNetworkingIdentity identity{};
        identity.Clear();
        identity.SetSteamID64(hostSteamId);

        const HSteamNetConnection connection = sockets->ConnectP2P(identity, 0, 0, nullptr);
        if (connection == k_HSteamNetConnection_Invalid) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ConnectP2P to %llu failed.",
                         static_cast<unsigned long long>(hostSteamId));
            return false;
        }
        sockets->SetConnectionPollGroup(connection, pollGroup);
        connections.push_back(connection);
        hostConnection = connection;

        hosting = false;
        active = true;
        SDL_Log("Connecting to host %llu over the relay.", static_cast<unsigned long long>(hostSteamId));
        return true;
    }

    bool NetPeer::startHostIP(const uint16_t port) {
        if (sockets == nullptr) return false;
        if (active) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Already in a session; ignoring %s.", "a second host");
            return false;
        }

        localTransport = true;
        ensurePollGroup();

        SteamNetworkingIPAddr address{};
        address.Clear();
        address.SetIPv4(0, port);

        SteamNetworkingConfigValue_t options{};
        const int optionCount = transportOptions(options);
        listenSocket = sockets->CreateListenSocketIP(address, optionCount, optionCount > 0 ? &options : nullptr);
        if (listenSocket == k_HSteamListenSocket_Invalid) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "Failed to create an IP listen socket on port %u.", port);
            return false;
        }

        hosting = true;
        active = true;
        SDL_Log("Hosting a local listen server on port %u.", port);
        return true;
    }

    bool NetPeer::connectToIP(const uint16_t port) {
        if (sockets == nullptr) return false;
        if (active) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Already in a session; ignoring %s.", "a second connect");
            return false;
        }

        localTransport = true;
        ensurePollGroup();

        SteamNetworkingIPAddr address{};
        address.Clear();
        address.SetIPv4(0x7f000001, port); // 127.0.0.1

        SteamNetworkingConfigValue_t options{};
        const int optionCount = transportOptions(options);
        const HSteamNetConnection connection = sockets->ConnectByIPAddress(address, optionCount, optionCount > 0 ? &options : nullptr);
        if (connection == k_HSteamNetConnection_Invalid) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ConnectByIPAddress to 127.0.0.1:%u failed.", port);
            return false;
        }
        sockets->SetConnectionPollGroup(connection, pollGroup);
        connections.push_back(connection);
        hostConnection = connection;

        hosting = false;
        active = true;
        SDL_Log("Connecting to local host 127.0.0.1:%u.", port);
        return true;
    }

    // Deliberately leaves hosting alone: this peer accepts connections without becoming the
    // authority for anything.
    bool NetPeer::listenForPeers() {
        if (sockets == nullptr || listenSocket != k_HSteamListenSocket_Invalid) return false;
        ensurePollGroup();

        SteamNetworkingConfigValue_t options{};
        const int optionCount = transportOptions(options);
        if (localTransport) {
            SteamNetworkingIPAddr address{};
            address.Clear();
            address.SetIPv4(0, static_cast<uint16_t>(peerAddress));
            listenSocket = sockets->CreateListenSocketIP(address, optionCount, optionCount > 0 ? &options : nullptr);
        } else {
            listenSocket = sockets->CreateListenSocketP2P(PeerVirtualPort, optionCount,
                                                          optionCount > 0 ? &options : nullptr);
        }

        if (listenSocket == k_HSteamListenSocket_Invalid) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to open a peer listen socket.");
            return false;
        }
        SDL_Log("Listening for peer input connections on %llu.",
                static_cast<unsigned long long>(peerAddress));
        return true;
    }

    bool NetPeer::connectToPeer(const uint64_t address) {
        if (sockets == nullptr || address == 0 || address == peerAddress) return false;
        ensurePollGroup();

        SteamNetworkingConfigValue_t options{};
        const int optionCount = transportOptions(options);
        uint32_t connection = k_HSteamNetConnection_Invalid;
        if (localTransport) {
            SteamNetworkingIPAddr target{};
            target.Clear();
            target.SetIPv4(0x7f000001, static_cast<uint16_t>(address));
            connection = sockets->ConnectByIPAddress(target, optionCount, optionCount > 0 ? &options : nullptr);
        } else {
            SteamNetworkingIdentity identity{};
            identity.Clear();
            identity.SetSteamID64(address);
            connection = sockets->ConnectP2P(identity, PeerVirtualPort, optionCount,
                                             optionCount > 0 ? &options : nullptr);
        }

        if (connection == k_HSteamNetConnection_Invalid) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to dial peer %llu.",
                         static_cast<unsigned long long>(address));
            return false;
        }
        sockets->SetConnectionPollGroup(connection, pollGroup);
        connections.push_back(connection);
        SDL_Log("Dialling peer %llu for input.", static_cast<unsigned long long>(address));
        return true;
    }

    void NetPeer::onStatusChanged(const uint32_t connection, const NetConnState state,
                                  const bool inboundFromListen, const char* endDebug) {
        switch (state) {
            case NetConnState::Connecting:
                // Only an inbound request on our listen socket needs accepting; our own outbound
                // connect also passes through Connecting and is left alone.
                if (inboundFromListen) {
                    const uint64_t remote = getRemoteId(connection);
                    if (!localTransport && remote != 0) {
                        for (const auto& [existing, id] : remoteIds) {
                            if (id != remote) continue;
                            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                                        "Rejecting a second connection from %llu; it is already "
                                        "connected on %u.",
                                        static_cast<unsigned long long>(remote), existing);
                            sockets->CloseConnection(connection, 0, "already connected", false);
                            return;
                        }
                    }
                    if (sockets->AcceptConnection(connection) != k_EResultOK) {
                        sockets->CloseConnection(connection, 0, nullptr, false);
                        return;
                    }
                    remoteIds[connection] = remote;
                    sockets->SetConnectionPollGroup(connection, pollGroup);
                    connections.push_back(connection);
                }
                break;
            case NetConnState::Connected:
                if (handler != nullptr) handler->onConnected(connection);
                break;
            case NetConnState::Closed:
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Connection closed: %s",
                            endDebug != nullptr ? endDebug : "");
                if (handler != nullptr) handler->onDisconnected(connection);
                sockets->CloseConnection(connection, 0, nullptr, false);
                removeConnection(connection);
                break;
        }
    }

    void NetPeer::poll() {
        if (!active) return;

        // Drained, not capped: a hitch runs several fixed steps with no polling, and a partial
        // drain leaves the backlog to compound over the following frames.
        constexpr int MaxMessagesPerBatch = 32;
        SteamNetworkingMessage_t* messages[MaxMessagesPerBatch];
        int received = MaxMessagesPerBatch;
        while (received == MaxMessagesPerBatch) {
            received = sockets->ReceiveMessagesOnPollGroup(pollGroup, messages, MaxMessagesPerBatch);
            for (int i = 0; i < received; ++i) {
                SteamNetworkingMessage_t* message = messages[i];
                if (handler != nullptr) {
                    handler->onMessage(message->m_conn, message->m_pData, message->m_cbSize);
                }
                message->Release();
            }
        }
    }

    void NetPeer::send(const uint32_t connection, const void* data, const uint32_t size, const int sendFlags) {
        if (sockets == nullptr) return;
        const EResult result =
            sockets->SendMessageToConnection(connection, data, size, sendFlags, nullptr);
        if (result != k_EResultOK) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Send to connection %u failed (%d bytes, result %d)",
                        connection, size, static_cast<int>(result));
        }
    }

    void NetPeer::sendReliable(const uint32_t connection, const void* data, const uint32_t size) {
        send(connection, data, size, k_nSteamNetworkingSend_Reliable);
    }

    void NetPeer::sendUnreliable(const uint32_t connection, const void* data, const uint32_t size) {
        send(connection, data, size, k_nSteamNetworkingSend_Unreliable);
    }

    void NetPeer::broadcastUnreliable(const void* data, const uint32_t size) {
        for (const uint32_t connection : connections) sendUnreliable(connection, data, size);
    }

    void NetPeer::broadcastReliable(const void* data, const uint32_t size) {
        for (const uint32_t connection : connections) sendReliable(connection, data, size);
    }

    int NetPeer::getPingMs(const uint32_t connection) const {
        if (sockets == nullptr || connection == 0) return -1;
        SteamNetConnectionRealTimeStatus_t status{};
        if (sockets->GetConnectionRealTimeStatus(connection, &status, 0, nullptr) != k_EResultOK) return -1;
        return status.m_nPing;
    }

    bool NetPeer::getStats(const uint32_t connection, NetConnectionStats& stats) const {
        if (sockets == nullptr || connection == 0) return false;
        SteamNetConnectionRealTimeStatus_t status{};
        if (sockets->GetConnectionRealTimeStatus(connection, &status, 0, nullptr) != k_EResultOK) return false;

        stats.pingMs = status.m_nPing;
        stats.qualityLocal = status.m_flConnectionQualityLocal;
        stats.qualityRemote = status.m_flConnectionQualityRemote;
        stats.inBytesPerSec = status.m_flInBytesPerSec;
        stats.outBytesPerSec = status.m_flOutBytesPerSec;
        stats.inPacketsPerSec = status.m_flInPacketsPerSec;
        stats.outPacketsPerSec = status.m_flOutPacketsPerSec;
        return true;
    }

    // Nagle holds a partly-filled packet briefly so following messages can share it. That grouping
    // is worth having, but the last message of a tick has nothing to wait for, so flush once the
    // tick's sends are queued.
    void NetPeer::flush() {
        if (sockets == nullptr) return;
        for (const uint32_t connection : connections) sockets->FlushMessagesOnConnection(connection);
    }

    void NetPeer::drawDebugUI() {
        if (!ImGui::CollapsingHeader("Multiplayer")) return;

        if (!active) {
            ImGui::TextUnformatted("Offline");
            return;
        }

        ImGui::Text("Mode: %s", hosting ? "Host" : "Client");
        ImGui::Text("Connections: %d", static_cast<int>(connections.size()));
        // Ping here includes the fake lag, so say when it's being simulated.
        if (netSim.isActive()) {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f),
                               "Simulated: %dms lag, %.0fms jitter, %.1f%% loss (outbound)",
                               netSim.lagMs, netSim.jitterMs, netSim.lossPct);
        }
        for (const uint32_t connection : connections) {
            char identity[128] = "?";
            char address[64] = "?";
            SteamNetConnectionInfo_t info{};
            if (sockets->GetConnectionInfo(connection, &info)) {
                info.m_identityRemote.ToString(identity, sizeof(identity));
                info.m_addrRemote.ToString(address, sizeof(address), true);
            }
            ImGui::BulletText("%s  (%s)", identity, address);

            ImGui::Indent();
            SteamNetConnectionRealTimeStatus_t status{};
            if (sockets->GetConnectionRealTimeStatus(connection, &status, 0, nullptr) != k_EResultOK) {
                ImGui::Text("connecting...  [#%u]", connection);
            } else if (status.m_flConnectionQualityLocal < 0.0f) {
                // -1 means Steam has no packet-loss data yet (idle / freshly connected).
                ImGui::Text("ping %dms  quality n/a  [#%u]", status.m_nPing, connection);
            } else {
                ImGui::Text("ping %dms  quality %.0f%%  [#%u]", status.m_nPing,
                            status.m_flConnectionQualityLocal * 100.0f, connection);
            }
            ImGui::Unindent();
        }
    }

    void NetPeer::removeConnection(const uint32_t connection) {
        connections.erase(std::remove(connections.begin(), connections.end(), connection),
                          connections.end());
        remoteIds.erase(connection);
        if (hostConnection == connection) hostConnection = 0;
    }

    void NetPeer::shutdown() {
        if (!active) return;

        for (const uint32_t connection : connections) {
            sockets->CloseConnection(connection, 0, "shutting down", true);
        }
        connections.clear();
        remoteIds.clear();
        hostConnection = 0;

        if (pollGroup != k_HSteamNetPollGroup_Invalid) {
            sockets->DestroyPollGroup(pollGroup);
            pollGroup = 0;
        }
        // Not gated on hosting: a client listens for its peers too.
        if (listenSocket != k_HSteamListenSocket_Invalid) {
            sockets->CloseListenSocket(listenSocket);
            listenSocket = 0;
        }
        if (activePeer == this) activePeer = nullptr;
        active = false;
        hosting = false;
    }
} // ytail::net
