#include "ipc_client.h"
#include "common/ipc/ipc_protocol.h"
#include "common/ipc/ipc_serialization.h"
#include <cstring>
#include <iostream>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace smsrelay::cli {

using namespace smsrelay::ipc;

IpcClient::IpcClient(const std::string &host, int port)
    : host_(host), port_(port)
{
#ifdef _WIN32
    sock_ = INVALID_SOCKET;
    wsa_initialized_ = false;
#else
    sock_ = -1;
#endif
}

IpcClient::~IpcClient() { disconnect(); }

bool IpcClient::connect()
{
    if (connected_)
    {
        return true;
    }

#ifdef _WIN32
    // Initialize Winsock
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0)
    {
        std::cerr << "[IPC Client] WSAStartup failed" << std::endl;
        return false;
    }
    wsa_initialized_ = true;
#endif

    // Use getaddrinfo to support hostnames, IPv4, and IPv6
    struct addrinfo hints, *result, *rp;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;     // Allow IPv4 or IPv6
    hints.ai_socktype = SOCK_STREAM; // TCP socket

    // Convert port to string
    std::string port_str = std::to_string(port_);

    // Get address information
    int ret = getaddrinfo(host_.c_str(), port_str.c_str(), &hints, &result);
    if (ret != 0)
    {
#ifdef _WIN32
        std::cerr << "[IPC Client] getaddrinfo failed: " << ret << std::endl;
        WSACleanup();
        wsa_initialized_ = false;
#else
        std::cerr << "[IPC Client] getaddrinfo failed: " << gai_strerror(ret)
                  << std::endl;
#endif
        return false;
    }

    // Try each address until connection succeeds
    bool connected = false;
    for (rp = result; rp != nullptr; rp = rp->ai_next)
    {
        // Create socket
#ifdef _WIN32
        sock_ = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sock_ == INVALID_SOCKET)
        {
            continue;
        }
#else
        sock_ = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sock_ < 0)
        {
            continue;
        }
#endif

        // Try to connect
        if (::connect(sock_, rp->ai_addr, static_cast<int>(rp->ai_addrlen)) == 0)
        {
            connected = true;
            break;
        }

        // Connection failed, close socket and try next address
#ifdef _WIN32
        closesocket(sock_);
        sock_ = INVALID_SOCKET;
#else
        close(sock_);
        sock_ = -1;
#endif
    }

    freeaddrinfo(result);

    if (!connected)
    {
        std::cerr << "[IPC Client] Failed to connect to " << host_ << ":" << port_
                  << std::endl;
#ifdef _WIN32
        if (wsa_initialized_)
        {
            WSACleanup();
            wsa_initialized_ = false;
        }
#endif
        return false;
    }

    connected_ = true;

    // Show connection info (use original host name, not resolved IP)
    std::cout << "[IPC Client] Connected to " << host_ << ":" << port_
              << std::endl;

    return true;
}

void IpcClient::disconnect()
{
    if (!connected_)
    {
        return;
    }

    connected_ = false;

#ifdef _WIN32
    if (sock_ != INVALID_SOCKET)
    {
        closesocket(sock_);
        sock_ = INVALID_SOCKET;
    }
    if (wsa_initialized_)
    {
        WSACleanup();
        wsa_initialized_ = false;
    }
#else
    if (sock_ >= 0)
    {
        close(sock_);
        sock_ = -1;
    }
#endif
}

std::vector<uint8_t>
IpcClient::send_command(const std::vector<uint8_t> &request)
{
    if (!connected_)
    {
        std::cerr << "[IPC Client] Not connected" << std::endl;
        return {};
    }

    // Send request
#ifdef _WIN32
    int sent = send(sock_, reinterpret_cast<const char *>(request.data()),
                    static_cast<int>(request.size()), 0);
    if (sent <= 0)
    {
        std::cerr << "[IPC Client] Send failed" << std::endl;
        return {};
    }
#else
    ssize_t sent = send(sock_, request.data(), request.size(), 0);
    if (sent <= 0)
    {
        std::cerr << "[IPC Client] Send failed" << std::endl;
        return {};
    }
#endif

    // Receive response header (16 bytes)
    uint8_t header_buf[16];
    size_t header_received = 0;

    while (header_received < 16)
    {
#ifdef _WIN32
        int received =
            recv(sock_, reinterpret_cast<char *>(header_buf + header_received),
                 static_cast<int>(16 - header_received), 0);
#else
        ssize_t received =
            recv(sock_, header_buf + header_received, 16 - header_received, 0);
#endif
        if (received <= 0)
        {
            std::cerr << "[IPC Client] Receive header failed" << std::endl;
            return {};
        }
        header_received += received;
    }

    // Parse header to get payload length
    size_t offset = 0;
    uint32_t magic, length, status, sequence;

    if (!IpcSerializer::deserialize_u32(header_buf, 16, magic, offset))
        return {};
    if (!IpcSerializer::deserialize_u32(header_buf, 16, length, offset))
        return {};
    if (!IpcSerializer::deserialize_u32(header_buf, 16, status, offset))
        return {};
    if (!IpcSerializer::deserialize_u32(header_buf, 16, sequence, offset))
        return {};

    // Verify magic
    if (magic != IPC_MAGIC)
    {
        std::cerr << "[IPC Client] Invalid magic number in response" << std::endl;
        return {};
    }

    // Receive payload if any
    std::vector<uint8_t> response;
    response.insert(response.end(), header_buf, header_buf + 16);

    if (length > 0)
    {
        std::vector<uint8_t> payload(length);
        size_t payload_received = 0;

        while (payload_received < length)
        {
#ifdef _WIN32
            int received = recv(
                sock_, reinterpret_cast<char *>(payload.data() + payload_received),
                static_cast<int>(length - payload_received), 0);
#else
            ssize_t received = recv(sock_, payload.data() + payload_received,
                                    length - payload_received, 0);
#endif
            if (received <= 0)
            {
                std::cerr << "[IPC Client] Receive payload failed" << std::endl;
                return {};
            }
            payload_received += received;
        }

        response.insert(response.end(), payload.begin(), payload.end());
    }

    return response;
}

} // namespace smsrelay::cli
