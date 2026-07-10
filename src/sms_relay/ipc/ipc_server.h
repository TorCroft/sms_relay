#pragma once

#include "common/app_config.h"
#include "sms_relay/sms/sms_service.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

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
 * @brief Client connection information
 */
struct ClientConnection
{
#ifdef _WIN32
    SOCKET socket_fd;
#else
    int socket_fd;
#endif
    std::string address;
    std::thread handler_thread;
    bool active;

    ClientConnection() : active(false)
    {
#ifdef _WIN32
        socket_fd = INVALID_SOCKET;
#else
        socket_fd = -1;
#endif
    }
};

/**
 * @brief TCP-based IPC Server for handling CLI commands
 *
 * Runs in the sms_relay service process
 * Supports multiple concurrent client connections (max: MAX_CLIENT_CONNECTIONS)
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

    /**
     * @brief Get current number of active client connections
     */
    int get_active_connections() const;

private:
    /**
     * @brief Accept client connections (main loop)
     */
    void accept_loop();

    /**
     * @brief Handle a single client connection (runs in separate thread)
     * @param client_idx Index in clients_ vector
     */
    void handle_client_thread(int client_idx);

    /**
     * @brief Remove a client connection
     * @param client_idx Index in clients_ vector
     */
    void remove_client(int client_idx);

    /**
     * @brief Find an available client slot
     * @return Index of available slot, or -1 if none available
     */
    int find_available_client_slot();

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

    // Client connection management
    std::vector<ClientConnection> clients_;
    std::mutex clients_mutex_;
    static constexpr int MAX_CLIENTS = IPCServerDefaults::MAX_CLIENT_CONNECTIONS;

#ifdef _WIN32
    SOCKET server_fd_{INVALID_SOCKET};
#else
    int server_fd_{-1};
#endif

    static constexpr uint32_t RECV_BUFFER_SIZE = 4096;
    static constexpr uint32_t MAX_PAYLOAD_SIZE = 1024 * 1024; // 1MB max
};

} // namespace smsrelay::ipc
