#pragma once

#include <string>
#include <vector>
#include <cstdint>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#endif

namespace smsrelay::cli {

/**
 * @brief TCP-based IPC Client for communicating with sms_relay service
 *
 * Supports both IPv4 and IPv6 connections
 */
class IpcClient {
public:
    /**
     * @brief Constructor
     * @param host Server hostname or IP (default: "::1" for IPv6 loopback)
     * @param port TCP port number
     */
    IpcClient(const std::string& host = "::1", int port = 7896);

    ~IpcClient();

    // Delete copy constructor and assignment operator
    IpcClient(const IpcClient&) = delete;
    IpcClient& operator=(const IpcClient&) = delete;

    /**
     * @brief Connect to server
     * @return true if connected successfully
     */
    bool connect();

    /**
     * @brief Disconnect from server
     */
    void disconnect();

    /**
     * @brief Check if connected
     */
    bool is_connected() const { return connected_; }

    /**
     * @brief Send command and receive response
     * @param request Request data
     * @return Response data
     */
    std::vector<uint8_t> send_command(const std::vector<uint8_t>& request);

private:
    std::string host_;
    int port_;
    bool connected_{false};

#ifdef _WIN32
    SOCKET sock_{INVALID_SOCKET};
    bool wsa_initialized_{false};
#else
    int sock_{-1};
#endif
};

} // namespace smsrelay::cli
