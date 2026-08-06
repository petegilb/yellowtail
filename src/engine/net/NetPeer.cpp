//
// Created by Peter Gilbert on 8/6/26.
//

#include "NetPeer.h"

#include <algorithm>

#include <SDL3/SDL.h>

#include "steam/steam_api.h"
#include "steam/isteamnetworkingsockets.h"
#include "steam/isteamnetworkingutils.h"

#include "imgui.h"

#include "engine/net/INetworkEventHandler.h"

namespace ytail {
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

    bool NetPeer::startHost() {
        if (sockets == nullptr) return false;

        pollGroup = sockets->CreatePollGroup();
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

        pollGroup = sockets->CreatePollGroup();

        SteamNetworkingIdentity identity;
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

        hosting = false;
        active = true;
        SDL_Log("Connecting to host %llu over the relay.", static_cast<unsigned long long>(hostSteamId));
        return true;
    }

    bool NetPeer::startHostIP(const uint16_t port) {
        if (sockets == nullptr) return false;

        pollGroup = sockets->CreatePollGroup();

        SteamNetworkingIPAddr address;
        address.Clear();
        address.SetIPv4(0, port);

        // Loopback / LAN peers have no Steam cert, so allow unauthenticated IP connections.
        SteamNetworkingConfigValue_t allowWithoutAuth;
        allowWithoutAuth.SetInt32(k_ESteamNetworkingConfig_IP_AllowWithoutAuth, 1);

        listenSocket = sockets->CreateListenSocketIP(address, 1, &allowWithoutAuth);
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

        pollGroup = sockets->CreatePollGroup();

        SteamNetworkingIPAddr address;
        address.Clear();
        address.SetIPv4(0x7f000001, port); // 127.0.0.1

        SteamNetworkingConfigValue_t allowWithoutAuth;
        allowWithoutAuth.SetInt32(k_ESteamNetworkingConfig_IP_AllowWithoutAuth, 1);

        const HSteamNetConnection connection = sockets->ConnectByIPAddress(address, 1, &allowWithoutAuth);
        if (connection == k_HSteamNetConnection_Invalid) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ConnectByIPAddress to 127.0.0.1:%u failed.", port);
            return false;
        }
        sockets->SetConnectionPollGroup(connection, pollGroup);
        connections.push_back(connection);

        hosting = false;
        active = true;
        SDL_Log("Connecting to local host 127.0.0.1:%u.", port);
        return true;
    }

    void NetPeer::onStatusChanged(const uint32_t connection, const NetConnState state,
                                  const bool inboundFromListen, const char* endDebug) {
        switch (state) {
            case NetConnState::Connecting:
                // Only an inbound request on our listen socket needs accepting; our own outbound
                // connect also passes through Connecting and is left alone.
                if (inboundFromListen) {
                    if (sockets->AcceptConnection(connection) != k_EResultOK) {
                        sockets->CloseConnection(connection, 0, nullptr, false);
                        return;
                    }
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

        SteamNetworkingMessage_t* messages[16];
        const int received = sockets->ReceiveMessagesOnPollGroup(pollGroup, messages, 16);
        for (int i = 0; i < received; ++i) {
            SteamNetworkingMessage_t* message = messages[i];
            if (handler != nullptr) {
                handler->onMessage(message->m_conn, message->m_pData, message->m_cbSize);
            }
            message->Release();
        }
    }

    void NetPeer::sendReliable(const uint32_t connection, const void* data, const uint32_t size) {
        if (sockets == nullptr) return;
        sockets->SendMessageToConnection(
            connection, data, size, k_nSteamNetworkingSend_Reliable, nullptr);
    }

    void NetPeer::drawDebugUI() {
        if (!ImGui::CollapsingHeader("Multiplayer")) return;

        if (!active) {
            ImGui::TextUnformatted("Offline");
            return;
        }

        ImGui::Text("Mode: %s", hosting ? "Host" : "Client");
        ImGui::Text("Connections: %d", static_cast<int>(connections.size()));
        for (const uint32_t connection : connections) {
            char identity[128] = "?";
            char address[64] = "?";
            SteamNetConnectionInfo_t info;
            if (sockets->GetConnectionInfo(connection, &info)) {
                info.m_identityRemote.ToString(identity, sizeof(identity));
                info.m_addrRemote.ToString(address, sizeof(address), true);
            }
            ImGui::BulletText("%s  (%s)", identity, address);

            ImGui::Indent();
            SteamNetConnectionRealTimeStatus_t status;
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
    }

    void NetPeer::shutdown() {
        if (!active) return;

        for (const uint32_t connection : connections) {
            sockets->CloseConnection(connection, 0, "shutting down", false);
        }
        connections.clear();

        if (pollGroup != k_HSteamNetPollGroup_Invalid) {
            sockets->DestroyPollGroup(pollGroup);
            pollGroup = 0;
        }
        if (hosting && listenSocket != k_HSteamListenSocket_Invalid) {
            sockets->CloseListenSocket(listenSocket);
            listenSocket = 0;
        }
        if (activePeer == this) activePeer = nullptr;
        active = false;
        hosting = false;
    }
} // ytail
