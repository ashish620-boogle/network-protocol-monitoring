#include "common.h"

#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <netdb.h>
#endif

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

using namespace Monitor;

static socket_t createUdpSocket() {
    socket_t sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == INVALID_SOCKET_FD) throw std::runtime_error("Failed to create UDP socket");
    return sock;
}

static socket_t createTcpSocket() {
    socket_t sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET_FD) throw std::runtime_error("Failed to create TCP socket");
    return sock;
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

static bool recvAllWithTimeout(socket_t fd, void* data, size_t length) {
    auto* bytes = reinterpret_cast<char*>(data);
    size_t totalReceived = 0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(ACK_TIMEOUT_SEC);

    while (totalReceived < length) {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return false;
        }

        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        timeval timeout{};
        timeout.tv_sec = static_cast<long>(remaining.count() / 1000);
        timeout.tv_usec = static_cast<long>((remaining.count() % 1000) * 1000);

        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(fd, &readSet);

        int ready = select(static_cast<int>(fd + 1), &readSet, nullptr, nullptr, &timeout);
        if (ready < 0) {
            if (socketInterrupted(getLastSocketError())) {
                continue;
            }
            return false;
        }
        if (ready == 0) {
            return false;
        }

        auto received = recv(fd, bytes + totalReceived, static_cast<int>(length - totalReceived), 0);
        if (received <= 0) {
            return false;
        }
        totalReceived += static_cast<size_t>(received);
    }
    return true;
}

static bool sendPacketTcp(socket_t tcpFd, const HeartbeatPacket& pkt) {
    return sendAll(tcpFd, &pkt, sizeof(pkt));
}

static bool sendHeartbeatUdp(socket_t udpFd, const sockaddr_in& serverAddr, const HeartbeatPacket& pkt) {
    auto sent = sendto(udpFd,
                       reinterpret_cast<const char*>(&pkt),
                       static_cast<int>(sizeof(pkt)),
                       0,
                       reinterpret_cast<const sockaddr*>(&serverAddr),
                       static_cast<int>(sizeof(serverAddr)));
    return sent == static_cast<int>(sizeof(pkt));
}

static bool waitForAck(socket_t tcpFd, uint32_t expectedSequence, const char* label) {
    HeartbeatPacket ackPacket{};
    if (!recvAllWithTimeout(tcpFd, &ackPacket, sizeof(ackPacket))) {
        logWarn(std::string("Timed out waiting for TCP ACK after ") + label);
        return false;
    }

    if (!validatePacketHeader(ackPacket.header) || ackPacket.header.type != PacketType::Ack) {
        logWarn("Received invalid TCP ACK packet");
        return false;
    }

    uint32_t ackSequence = ntohl(ackPacket.sequence);
    if (ackSequence != expectedSequence) {
        logWarn("Received mismatched TCP ACK sequence " + std::to_string(ackSequence) +
                ", expected " + std::to_string(expectedSequence));
        return false;
    }

    logInfo(std::string("Received TCP ACK for ") + label + " sequence " + std::to_string(ackSequence));
    return true;
}

static bool resolveServerAddress(const std::string& serverHost, uint16_t port, sockaddr_in& outAddr) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* result = nullptr;
    std::string portText = std::to_string(port);
    int rc = getaddrinfo(serverHost.c_str(), portText.c_str(), &hints, &result);
    if (rc != 0 || result == nullptr) {
        return false;
    }

    outAddr = *reinterpret_cast<sockaddr_in*>(result->ai_addr);
    freeaddrinfo(result);
    return true;
}

