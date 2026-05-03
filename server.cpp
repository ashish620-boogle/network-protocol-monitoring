#include "common.h"

#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/select.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

using namespace Monitor;

struct DeviceState {
    std::string deviceId;
    std::string lastSource;
    uint32_t lastSequence = 0;
    DeviceStatus status = DeviceStatus::Ok;
    std::chrono::steady_clock::time_point lastSeen = std::chrono::steady_clock::now();
    bool online = false;
    bool tcpConnected = false;
    socket_t tcpClientFd = INVALID_SOCKET_FD;
};

struct TcpClientState {
    std::vector<char> pending;
    std::string peer;
    std::string deviceId;
};

static int makeNonBlocking(socket_t fd) {
    return setNonBlocking(fd);
}

static socket_t createListeningSocket(uint16_t port) {
    socket_t listenFd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd == INVALID_SOCKET_FD) throw std::runtime_error("Failed to create TCP socket");

    int opt = 1;
    setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(port);

    if (bind(listenFd, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)) < 0) {
        closeSocket(listenFd);
        throw std::runtime_error("Failed to bind TCP socket");
    }
    if (listen(listenFd, 8) < 0) {
        closeSocket(listenFd);
        throw std::runtime_error("TCP listen failed");
    }
    if (makeNonBlocking(listenFd) < 0) {
        closeSocket(listenFd);
        throw std::runtime_error("Failed to configure TCP socket non-blocking");
    }
    return listenFd;
}

static socket_t createUdpSocket(uint16_t port) {
    socket_t udpFd = socket(AF_INET, SOCK_DGRAM, 0);
    if (udpFd == INVALID_SOCKET_FD) throw std::runtime_error("Failed to create UDP socket");

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(udpFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        closeSocket(udpFd);
        throw std::runtime_error("Failed to bind UDP socket");
    }
    if (makeNonBlocking(udpFd) < 0) {
        closeSocket(udpFd);
        throw std::runtime_error("Failed to configure UDP socket non-blocking");
    }
    return udpFd;
}

static bool sendAll(socket_t fd, const void* data, size_t length) {
    const auto* bytes = reinterpret_cast<const char*>(data);
    size_t totalSent = 0;
    while (totalSent < length) {
        auto sent = send(fd, bytes + totalSent, static_cast<int>(length - totalSent), SOCKET_SEND_FLAGS);
        if (sent <= 0) {
            return false;
        }
        totalSent += static_cast<size_t>(sent);
    }
    return true;
}

static std::string formatSocketAddress(const sockaddr_in& addr) {
    char srcHost[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &addr.sin_addr, srcHost, sizeof(srcHost));
    return std::string(srcHost) + ":" + std::to_string(ntohs(addr.sin_port));
}

static void sendAck(socket_t fd, const HeartbeatPacket& request) {
    HeartbeatPacket ackPacket = request;
    ackPacket.header.type = PacketType::Ack;
    if (!sendAll(fd, &ackPacket, sizeof(ackPacket))) {
        logWarn("Failed to send TCP ACK on fd=" + std::to_string(static_cast<long long>(fd)));
    }
}

static void processHeartbeatPacket(const HeartbeatPacket& pkt,
                                   const std::string& source,
                                   std::map<std::string, DeviceState>& stateMap,
                                   bool tcpSource,
                                   socket_t clientFd = INVALID_SOCKET_FD) {
    std::string deviceId = deviceIdToString(pkt.deviceId);
    if (deviceId.empty()) {
        logWarn("Discarded packet with empty device id from " + source);
        return;
    }

    uint32_t seq = ntohl(pkt.sequence);
    uint64_t timestamp = networkToHost64(pkt.timestamp);
    DeviceStatus status = pkt.status;

    auto& state = stateMap[deviceId];
    if (state.online && seq < state.lastSequence) {
        logWarn("Out-of-order sequence for " + deviceId + ": got " + std::to_string(seq) +
                ", previous " + std::to_string(state.lastSequence));
    }

    bool wasOnline = state.online;
    state.deviceId = deviceId;
    state.lastSource = source;
    state.lastSequence = std::max(state.lastSequence, seq);
    state.status = status;
    state.lastSeen = std::chrono::steady_clock::now();
    state.online = true;
    if (tcpSource) {
        state.tcpConnected = true;
        state.tcpClientFd = clientFd;
    }

    if (!wasOnline) {
        logInfo("Device " + deviceId + " is online");
    }

    logInfo("Received heartbeat from " + deviceId + " via " + source + ": seq=" + std::to_string(seq) +
            ", status=" + formatStatus(status) + ", ts=" + std::to_string(timestamp));
}

