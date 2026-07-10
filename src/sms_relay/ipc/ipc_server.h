#pragma once

#include "sms_relay/sms/sms_service.h"
#include <atomic>
#include <memory>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace smsrelay::ipc {

/**
 * @brief TCP-based IPC Server for handling CLI commands
 *
 * Runs in the sms_relay service process
 * Uses TCP loopback (127.0.0.1) for local communication
 */
class IpcServer
{
public:
    /**
     * @brief Constructor
     * @param port TCP port number to listen on
     * @param sms_service SMS service instance
     */
    IpcServer(int port, std::shared_ptr<SmsService> sms_service);
    ~IpcServer();

    // Delete copy constructor and assignment operator
    IpcServer(const IpcServer &) = delete;
    IpcServer &operator=(const IpcServer &) = delete;

    /**
     * @brief Start the IPC server
     * @return true if started successfully
     */
    bool start();

    /**
     * @brief Stop the IPC server
     */
    void stop();

    /**
     * @brief Check if server is running
     */
    bool is_running() const { return running_.load(); }

private:
    /**
     * @brief Accept client connections (main loop)
     */
    void accept_loop();

    /**
     * @brief Handle a single client connection
     * @param client_fd Client socket file descriptor
     */
    void handle_client(int client_fd);

    /**
     * @brief Process command and return response
     * @param request Request data
     * @return Response data
     */
    std::vector<uint8_t> process_command(const std::vector<uint8_t> &request);

    /**
     * @brief List messages handler
     */
    std::vector<uint8_t> handle_list(const std::vector<uint8_t> &payload);

    /**
     * @brief Read messages handler
     */
    std::vector<uint8_t> handle_read(const std::vector<uint8_t> &payload);

    /**
     * @brief Delete messages handler
     */
    std::vector<uint8_t> handle_delete(const std::vector<uint8_t> &payload);

    /**
     * @brief Send message handler
     */
    std::vector<uint8_t> handle_send(const std::vector<uint8_t> &payload);

    /**
     * @brief Status handler
     */
    std::vector<uint8_t> handle_status(const std::vector<uint8_t> &payload);

    int port_;
    std::shared_ptr<SmsService> sms_service_;
    std::atomic<bool> running_{false};
    std::thread server_thread_;

#ifdef _WIN32
    SOCKET server_fd_{INVALID_SOCKET};
#else
    int server_fd_{-1};
#endif

    static constexpr uint32_t RECV_BUFFER_SIZE = 4096;
    static constexpr uint32_t MAX_PAYLOAD_SIZE = 1024 * 1024; // 1MB max
};

} // namespace smsrelay::ipc
