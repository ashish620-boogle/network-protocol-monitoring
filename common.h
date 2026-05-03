#pragma once

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#ifdef _MSC_VER
#pragma comment(lib, "Ws2_32.lib")
#endif
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

namespace Monitor {
#ifdef _WIN32
using socket_t = SOCKET;
using socket_len_t = int;
static constexpr socket_t INVALID_SOCKET_FD = INVALID_SOCKET;
inline int closeSocket(socket_t fd) { return closesocket(fd); }
inline int setNonBlocking(socket_t fd) {
    u_long mode = 1;
    return ioctlsocket(fd, FIONBIO, &mode);
}
inline void initializeSockets() {
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
}
inline void shutdownSockets() {
    WSACleanup();
}
inline int getLastSocketError() { return WSAGetLastError(); }
inline bool socketWouldBlock(int errorCode) { return errorCode == WSAEWOULDBLOCK; }
inline bool socketInterrupted(int errorCode) { return errorCode == WSAEINTR; }
#else
using socket_t = int;
using socket_len_t = socklen_t;
static constexpr socket_t INVALID_SOCKET_FD = -1;
inline int closeSocket(socket_t fd) { return close(fd); }
inline int setNonBlocking(socket_t fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}
inline void initializeSockets() {}
inline void shutdownSockets() {}
inline int getLastSocketError() { return errno; }
inline bool socketWouldBlock(int errorCode) { return errorCode == EWOULDBLOCK || errorCode == EAGAIN; }
inline bool socketInterrupted(int errorCode) { return errorCode == EINTR; }
#endif

constexpr uint8_t PROTOCOL_VERSION = 1;
constexpr uint16_t PACKET_MAGIC = 0x4D44;
constexpr uint16_t SERVER_TCP_PORT = 5000;
constexpr uint16_t SERVER_UDP_PORT = 5001;
constexpr size_t DEVICE_ID_LENGTH = 16;
constexpr int HEARTBEAT_INTERVAL_SEC = 5;
constexpr int DEVICE_TIMEOUT_SEC = 15;
constexpr int ACK_TIMEOUT_SEC = 2;

#ifdef MSG_NOSIGNAL
constexpr int SOCKET_SEND_FLAGS = MSG_NOSIGNAL;
#else
constexpr int SOCKET_SEND_FLAGS = 0;
#endif

enum class PacketType : uint8_t {
    Register = 1,
    Heartbeat = 2,
    Ack = 3,
};

enum class DeviceStatus : uint8_t {
    Ok = 1,
    Warning = 2,
    Error = 3,
};

#pragma pack(push, 1)
struct PacketHeader {
    uint8_t version;
    PacketType type;
    uint16_t magic;
};

struct HeartbeatPacket {
    PacketHeader header;
    uint32_t sequence;
    uint64_t timestamp;
    char deviceId[DEVICE_ID_LENGTH];
    DeviceStatus status;
    uint8_t padding[7];
};
#pragma pack(pop)

static_assert(sizeof(PacketHeader) == 4, "PacketHeader must be 4 bytes");
static_assert(sizeof(HeartbeatPacket) == 40, "HeartbeatPacket must be 40 bytes");

inline uint64_t hostToNetwork64(uint64_t value) {
    uint64_t high = htonl(static_cast<uint32_t>(value >> 32));
    uint64_t low = htonl(static_cast<uint32_t>(value & 0xffffffffull));
    return (low << 32) | high;
}

inline uint64_t networkToHost64(uint64_t value) {
    uint64_t high = ntohl(static_cast<uint32_t>(value >> 32));
    uint64_t low = ntohl(static_cast<uint32_t>(value & 0xffffffffull));
    return (low << 32) | high;
}

inline std::string formatTimestamp() {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto secs = system_clock::to_time_t(now);
    auto micros = duration_cast<microseconds>(now.time_since_epoch()) % 1000000;

    std::ostringstream oss;
    oss << std::put_time(std::localtime(&secs), "%Y-%m-%d %H:%M:%S")
        << "." << std::setw(6) << std::setfill('0') << micros.count();
    return oss.str();
}

inline void logMessage(const char* level, const std::string& message) {
    std::cerr << "[" << formatTimestamp() << "] [" << level << "] " << message << "\n";
}

inline void logInfo(const std::string& message) { logMessage("INFO", message); }
inline void logWarn(const std::string& message) { logMessage("WARN", message); }
inline void logError(const std::string& message) { logMessage("ERROR", message); }

inline void initHeartbeatPacket(HeartbeatPacket& pkt, PacketType type, const std::string& deviceId, uint32_t sequence, DeviceStatus status) {
    pkt.header.version = PROTOCOL_VERSION;
    pkt.header.type = type;
    pkt.header.magic = htons(PACKET_MAGIC);
    pkt.sequence = htonl(sequence);
    pkt.timestamp = hostToNetwork64(static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count()));
    std::memset(pkt.deviceId, 0, sizeof(pkt.deviceId));
    std::strncpy(pkt.deviceId, deviceId.c_str(), sizeof(pkt.deviceId) - 1);
    pkt.status = status;
    std::memset(pkt.padding, 0, sizeof(pkt.padding));
}

inline bool validatePacketHeader(const PacketHeader& header) {
    return header.version == PROTOCOL_VERSION &&
        ntohs(header.magic) == PACKET_MAGIC &&
        (header.type == PacketType::Register ||
         header.type == PacketType::Heartbeat ||
         header.type == PacketType::Ack);
}

inline std::string deviceIdToString(const char* buffer) {
    return std::string(buffer, strnlen(buffer, DEVICE_ID_LENGTH));
}

inline std::string formatStatus(DeviceStatus status) {
    switch (status) {
        case DeviceStatus::Ok: return "OK";
        case DeviceStatus::Warning: return "WARNING";
        case DeviceStatus::Error: return "ERROR";
    }
    return "UNKNOWN";
}

} // namespace Monitor