static void checkTimeouts(std::map<std::string, DeviceState>& stateMap) {
    auto now = std::chrono::steady_clock::now();
    for (auto& [id, state] : stateMap) {
        auto age = std::chrono::duration_cast<std::chrono::seconds>(now - state.lastSeen).count();
        if (state.online && age > DEVICE_TIMEOUT_SEC) {
            logWarn("Device " + id + " timed out after " + std::to_string(age) + " seconds; last source=" + state.lastSource);
            state.online = false;
            state.tcpConnected = false;
            state.tcpClientFd = INVALID_SOCKET_FD;
        }
    }
}

static void printDeviceSummary(const std::map<std::string, DeviceState>& stateMap) {
    std::cout << "\n-- Device Summary --\n";
    for (const auto& [id, state] : stateMap) {
        auto age = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - state.lastSeen).count();
        std::cout << id << " | last=" << state.lastSource << " | seq=" << state.lastSequence
                  << " | status=" << formatStatus(state.status) << " | age=" << age << "s"
                  << " | online=" << (state.online ? "yes" : "no")
                  << " | tcp=" << (state.tcpConnected ? "yes" : "no") << "\n";
    }
    std::cout << "---------------------\n";
}

int main() {
    try {
        initializeSockets();
        logInfo("Starting device monitoring server");
        socket_t tcpListen = createListeningSocket(SERVER_TCP_PORT);
        socket_t udpSocket = createUdpSocket(SERVER_UDP_PORT);
        logInfo("Listening TCP port " + std::to_string(SERVER_TCP_PORT) + " and UDP port " + std::to_string(SERVER_UDP_PORT));

        std::map<socket_t, TcpClientState> tcpClients;
        std::map<std::string, DeviceState> stateMap;
        std::vector<char> buffer(1024);
        fd_set readSet;

        while (true) {
            int maxFd = std::max(static_cast<int>(tcpListen), static_cast<int>(udpSocket));
            FD_ZERO(&readSet);
            FD_SET(tcpListen, &readSet);
            FD_SET(udpSocket, &readSet);
            for (const auto& [clientFd, _] : tcpClients) {
                FD_SET(clientFd, &readSet);
                int clientFdInt = static_cast<int>(clientFd);
                if (clientFdInt > maxFd) {
                    maxFd = clientFdInt;
                }
            }

            timeval timeout{1, 0};
            int ready = select(maxFd + 1, &readSet, nullptr, nullptr, &timeout);
            if (ready < 0) {
                if (socketInterrupted(getLastSocketError())) {
                    continue;
                }
                throw std::runtime_error("select failed");
            }

            if (FD_ISSET(tcpListen, &readSet)) {
                sockaddr_in clientAddr{};
                socket_len_t addrLen = sizeof(clientAddr);
                socket_t clientFd = accept(tcpListen, reinterpret_cast<sockaddr*>(&clientAddr), &addrLen);
                if (clientFd != INVALID_SOCKET_FD) {
                    if (makeNonBlocking(clientFd) == 0) {
                        TcpClientState clientState;
                        clientState.peer = formatSocketAddress(clientAddr);
                        tcpClients[clientFd] = clientState;
                        logInfo("Accepted TCP client " + clientState.peer + " fd=" +
                                std::to_string(static_cast<long long>(clientFd)));
                    } else {
                        closeSocket(clientFd);
                    }
                }
            }

            if (FD_ISSET(udpSocket, &readSet)) {
                sockaddr_in srcAddr{};
                socket_len_t addrLen = sizeof(srcAddr);
                auto received = recvfrom(udpSocket,
                                         reinterpret_cast<char*>(buffer.data()),
                                         static_cast<int>(buffer.size()),
                                         0,
                                         reinterpret_cast<sockaddr*>(&srcAddr),
                                         &addrLen);
                if (received >= static_cast<int>(sizeof(HeartbeatPacket))) {
                    HeartbeatPacket pkt{};
                    std::memcpy(&pkt, buffer.data(), sizeof(pkt));
                    if (validatePacketHeader(pkt.header) &&
                        (pkt.header.type == PacketType::Heartbeat || pkt.header.type == PacketType::Register)) {
                        processHeartbeatPacket(pkt, formatSocketAddress(srcAddr), stateMap, false);
                    } else {
                        logWarn("Discarded invalid UDP packet from " + formatSocketAddress(srcAddr));
                    }
                } else if (received > 0) {
                    logWarn("Discarded short UDP packet from " + formatSocketAddress(srcAddr));
                }
            }

            std::vector<socket_t> disconnected;
            for (auto& [clientFd, clientState] : tcpClients) {
                if (!FD_ISSET(clientFd, &readSet)) {
                    continue;
                }

                auto received = recv(clientFd, reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), 0);
                if (received <= 0) {
                    logWarn("TCP client " + clientState.peer + " fd=" +
                            std::to_string(static_cast<long long>(clientFd)) + " closed or error");
                    disconnected.push_back(clientFd);
                    continue;
                }

                clientState.pending.insert(clientState.pending.end(), buffer.begin(), buffer.begin() + received);
                while (clientState.pending.size() >= sizeof(HeartbeatPacket)) {
                    HeartbeatPacket pkt{};
                    std::memcpy(&pkt, clientState.pending.data(), sizeof(pkt));
                    clientState.pending.erase(clientState.pending.begin(),
                                              clientState.pending.begin() + static_cast<long long>(sizeof(pkt)));
                    if (!validatePacketHeader(pkt.header)) {
                        logWarn("Received invalid packet from TCP client " + clientState.peer);
                        disconnected.push_back(clientFd);
                        break;
                    }

                    if (pkt.header.type == PacketType::Heartbeat || pkt.header.type == PacketType::Register) {
                        clientState.deviceId = deviceIdToString(pkt.deviceId);
                        processHeartbeatPacket(pkt, clientState.peer, stateMap, true, clientFd);
                        sendAck(clientFd, pkt);
                    }
                }
            }

            for (socket_t fd : disconnected) {
                auto clientIt = tcpClients.find(fd);
                if (clientIt != tcpClients.end() && !clientIt->second.deviceId.empty()) {
                    auto deviceIt = stateMap.find(clientIt->second.deviceId);
                    if (deviceIt != stateMap.end() && deviceIt->second.tcpClientFd == fd) {
                        deviceIt->second.tcpConnected = false;
                        deviceIt->second.tcpClientFd = INVALID_SOCKET_FD;
                        logWarn("Lost TCP session for device " + clientIt->second.deviceId);
                    }
                }
                closeSocket(fd);
                tcpClients.erase(fd);
            }

            checkTimeouts(stateMap);
            static int summaryCounter = 0;
            if (++summaryCounter >= 15) {
                printDeviceSummary(stateMap);
                summaryCounter = 0;
            }
        }
    } catch (const std::exception& ex) {
        logError(ex.what());
        shutdownSockets();
        return 1;
    }

    shutdownSockets();
    return 0;
}