static bool connectAndRegister(socket_t& tcpFd,
                               const sockaddr_in& serverAddr,
                               const std::string& serverHost,
                               const std::string& deviceId,
                               uint32_t& sequence) {
    tcpFd = createTcpSocket();
    logInfo("Attempting TCP connection to " + serverHost + ":" + std::to_string(SERVER_TCP_PORT));
    if (connect(tcpFd, reinterpret_cast<const sockaddr*>(&serverAddr), sizeof(serverAddr)) < 0) {
        closeSocket(tcpFd);
        tcpFd = INVALID_SOCKET_FD;
        logWarn("TCP connect failed");
        return false;
    }

    logInfo("TCP connection established");

    HeartbeatPacket registerPacket{};
    initHeartbeatPacket(registerPacket, PacketType::Register, deviceId, ++sequence, DeviceStatus::Ok);
    if (!sendPacketTcp(tcpFd, registerPacket)) {
        logWarn("Failed to send TCP register packet");
        closeSocket(tcpFd);
        tcpFd = INVALID_SOCKET_FD;
        return false;
    }

    logInfo("Sent TCP register seq=" + std::to_string(sequence));
    if (!waitForAck(tcpFd, sequence, "register")) {
        closeSocket(tcpFd);
        tcpFd = INVALID_SOCKET_FD;
        return false;
    }

    return true;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <device-id> [server-host]" << std::endl;
        return 1;
    }

    std::string deviceId = argv[1];
    std::string serverHost = argc > 2 ? argv[2] : "127.0.0.1";
    uint32_t sequence = 0;
    initializeSockets();

    sockaddr_in serverAddr{};
    if (!resolveServerAddress(serverHost, SERVER_TCP_PORT, serverAddr)) {
        std::cerr << "Invalid server address: " << serverHost << std::endl;
        shutdownSockets();
        return 1;
    }

    sockaddr_in udpAddr = serverAddr;
    udpAddr.sin_port = htons(SERVER_UDP_PORT);

    socket_t udpFd = INVALID_SOCKET_FD;
    socket_t tcpFd = INVALID_SOCKET_FD;
    try {
        udpFd = createUdpSocket();
        logInfo("UDP heartbeat socket initialized");

        while (true) {
            if (tcpFd == INVALID_SOCKET_FD) {
                connectAndRegister(tcpFd, serverAddr, serverHost, deviceId, sequence);
            }

            HeartbeatPacket pkt{};
            initHeartbeatPacket(pkt, PacketType::Heartbeat, deviceId, ++sequence, DeviceStatus::Ok);

            if (sendHeartbeatUdp(udpFd, udpAddr, pkt)) {
                logInfo("Sent UDP heartbeat seq=" + std::to_string(sequence));
            } else {
                logWarn("Failed to send UDP heartbeat");
            }

            if (tcpFd != INVALID_SOCKET_FD) {
                if (!sendPacketTcp(tcpFd, pkt)) {
                    logWarn("Failed to send TCP heartbeat");
                    closeSocket(tcpFd);
                    tcpFd = INVALID_SOCKET_FD;
                } else {
                    logInfo("Sent TCP heartbeat seq=" + std::to_string(sequence));
                    if (!waitForAck(tcpFd, sequence, "heartbeat")) {
                        logWarn("TCP session unhealthy, reconnecting");
                        closeSocket(tcpFd);
                        tcpFd = INVALID_SOCKET_FD;
                    }
                }
            } else {
                logWarn("TCP unavailable; continuing with UDP heartbeat only");
            }

            std::this_thread::sleep_for(std::chrono::seconds(HEARTBEAT_INTERVAL_SEC));
        }
    } catch (const std::exception& ex) {
        logError(ex.what());
        if (tcpFd != INVALID_SOCKET_FD) {
            closeSocket(tcpFd);
        }
        if (udpFd != INVALID_SOCKET_FD) {
            closeSocket(udpFd);
        }
        shutdownSockets();
        return 1;
    }

    if (tcpFd != INVALID_SOCKET_FD) {
        closeSocket(tcpFd);
    }
    if (udpFd != INVALID_SOCKET_FD) {
        closeSocket(udpFd);
    }
    shutdownSockets();
    return 0;
}
